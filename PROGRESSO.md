# PROGRESSO — EdenMaliIA

Motor de execução **nativa** (sem JIT) de ROMs de Nintendo Switch para o
Orange Pi 5 Pro (RK3588 / Mali-G610).

Objectivo intermédio: **a thread principal passa pelo crt0**. Serviços do
Horizon, nvdrv e GPU estão deliberadamente **fora** até lá chegar.

---

## Como se mede

Um sintoma nunca é aceite sem uma medição. Ordem de custo, do mais barato ao
mais caro:

1. Ler o binário (`objdump -b binary -m aarch64`, `strings`, Python).
2. Descompilar com o script em `/home/trebor/opi5-extract/nso.py` (NSO cru →
   segmentos descomprimidos), **sem** depender do Pi.
3. Instrumentar o motor (`ia_boot --raw`, dumps de blocos de memória).
4. Só depois mexer em código.

---

## Regra que os bugs ensinaram

Nove bugs came a mesma regra: **todo o valor de que o motor depende tem de
vir da fonte ou de uma medição**. Nunca de memória, nunca de "deve ser".

Quando uma hipótese é refutada, escreve-se. A lista está em
`docs/ARQUITETURA.md`.

---

## Estado

| Marco | Estado |
|---|---|
| Loader de NSO (segmentos LZ4) | feito |
| NPDM 64-bit, ProcessAddress=3 | feito |
| Patcher SVC→BRK, HINT→NOP | feito |
| Motor de execução nativa (traps) | feito, validado |
| Telemetria por frame e por serviço | feito |
| Descoberta de módulos (MOD0 + QueryMemory) | feito |
| ThreadLocalRegion da thread principal | feito |
| Patch de `MRS TPIDRRO_EL0` | feito |
| **Passar o crt0** | **feito** |
| Serviços / nvdrv / GPU | **bloqueio declarado** |

---

## Onde paramos

O guest passou o crt0, pediu a porta **`sm:`**, e está a falar SMART IPC.
Construí­mos o transporte e despachamos o primeiro comando — `sm: Initialize`
— mas o guest continua a chamar `svc Break(x0=0)` a seguir.

**164 verificações, 0 falhas.**

### O transporte (`src/ipc_min.cpp`)

Medido no pedido real do jogo, e conferido contra `core/hle/ipc.h` do Eden:

```
0x00  CommandHeader        type=6 (RequestWithContext), data_size=10
0x08  BufferDescriptorC    8 bytes
0x10  HandleDescriptorHeader
0x20  DataPayloadHeader    "SFCI"
0x28  corpo: cmd_id = 0   -> Initialize
```

E a resposta, de `ResponseBuilder` (`ipc_helpers.h:36-95`):

- **magic = `"SFCO"`**, não `"SFCI"` — I/O, input/output (`ipc_helpers.h:88`)
- `CommandHeader.type = 0` — o `type` só é escrito no caminho TIPC
- `data_size = normal_params + 2 + 4` (os 4 u32 são 16 bytes de padding)

Os comandos do `sm:` e os seus números são os do Eden (`sm.cpp:270-278`):
`0 Initialize`, `1 GetService`, `2 RegisterService`, `3 UnregisterService`.

### A geometria do payload NÃO é uma constante

Duas tentativas com offsets fixos (0x28, depois 0x18) falharam as duas, e o
motivo é que **o payload não está sempre no mesmo sítio**:

```
pedido 1 (GetService, nome vazio):  payload em 0x20
pedido 2 (GetService "lm")        :  payload em 0x10
```

A diferença vem dos descritores que cada pedido traz. Em vez de reproduzir a
aritmética — que tem três casos e um alinhamento de 16 bytes, e que é
perfeitamente capaz de estar errada — **procura-se a assinatura `SFCI`**. É
mais barato, é à prova de um descritor que não conhecemos, e se aparecer uma
geometria nova o código continua a funcionar em vez de escrever por cima do
sítio errado.

E assim discovered-se que **`cmd_id` está em `magic + 4`** e **o nome do
serviço em `magic + 0x10`**. Uma versão anterior lia o `cmd_id` de 0x28 e
achava que o primeiro pedido era `Initialize`; era `GetService`, e o
`Initialize` nunca chegou a ser pedido.

### Experimento em matriz: a resposta NÃO é a causa

Em vez de escolher mais um palpite, testaram-se **todas as combinações** de
geometria da resposta. `IA_IPC_VARIANT` é uma bitmask:

| bit | o que muda |
|---|---|
| 0 | handle no descritor de handles (+ `enable_handle_descriptor`) em vez de inline |
| 1 | repetir o `type` do pedido em vez de `type = 0` |
| 2 | pôr o payload 0x10 mais acima |
| 3 | **não responder** — o controlo |

Resultado das 16 variantes:

```
v=0..15   sync=1   svc=32   break=2
```

**Todas as dezasseis dão exactamente o mesmo.** Incluindo as quatro em que
**não escrevemos nada no buffer**. Se o conteúdo da resposta fosse a causa,
uma delas teria de ser diferente.

Conclusão, e é uma conclus��ão Negative que vale muito: **o guest não depende
do que escrevemos no buffer de IPC.** O problema está antes da resposta, ou
noutro sítio.

### O que se aprendeu pelo caminho

**O `Break(x0)` é um abort deliberado**, não um crash:

```
0x82b7b1d8  mov  x1, x0            ; x1 = o endereço culpado
0x82b7b1dc  orr  w2, wzr, #0x4
0x82b7b1e0  mov  w0, wzr
0x82b7b1e4  bl   0x82d79ba4        ; svc Break
0x82b7b1e8  b    .                 ; e fica preso
```

E o wrapper do `SendSyncRequest` confirma que o Result é lido:

```
0x82d57c44  and  w9, w0, #0x3fffff
0x82d57c50  cmp  w9, #0xf601
0x82d57c5c  csel x8, x9, x8, eq    ; traduz Result inválido -> codigo interno
0x82d57c60  cmp  w0, #0x0
0x82d57c64  csel x0, xzr, x8, eq   ; 0 se sucesso
```

Logo, o SDK le o nosso Result=0 e continua. O abort vem **depois**.

**O TLS funciona a sério.** O guest reescreveu o nosso `tls_tp`:

```
antes (nosso):  tls_tp = 0x88100010
depois (dele):  tls_tp = 0x836a0bc0
```

O SDK montou o seu próprio bloco de TLS estático, dentro do espaço do guest.
Isto é o que a convenção do `ThreadLocalRegion` predict — e é a primeira vez
que vemos o guest a **escrever** lá em vez de o ler.

### Onde o abort vem de dentro

Na stack, no momento do `Break`:

```
sp+  0  0x847ffdd0        (x29 do helper)
sp+  8  0x82b79cc0        <- regresso a partir do helper do Break
sp+448  0x88000010        <- o NOSSO ThreadLocalRegion + 0x10
```

A instrumentação de stack foi acrescentada ao tracer por causa disto: um
`Break(x0=0)` diz **que** valor era zero; a stack diz **quem** o tinha.
### O que NÃO está resolvido

O `Break(x0=0)` continua, e a tentativa 5 diz que **não é a resposta**. As
hipóteses que sobram, por ordem deuche:

1. **O handle devolvido não vai onde o guest o lê.** Para `GetService`, o Eden
   usa `ResponseBuilder{ctx, 2, 0, 1, AlwaysMoveHandles}` — o handle é
   MOVIDO, e handles movidos vão no descritor de handles
   (`enable_handle_descriptor`), não na secção de dados. A nossa resposta tem
   o handle inline e `enable_handle_descriptor = 0`. **Esta é a hipótese
   mais forte e é a próxima a testar.**
2. O `cmd_id` estar em `magic + 4` é medido, mas os 8 bytes entre o `cmd_id` e
   o nome não foram identificados — podem ser um campo que o guest preenche e
   que o servidor devolve.
3. Pode faltar o `svcReplyAndReceive` / deferral: o Eden, quando um serviço
   não está pronto, faz `ctx.SetIsDeferred()` e o kernel continua a thread
   depois. Nós respondemos sempre de imediato.

Paro aqui pela regra das três tentativas — e a tentativa 5 deu-nos o
diagnóstico que faltava para escolher bem a próxima.

---

## As SVCs que o guest pediu no arranque (medido, por ordem)

| # | SVC | x1 | Para quê |
|---|---|---|---|
| 1–12 | `QueryMemory` (0x06) | — | a varrura de módulos do rtld |
| 13,14 | `GetInfo` (0x29) | 0x0b | `RandomEntropy`, duas vezes |
| 15,16 | `GetInfo` | 0x0c | `AslrRegionAddress` |
| 17 | `GetInfo` | 0x0d | `AslrRegionSize` |
| 18,19 | `GetInfo` | 0x0e | `StackRegionAddress` |
| 20 | `GetInfo` | 0x0f | `StackRegionSize` |
| 21 | `QueryMemory` | — | a própria stack do guest |
| 22 | `GetThreadPriority` (0x0c) | — | prioridade da thread principal |
| 23 | `GetThreadId` (0x25) | — | id de thread |
| 24 | `GetInfo` | 0x02 | `AliasRegionAddress` |
| 25 | `GetInfo` | 0x03 | `AliasRegionSize` |
| 26 | `GetInfo` | 0x16 | `SystemResourceSizeTotal` |
| 27 | `GetInfo` | 0x15 | `StackRegionSize` |
| 28 | `QueryMemory` | — | a região de código |
| 29 | `ConnectToNamedPort` (0x1f) | — | **`"sm:"`** |
| 30 | `SendSyncRequest` (0x21) | — | o pedido em si |

Nenhuma delas é `nvdrv`, GPU ou tela. O jogo ainda não tocou nagraficos.

---

## Diário

### 1. Boot chain: o rtld falha a resolver símbolos

**Medido.** O rtld imprime `[rtld] Unresolved symbol: '_ZN2nn4init5StartEmmPFvvES2_'`.

**Hipótese inicial (errada).** Faltava um bloco de módulos para o rtld
receber. Nove dias dechristo a procurar um `NsModInfo` que não existe.

**Descoberta.** Descompilei o binário do rtld. Não há lista de módulos: o
rtld **varre a memória** com `svcQueryMemory` e aceita uma região só se
`permission == 5` (R-X) e `state == 3` (Code). Depois exige um cabeçalho
`MOD0`. O `MOD0` já está dentro de cada NSO, posto pelo linker da Nintendo.

**Confirmado em:** switchbrew `/wiki/MOD`, `/wiki/Rtld`, e nas imagens
descomprimidas do `main` (vaddr 0x08).

### 2. Três bugs escondidos atrás do mesmo sintoma

| # | Bug | Como apareceu |
|---|---|---|
| 9º | `svcQueryMemory` devolvia a reserva inteira como `Normal`/`RWX` | o rtld não aceitava nenhuma região |
| 10º | `plan_layout` ignorava o `.bss` | o `.bss` do `main` escrevia por cima da `sdk` |
| 11º | A tabela de regiões tinha buracos | o rtld pára no primeiro endereço não descrito |

**Resultado:** `Unresolved symbol` desapareceu. O rtld carrega os três
módulos e o SDK arranca.

### 3. O guest lê o seu TLS de `TPIDRRO_EL0`

```asm
mrs  x8, tpidrro_el0
ldr  w9, [x0]                ; spill
ldr  x8, [x8, #0x1f8]        ; <-- fault
```

Como o guest corre **nativo**, esse registo é o `fs` da thread do Linux.

**Medido, quatro vias:**

| via | resultado |
|---|---|
| `mrs TPIDRRO_EL0` | `0x0`, **sempre** |
| `msr TPIDRRO_EL0` | **SIGILL** |
| `arch_prctl(ARCH_SET_FS)` | `-1`, errno **22 (EINVAL)** — não é permissão |
| `msr TPIDR_EL0` | **funciona** |
| `mrs TPIDR_EL0` | `0xffff9c84a620` — TLS real da libc |

E o resultado decisivo:

```
msr tpidr_el0, 0x88000000
mrs tpidr_el0     -> 0x88000000   ✓
mrs tpidrro_el0   -> 0x0          ✗
```

**Não são o mesmo registo neste kernel.** Trocar o TPIDR_EL0 não alimenta o
TPIDRRO.

### 4. Porquê `0x1F8` (e não `0x1F0`)

O switchbrew (`/wiki/Thread_Local_Storage`, `nn::svc::ThreadLocalRegion`) diz
`ThreadPointer` em **+0x1F0** e `ThreadType` em **+0x1F8**. O libnx
(`internal.h`) diz que `tls_tp` tem de estar em **+0x1F8**.

**Medido no código do jogo, e o jogo ganha:** `sdk+0x32f840` é
`mrs x8, tpidrro_el0; ldr x8, [x8, #0x1F8]; ldur w8, [x8, #4]` — a sequência
do compilador AArch64 para ler uma variável `__thread`. **0x1F8.**

### 5. Quem escreve o `tls_tp` — o guest não é

`libnx/nx/source/kernel/thread.c`:

```c
static void _EntryWrap(...) {          // threadCreate — GUEST
    tv->magic = THREADVARS_MAGIC;  tv->tls_tp = args->tls - getTlsStartOffset();
}
void __libnx_init_thread(void) {       // thread PRINCIPAL
    getThreadVars()->thread_ptr = &g_mainThread;   // <-- só isto
}
```

A thread principal não passa por `_EntryWrap`. O libnx escreve-lhe um único
campo, o que só faz sentido se `magic`, `reent` e `tls_tp` já lá estiverem —
e o único que os pode por antes de o guest arrancar é **o kernel**.

**Confirmado pelo nosso lado:** quando o SDK falha, `tls_tp` ainda está a
zero, e o SDK já tinha corrido meia-PAGE de código.

**Preenchido por nós** (`GuestProgram::init_main_thread_tls`):

```
magic=0x21545624  handle=0x1  obj=0x88200000  reent=0x88201000  tls_tp=0x88100010
```

**Hipótese declarada, não facto:** `getTlsStartOffset()` do libnx devolve
`max(__tls_align, 16)`. Não sabemos `__tls_align` do SDK da Nintendo; usamos
o mínimo garantido, 16 (o TCB). A confirmar pelo próximo fault.

### 6. Decisão: patch de um bit

**Pergunta posta pelo utilizador:** um literal fixo só serve uma thread, e
`LDR literal` só alcança ±1 MB — os 50 `mrs` do sdk estão por 5 MB.

**Resposta:** as duas objecções são válidas e tornam o literal a pior das
três opções. A resposta certa não é um pool de literais: é usar o **registo
de hardware**, que já é por thread.

Os dois registos diferem por **um bit** (confirmado com o assembler):

```
mrs x0, tpidrro_el0 ->  d53bd060
mrs x5, tpidr_el0    ->  d53bd045
              bit 6:  1
```

Patch: `d53bd068` → `d53bd048`. **Mesmos 4 bytes.** Sem relocação, sem pool,
sem limite de alcance. E depois troca-se o `TPIDR_EL0` à volta da execução do
guest — que é por thread de hardware, logo dá a multi-thread de graça.

**Risco medido:** com o `TPIDR_EL0` a apontar para o bloco do guest,
`snprintf` dá segfault. O handler de sinais tem de repor o `TPIDR_EL0` do
host à entrada e reinstalar o do guest à saída.

**Erro meu, registado:** é o desenho que eu tinha escrito e removi. Usei
`TPIDRRO` quando devia ter usado `TPIDR_EL0`, e obtive SIGILL em vez de
sucesso.

### 7. SVCs do arranque

Encadeado, um obstáculo por vez, cada um medido antes do seguinte.

**`GetThreadPriority` (0x0C)** — devolve a prioridade do NPDM. A forma: o valor
volta em **x1**, no registo que era o handle (`SvcWrap_GetThreadPriority64`,
eden svc.cpp:1331).

**`GetInfo` (0x29)** — reescrita com o enum **real** do Eden
(`svc_types.h:130-159`).

> **Bug meu, e o pior dos dois tipos.** A primeira versão tinha
> `case 16: value = 4; // PersonalWordSize`. **`PersonalWordSize` não existe**
> — nem no Eden, nem no switchbrew, nem na Terra. 16 é
> `SystemResourceSizeTotal` e 22 é `UsedNonSystemMemorySize`. O SDK pedia o 22
> e recebia **quatro bytes de "memória em uso"**, e depois desreferenciava o
> resultado. Um número inventado não dá erro nenhum: só dá segfault mais
> tarde, e o culpado parece ser o guest.

**`GetProcessId` / `GetThreadId`** — duas formas erradas ao mesmo tempo:
devolviam o id em **x0** (que é o Result), e o `GetProcessId` *lia* oito
bytes do guest em vez de escrever o valor. Agora em x1, e fixos — o SDK usa-os
como chave do estado por-thread.

**`ConnectToNamedPort` (0x1F)** — a assinatura **não tem ponteiro de saída nem
comprimento**. `x1` é o apontador para o nome, terminado em NUL, e o handle
volta no **mesmo registo** (eden svc.cpp:1470). A versão anterior lia o
comprimento de `x2` — que vem sempre a zero — e escrevia o handle num
ponteiro em x0. Eram dois erros que se cancelavam por acidente num teste com
ponteiros do host.

**Região de alias** — `GetInfo(2/3)` devolvia 0, e o SDK aloca dali: null
dereference. Mapeámos [0x6000'0000, 0x8000'0000).

> O tamanho **não é redondo de propósito**: termina exactamente em
> `kCodeBase`. Com 256 MiB ficou um buraco, o rtld caiu lá durante a varredura
> de módulos e o jogo deixou de arrancar — uma mudança que parecia não ter
> nada a ver com a mudança que a fez.

---

## Não confirmado (não inventar)

- `__tls_align` do SDK oficial → usamos 16 como mínimo.
- `Handle` válido para a thread principal → usamos `1`.
- `InfoType::ProgramId` (18) → **devolvemos erro de propósito**. Não está no
  NPDM (o meta tem `ProductCode` a zero — switchbrew/NPDM 0x30); vem do title
  id do NCA, que se perdeu ao extrair o ExeFS. Só é usado para dados de save.
- `InfoType::ResourceLimit` (9) → idem, não implementado.
- Os valores de `SystemResourceSizeTotal` e `UsedNonSystemMemorySize` são
  plausíveis, **não medidos contra hardware real**.
- Como o kernel do Switch dimensiona o bloco estático de TLS da thread
  principal a partir do NPDM → **não investigado**.
---

# GPU: Maxwell -> Valhall (investigado a pedido)

## O que está medido no Orange Pi 5 Pro

```
GPU0: deviceName  = Mali-G610
      driverName  = panvk   (Mesa 25.2.8)
      apiVersion  = 1.4.318
```

PanVK activo, Vulkan 1.4, mais GBM e EGL/GLES2. **O lado destino existe e
funciona.** Limites da G610 relevantes para degradar recursos:
`maxImageDimension2D = 16383`, `maxImageDimension3D = 512`.

## O tradutor já existe como fonte

`~/eden/src/shader_recompiler/`, ~19 000 linhas:

```
frontend/maxwell/   decode.cpp, opcodes.cpp, structured_control_flow.cpp,
                    translate/        -> descodifica SASS Maxwell
frontend/ir/        -> IR
ir_opt/             -> optimizações
backend/spirv/      -> SPIR-V
backend/glsl/       -> GLSL
backend/glasm/      -> GLASM
```

Isto é literalmente o Maxwell -> Valhall que se pede, pela via que é possível:
SASS -> IR -> SPIR-V, e o PanVK compõe para Valhall. O caminho não é
instrução-a-instrução para a ALU da Mali (ninguém publica a ISA da Mali), é
**descodificar Maxwell e voltar a compilar** — que é o mesmo resultado.

## O nosso `src/gpu/maxwell_translator.cpp` é um esqueleto

`ShaderCache::compile()` faz `memcpy` do blob e chama-lhe SPIR-V. São 480
linhas das quais ~90 são comentários. É a mesma classe de tease do runtime
antigo do piDroid — e a regra do projecto diz que isso se apanha medindo.

## O bloqueio real

**O jogo não chega à GPU.** Aborta em `nn::oe` antes de abrir o NVN. Não há
comandos Maxwell para traduzir porque ainda não existe nenhum.

O caminho até lá é o boot: IPC do kernel. Foi aí que me perdi.

## O que muda no plano

Duas frentes, e a segunda **não depende da primeira**:

1. **Boot** — terminar o que falta do `nn::oe` para o jogo abrir o NVN.
2. **Tradutor** — construí-lo e testá-lo **sem o jogo**, com programas Maxwell
   sintéticos, exactamente como o motor de CPU foi validado com um guest
   sintético antes de tocar no Pokémon Shield.

A frente 2 é a que o enunciado pede, e é testável hoje.


---

# Frente 2: o tradutor Maxwell, com programas sintéticos

## O que ficou feito

**A tabela da ISA Maxwell, no nosso projecto.**

`include/ia/gpu/maxwell_isa.inc` — 280 opcodes com máscara e valor, transcrita
de `maxwell.inc` do Eden. `src/gpu/maxwell_isa.cpp` descodifica uma palavra de
64 bits. `tests/test_maxwell.cpp` valida.

**O que está medido, não suposto:**
- a opcode ocupa os **bits [63:48]** da palavra de 64 bits, MSB primeiro
  (`MaskValueFromEncoding` no Eden começa em `u64(1) << 63` e percorre 16 bits);
- os wildcards do padrão são `-`;
- o valor bruto de uma opcode é o **seu índice na tabela**, não um campo
  numérico da codificação.

**13 verificações, 0 falhas.** Suite completa: 4 testes, todos a passar.

## O que o round-trip apanhou

A primeira versão do teste afirmava "todas as 280 opacodes fazem ida e volta" e
**falhou**. A opcode `LD` não voltava a `LD`.

Investigado, e **o descodificador está certo**: `LD` tem máscara `0xE000` — só
três bits — e está no fim da tabela de propósito. É uma **classe prefixo**, não
uma instrução. O descodificador é uma varredura linear que devolve a primeira
que casa, e `IMAD32I` (máscara `0xFC00`) vem antes, por isso qualquer coisa com
o topo `0x8000` é `IMAD32I` e nunca chega a `LD`. É a mesma semântica do Eden.

O teste passou a medir a lista de classes em vez de assumir que é vazia, e a
verificar que é pequena e que cada uma é mesmo sombreada por outra. Uma lista
de nomes written à mão seria o tipo de memória que este projecto proíbe.

## Um bug meu, registado

Guardei `std::string(name_of(op)).c_str()` numa estrutura — um `c_str()` de um
temporário que morre no fim da linha. Imprimiu "classe 0" em vez de "classe LD".
Corrigido para `std::string`. O sintoma foi inofensivo desta vez; a próxima vez
pode não ser.

## O que falta para o tradutor

A cadeia real, e onde está cada peça:

```
Maxwell SASS  ──►  IR  ──►  SPIR-V  ──►  PanVK  ──►  Valhall (G610)
   [feito]        [?]      [?]          [existe]     [existe]
```

- **IR**: o Eden tem `frontend/ir/`. Não é grande, mas depende do `common` do
  yuzu (fmt, boost::container).
- **SPIR-V**: o Eden tem `backend/spirv/`, ~5 000 linhas. Há uma via mais
  barata: se o IR consegue ir a **GLSL**, o `glslang` (instalado) faz
  GLSL → SPIR-V e não é preciso escrever o emissor.
- **Painel**: PanVK está activo no Pi. Falta vulkaninfo a um shader.

## O passo seguinte, e é uma decisão

Duas vias para o meio da cadeia:

1. **Trazer o `shader_recompiler` do Eden** (~19 000 linhas, com a dependência
   do `common` do yuzu). Rápido se compilar; arrasta uma árvore grande.
   Pergunta de licença: GPL-3.0-or-later.

2. **Escrever o nosso**, começando por um subconjunto: os opcodes de que um
   fragment shader mínimo precisa (MOV, FADD, FMUL, TEX, EXIT), IR próprio,
   e saída em GLSL para o glslang.

A 1 é rápida e de risco desconhecido. A 2 é lenta e controlada. É uma decisão
de arquitectura — não a tomo sozinho.


---

# Frente 2 — marco 1: a cadeia inteira, sem o jogo

```
Maxwell SASS sintetico  ->  IR  ->  GLSL  ->  SPIR-V
   [feito]                 [feito] [feito]  [feito, pelo glslang real]
```

**17 verificações, 0 falhas. Suite completa: 5 testes, todos a passar.**

O shader que passa:

```glsl
void main() {
    uint R2 = uint(0);   uint R3 = uint(0);   float R4 = float(0);
    R2 = uint(u_c[0][16]);        // MOV R2, c[0][0x10]
    R3 = uint(u_c[0][20]);        // MOV R3, c[0][0x14]
    R4 = float(float(R2) * float(R3));   // FMUL R4, R2, R3
    R4 = float(float(R4) + float(R2));   // FADD R4, R4, R2
    o_color = vec4(R4);
}
```

O `glslang` aceita, o `spirv-val` valida, o `spirv-dis` desassembla.

## A codificação dos operandos (medida, não de memória)

De `impl.cpp` e `move_register.cpp` do Eden:

| campo | bits |
|---|---|
| `Ra` | [0, 8) |
| `Rb` | [8, 16) |
| predicado | [16, 20) |
| `Rd` nos aritméticos | [20, 28) |
| `Rc` | [39, 47) |
| cbuf offset (palavras) | [20, 34) |
| cbuf binding | [34, 39) |
| imm 20 (sinal no bit 56) | [20, 39) |
| imm 32 | [20, 52) |
| opcode | [63, 48) |

## Quatro bugs que os testes apanharam

Nenhum deles dava erro. Todos davam GLSL que **compilava**.

1. **`make()` com a máscara dos wildcards invertida.** `payload & wild` em vez
   de `payload & ~wild` — todos os operandos liam zero. O GLSL dizia
   `R0 = R0 * R0`.
2. **O destino do `MOV` não está em [20:28).** Está em **[0, 8)** — o Maxwell
   usa um layout de operandos diferente para `MOV` e para os aritméticos.
   Confirma-se em `move_register.cpp` (`BitField<0, 8> dest_reg`).
3. **O bit de "vou buscar a uma constante" não existe para o `MOV`.**
   `MOV (reg)` e `MOV (cbuf)` são opcodes distintos. Pôr `is_cbuf` à mão
   arruinou o destino duas vezes — com o bit 0 escrevia em R3, com o bit 5
   escrevia em R34.
4. **Em Maxwell um registo não tem tipo.** O tipo vem do uso: R2 escrito por
   um `MOV` inteiro e lido por um `FMUL` como float. O GLSL não tem isso, e
   `float + uint` é erro de compilação. Os operandos levam conversão explícita.

O ponto comum: **um tradutor de shaders que produz saída válida com a
semântica errada é o pior tipo de bug possível.** Compila, executa, e dá uma
imagem errada que ninguém consegue explicar.

## `IMAD` ficou de fora, e é de propósito

`IMAD.MOV.U32 Rd, RZ, RZ, c[bank][off]` é a forma canónica de carregar uma
constante, mas o destino do IMAD **não** está em [20:28) — esse campo é o
offset do banco. A posição certa não foi confirmada, e adivinhá-la produziria
constantes erradas com o GLSL a compilar.

Fica para quando se confirmar em `integer_scaled_add.cpp`.

## O passo seguinte

**Executar o SPIR-V na G610 de verdade.** Falta um device Vulkan, um UBO com
as constantes, um render pass e um readback de um pixel — e comparar o valor
com o que o shader deveria produzir. É o que fecha o ciclo, e é medível:
`R4 = 1.0f * 0.5f + 1.0f = 1.5`, e o pixel tem de dar 1.5.


---

# Frente 2 — marco 2: a cadeia executada na Mali-G610

```
Maxwell SASS -> IR -> GLSL -> SPIR-V -> PanVK -> Mali-G610 -> pixel lido
   [feito]      [feito] [feito]  [feito]   [feito]   [feito]      [feito]
```

**O pixel lido de volta é 1.500000. O previsto é 1.500000.**

O shader é `R4 = 1.0f * 0.5f + 1.0f`, com `c[0][0x10] = 1.0f` e
`c[0][0x14] = 0.5f` no banco de constantes.

Device: **Mali-G610**, PanVK (Mesa 25.2.8), Vulkan 1.4.318, sem surface e
sem Wayland — render para uma imagem de 64x64 offscreen e readback.

Suite: **6 testes, todos a passar.**

## Os quatro bugs que faltavam, e como foram apanhados

### 1. `std140` achata arrays de escalares a vec4

O backend dividia o offset da constante por 4. O SPIR-V medido diz:

```
OpDecorate %_arr_uint_uint_256 ArrayStride 16
```

O passo é **16**, não 4. O shader lia `u_c[0][16]` = byte 256; o teste
escrevia em byte 64. Um `uint[256]` em std140 ocupa 4096 bytes, não 1024,
com três quartos de enchimento.

Agora o passo vive numa função (`cbuf_stride_bytes()`) que o backend e o
teste partilham, e o teste **confirma-o contra o binário** com `grep -q` em
`ArrayStride 16`. Se o layout mudar, falha com um número.

### 2. Dependências de subpass — e a lição do controlo

Sem `VkSubpassDependency` explícita, a escrita no attachment de cor não é
visível para o `vkCmdCopyImageToBuffer`. O sintoma medido: **cada desenho lia
o resultado do ANTERIOR**. O primeiro lia 0, o segundo lia o valor do
primeiro. Nenhum erro, nenhuma mensagem do validador.

E o que quase custou: o diagnóstico automático que escrevi dizia "as
constantes chegaram vazias". Estava certo **e** era mentira — 0 também é o
que se lê de um buffer que nunca foi preenchido.

Daí a saída: **dois shaders de controlo**, escritos à mão.

| shader | o que toca | o que tem de dar |
|---|---|---|
| controlo | nada, escreve `3.0` | 3.0 |
| controlo do banco | só lê `c[0][0x10]` | 1.0 |
| do tradutor | a cadeia toda | 1.5 |

Sem o controlo, a conclusão teria sido "o tradutor está errado", e o tradutor
estava inocente.

### 3. A barreira estava invertida

`srcSubpass`/`dstSubpass` trocados. A dependência descrevia a entrada no
subpass em vez da saída. Com a troca, o controlo passou a dar 3.0.

### 4. `float(uint)` não é o que o guest faz

Este é o importante, e só apareceu quando o pipeline deixou de estar atrasado
uma frame.

Em Maxwell, `MOV R2, c[0][0x10]` escreve um **padrão de bits**, e um
`FMUL R4, R2, R3` a seguir **reinterpreta** esses 32 bits como float. O GLSL
separa as duas coisas:

```glsl
float(R2)           // conversão NUMÉRICA: 1065353216 -> 1065353216.0
uintBitsToFloat(R2)  // reinterpretação:      1065353216 -> 1.0
```

O backend usava `float()`. Compilava, validava, executava, e devolvia
**1126040644330979328.000000** em vez de 1.5 — que é
`1065353216.0 * 1056964608.0 + 1065353216.0`, exactamente os dois padrões de
bits (`0x3F800000` e `0x3F000000`) tratados como números.

O que a medição revela: **o banco de constantes estava perfeito.** Os valores
que chegavam ao shader eram os dois certos, à mão e ao bit. O tradutor não
tinha um problema de dados; tinha um problema de *interpretação*.

Nenhum teste do tipo "os valores que chegam são os que se escreveram"
apanharia isto, porque os valores eram os certos. O teste que apanha é o que
pergunta "a conta deu o resultado que a conta dá".

O frontend passou a guardar o tipo com que cada registo foi escrito pela
última vez (`reg_tipo[16]`), para o backend poder distinguir padrão de bits de
número — sem isso não há forma de saber que `R2` é bits e não um inteiro.

## O GLSL que passa

```glsl
void main() {
    uint R2 = uint(0);  uint R3 = uint(0);  float R4 = float(0);
    R2 = u_c[0][4];                              // MOV R2, c[0][0x10]
    R3 = u_c[0][5];                              // MOV R3, c[0][0x14]
    R4 = uintBitsToFloat(R2) * uintBitsToFloat(R3);   // FMUL R4, R2, R3
    R4 = R4 + uintBitsToFloat(R2);               // FADD R4, R4, R2
    o_color = vec4(R4);
}
```

## O que isto NÃO é

- **Não é um shader de jogo.** É um shader de teste, sem texturas, sem
  controlo de fluxo, sem predicados, sem `ST.E.CON` (cuja codificação ainda
  não está confirmada), e que escreve um literal de 1.5 num pixel.
- **Não é o NVN.** Não há memória de guest, não há canais, não há barrieras,
  não há-transferências de memória. É um render pass num buffer Vulkan.
- **Não é o backend de produção.** É um caminho de teste directo.

O que É: a cadeia de tradução está correcta e medida no destino. Tudo o que
falhar a partir de agora vai ser por falta de cobertura da SASS, não por
conversão errada. É uma base muito diferente de um tradutor que "parece
funcionar".

## O passo seguinte

Trazer a **SASS real** de um shader do jogo e ver o que cai fora do
subconjunto. Já sabemos que o `IMAD.MOV.U32` é o idioma de carregar
constantes e que o seu destino não está em [20:28). Confirmá-lo em
`integer_scaled_add.cpp`, e depois medir o subconjunto contra os shaders de
um `.nsp` de verdade.


---

# Frente 2 — marco 3: o layout de operandos estava invertido

Fui confirmar o `IMAD` e encontrei outra coisa. Vale a pena porque é o erro
mais instrutivo do projecto todo.

## O que a fonte diz

`integer_scaled_add.cpp` (o ficheiro do `ISCADD`, não do `IMAD`) tem:

```cpp
union {
    u64 raw;
    BitField<0, 8, IR::Reg> dest_reg;
    BitField<8, 8, IR::Reg> op_a;
} const iscadd{insn};
```

E o mesmo em `floating_point_add.cpp`, `floating_point_multiply.cpp`,
`integer_add_three_input.cpp` e `move_register.cpp`: **`dest_reg` é
`BitField<0, 8>` nos cinco.** O layout certo é:

| campo | bits |
|---|---|
| `Rd` destino | **[0, 8)** |
| `Ra` | **[8, 16)** |
| `Rb` | **[20, 28)** |
| `Rc` | [39, 47) — só o IADD3 |
| predicado | [16, 20) |

E `[20, 39)` é o **operando B**, que conforme o opcode é registo (em
[20,28)), constante (offset [20,34) + binding [34,39)) ou imediato de 20.

O que eu tinha: `Rd` em [20:28) e `Ra` em [0:8). **Invertido.**

## Porque é que os testes passavam na mesma

Porque o **gerador de SASS sintética** usava a mesma convenção errada. O
frontend e o gerador concordavam, o teste comparava os dois, e dava certo.

É a forma mais perigosa de um teste passar: um teste que concorda com o bug
não é um teste, é uma assinatura. A correcção só veio de ir à fonte, e a
fonte é unânime.

## `ISCADD`, não `IMAD`

Procurando o `IMAD` descobri que nos shaders da Switch o opcode real é o
**`ISCADD`** — `IMAD_*` está em `not_implemented.cpp` no Eden, e `ISCADD_*`
está implementado. O Eden não implementa o que não aparece nos jogos.

E o `ISCADD` cobre, com um só opcode:

```
ISCADD Rd, RZ, c[bank][off]        ->  Rd = constante
ISCADD Rd, RZ, Rb                  ->  Rd = Rb
ISCADD Rd, Ra, c[bank][off], esc=n ->  Rd = (Ra << n) + constante
```

## RZ = 255

`ir/reg.h` no Eden, com `static_assert`:

```cpp
RZ,
};
static_assert(static_cast<int>(Reg::RZ) == 255);
constexpr size_t NUM_USER_REGS = 255;
```

Um campo de registo com **0xFF** é o registo zero, não o R255.

O frontend traduz RZ para a **constante 0** logo no `v_reg()`. É o que faz
`ISCADD Rd, RZ, c[..]` cair no caminho comum sem nenhum caso especial: no
GLSL, `0 << n` é 0, e a constante fica. Traduzir RZ no frontend em vez de o
tratarem no backend é o que mantém esta parte do IR honesta — o IR não tem
casos especiais para "isto era um RZ".

## Campos do `ISCADD` (de `integer_scaled_add.cpp`)

- escala: bits **[39, 44)** (5 bits)
- `cc` (escreve flags): bit 47
- `neg_b`: bit 48
- `neg_a`: bit 49
- **os dois juntos não são duas negações** — é a forma *predicada* do
  opcode. O Eden trata-a como `B + 1`. Copiei isso porque é o comportamento
  medido contra hardware, mas **não é uma predicação** e o IR não tem
  predicados. Um shader que use isto a sério vai dar um resultado errado sem
  nenhum aviso. Está escrito no `ir.hpp`.

## Estado

24 verificações no `ia_translate_test`, 20 no `ia_gpu_exec`, 6 suites a
passar. O pixel na G610 continua a dar 1.500000 com o shader agora escrito em
`ISCADD`, que é o idioma real.


---

# O jogo a correr: onde para, e o que se confirmou

Corrido com `ia_boot ~/piDroid/piDroid_Test_ARM/work/exefs`. O guest chega ao
primeiro IPC do programa inteiro e aborta logo a seguir.

```
ConnectToNamedPort  x1=0x834593c9        -> "sm:"     (handle fabricado 0x1000)
SendSyncRequest     x0=0x1000 x1=0x88000000 x2=0x1000
Break               x0=0x0                (abort deliberado do SDK)
```

## O pedido, byte a byte

`IA_IPC_DUMP=1` despeja o buffer do IPC. O pedido completo são 4096 bytes e
só quatro têm alguma coisa:

```
0000  06 00 00 00   type = 6 (RequestWithContext)
0004  0a 00 00 80   data_size = 10, flags = 0, enable_handle_descriptor = 1
0008  01 00 00 00   HandleDescriptorHeader: send_current_pid = 1
0020  53 46 43 49   "SFCI"
0024  01 00 00 00   <-- a palavra de enchimento, que valia 1
01e0  24 56 54 21   "$VT!" — a nossa ThreadLocalRegion
```

## Três erros de 4 bytes, todos confirmados na fonte

**O `command id` é um `u64` e o `DataPayloadHeader` tem 8 bytes.**

```cpp
// core/hle/ipc.h:174
struct DataPayloadHeader {
    u32_le magic;
    INSERT_PADDING_WORDS_NOINIT(1);
};
static_assert(sizeof(DataPayloadHeader) == 8, ...);

// core/hle/service/ipc_helpers.h:298 — o servidor ao LER um pedido
// Skip the u64 command id, it's already stored in the context
static constexpr u32 CommandIdSize = 2;   // dois u32
Skip(CommandIdSize, false);
```

Líamos o command id em `payload+4` — dentro da palavra de enchimento, que
por acaso valia **1**. Ou seja: o guest enviava `Initialize` (0) e nós
respondíamos a `GetService`. **Respondíamos a um comando que ninguém tinha
enviado.** Corrigido.

**O `Result` também estava 4 bytes a menos.** Vai em `payload+0x10`, não em
`payload+4`.

**E o payload da resposta não é no sítio do pedido.** O `ResponseBuilder`
(ipc_helpers.h:36-101) alinha o índice de palavras a um múltiplo de 4 — 16
bytes — depois do `CommandHeader` e do descritor de handles. Dá sempre **0x10**,
com zero ou com um handle. Medido no pedido: **0x20**. A resposta anterior era
escrita a ecoar o pedido. E o Eden faz `memset` do buffer inteiro antes de
escrever, sem o que sobram bytes do pedido — uma resposta feita de remendos.

Também ficou certo o `data_size` da resposta: `normal_params + 2 + 4 +
normal_params` = 10 para 2 parâmetros. O valor anterior, 8, saiu do ar. E o
pedido real do jogo tem 10, o que confirma a conta por um caminho
independente.

## A resposta NÃO é a causa — medido

Matriz de 16 variantes de formato de resposta, incluindo **não escrever
nada**:

| variante | SendSyncRequest | Break |
|---|---|---|
| 0 (regra do ResponseBuilder) | 1 | 1 |
| 8 (sem resposta nenhuma) | 1 | 1 |
| 1, 2, 3, 9, 15 | 1 | 1 |

Todas iguais. A resposta IPC sai da lista de hipóteses.

## O que NÃO se confirmou

- **Porque é que o guest aborta.** Não é o formato da resposta. Pode ser o
  `send_current_pid`, o handle de sessão fabricado, o `Result` da SVC
  `SendSyncRequest`, ou outra coisa entirely fora do IPC.
- **O `tid` que o guest envia no `Initialize` é 0.** Vem do nosso
  `GetThreadId`. Não se sabe se isso é um problema ou se é o que o guest
  espera.
- **Se o abort é do `nn::oe` ou do `libnx`.** O `x30` do `Break` é
  `0x82b7b1e8`, dentro do `sdk` (0x829fb000–0x83529000), sem símbolos.

## Estado

Suite: **6 testes, todos a passar**. A frente do GPU continua intacta: o pixel
na G610 dá 1.500000.

O motor, a telemetria e o tradutor estão bons. O que falta é o OS — e o
`sm:` é o primeiro serviço de que o jogo depende.


---

# Seguir o abort: a cadeia de chamadas

Sem símbolos e sem ponto de paragem no debugger, a pergunta e' "que comparacao
falhou".
Resposta: chegar ao codigo.

## A janela do abort

`[cod]` despeja 0x100 bytes à volta do `x30` do `Break`. Desassemblado:

```
82b7b1d0: stp  x29, x30, [sp, #-16]!
82b7b1d4: mov  x29, sp
82b7b1d8: mov  x1, x0            ; o argumento da função
82b7b1dc: orr  w2, wzr, #4
82b7b1e0: mov  w0, wzr           ; x0 = 0
82b7b1e4: bl   0x82d79ba4        ; Break
82b7b1e8: b    0x82b7b1e8        ; nunca sai
```

A função de abort começa em `0x82b7b1d0`, e `x1` NÃO é texto — o guess da
string de asserção estava errado. (`[msg]` ficou no código e mostra `w2 = 4`
e mais nada, que é o que há.)

## Quem a chamou — pela stack, não pelo `.text`

Primeiro o caminho óbvio: varrer o `.text` dos três módulos à procura de um
`BL` cujo alvo fosse `0x82b7b1d0`. Imagem pequena dá 6.656 bytes, `main`
dá 25 MB. Resultado: **nenhuma chamada directa**.

E a razão é boa: a chamada é **indirecta**, por um stub tipo PLT. Os `bl` do
SDK apontam para uma coluna de stubs em `0x82f13xxx`, que depois saltam para
o destino real.

O caminho que funciona é a **stack**. A função fez `stp x29, x30, [sp,#-16]!`,
logo o x30 dela está no topo da stack — e o x30 é o endereço de retorno:

```
[quem] stack+8 = 0x82b79cc0 (retorno)
```

Vinte e cinco megabytes lidos para "nenhuma"; a stack deu a resposta à
primeira. É sempre o caminho que não depende de haver símbolos.

## O chamador

```
82b79c70: sub  sp, sp, #0xe0
82b79c74: stp  x26, x25, [sp, #144]
...
82b79c88: add  x29, sp, #0xd0
82b79c8c: mov  x24, x0
82b79c90: adrp x0, 0x83658000
82b79c94: add  x0, x0, #0x4d0     ; x0 = 0x836584d0
82b79cb0: bl   0x82f138c0
82b79cb4: tbz  w0, #0, 0x82b79cc0 ; se w0 bit0 == 0, NAO aborta
82b79cb8: mov  x0, x19
82b79cbc: bl   0x82f138d0          ; aborta
82b79cc0: ...
82b79cc4: adrp x0, 0x83658000     ; a MESMA referência
82b79ccc: bl   0x82f0de60
82b79cd0: orr  w8, wzr, #3         ; w8 = 3
82b79cd4: str  w8, [sp, #80]      ; ... e constrói uma va_list
```

Isto é uma função de **log de erro** no formato do printf: guarda x0..x7, e
depois de `0x82b79cd4` monta a lista de argumentos variáveis (`w8 = 3` é o
`gp_offset`, e as seguintes movem `x25=x7`, `x20=x5`, `x19=x4`, `w21=x3`,
`x22=x2`, `x23=x1` para os registos de argumento).

**A condição do abort**: `bl 0x82f138c0` devolveu `w0` com o **bit 0 a 1**, e
o `tbz` deixou passar.

## O endereço `0x836584d0`

O guess seguinte era "é uma string". Não é: os 64 bytes seguintes são todos
zeros, e o offset dentro da imagem do sdk é `0x12ACD68` de `0xC72CC8` — ou
seja, dentro da imagem, mas em `.bss`. **Um global inicializado a zero.**

E a lição: `adrp`+`add` a formar um endereço não implica que esse endereço
seja uma string. Um global em `.bss` é um alvo perfeitamente normal para
`x0` numa função que depois chama outras com um contexto.

## O que ficou de fora, e porquê

- **Descodificar o `ADRP` à mão** para varrer strings da `.rodata` foi
  tentado e **não funcionou** — com a conta certa dava sempre zero. O
  `objdump` descode-o bem; a ferramenta certa para o trabalho é a
  ferramenta certa, e não uma reimplementação à pressa. Ficou o `IA_STR=addr`,
  que lê texto num endereço à escolha.
- **A janela de código** media 0x100 e terminava exactamente no endereço de
  retorno. O `objdump` mostrava um `...` que parecia um buraco de zeros e
  era só o fim dos dados. Um ficheiro de despejo que acaba onde o olho está
  a olhar é pior do que um ficheiro menor mas completo. Agora são 0x200.

## Telemetria que ficou

| o que | como |
|---|---|
| pedido e resposta IPC, byte a byte | `IA_IPC_DUMP=1` |
| janela de código à volta do abort | `[cod]` |
| quem chamou, pela stack | `[quem]` |
| texto num endereço do guest | `IA_STR=0x...` |
| a resposta IPC, isolada | `IA_IPC_VARIANT` (16 variantes) |

Suite: **6 testes, todos a passar**.


---

# Parar de descobrir a roda: o que o Eden já sabe

O aviso certo. Vinte variantes de formato de resposta e três offsets adivinhados
são o caminho lento quando ao lado está um emulador a correr o mesmo jogo.

## 1. O payload calcula-se; não se procura

A versão anterior **varria o buffer à procura da assinatura "SFCI"**. Era
robusto e era o caminho errado: estava a esconder a aritmética que o Eden faz
por escrito.

`HLERequestContext::ParseCommandBuffer` (`hle_ipc.cpp:142`) diz tudo, com o
índice contado em palavras de 32 bits:

```
índice = 2                              // o CommandHeader
se enable_handle_descriptor:
    índice += 1                         // o HandleDescriptorHeader
    se send_current_pid:
        índice += 2                     // <<<< a peça que faltava
    índice += num_handles_to_copy + num_handles_to_move
índice += descritores X (1), A (3), B (3), W (3)
índice  = arredondar(index) para múltiplo de 4      // 16 bytes
payload = índice * 4
índice += 2                             // o DataPayloadHeader
corpo   = índice * 4
```

Para o pedido real do jogo:

```
CommandHeader       0..1  -> 2
HandleDescriptor    2     -> 3
send_current_pid    skip 2 -> 5
alinhamento a 16    5 -> 8          (5 & 3 = 1, salta 3)
DataPayloadHeader   8     = 0x20    <-- medido
corpo               0x30
```

**O `Skip(2, false)` do `send_current_pid` é a linha que faltava.** Sem ela o
índice dá 4 e o payload cairia em 0x10. Com ela dá 8 e cai em 0x20 — que é
onde o jogo o põe. A aritmética está agora no código, e uma comparação
contra a assinatura diz logo se algum dia divergir.

O `CommandHeader` completo, de `ipc.h:51`:

| raw_low | bits | |
|---|---|---|
| `type` | [0,16) | Request=4, Control=5, **RequestWithContext=6**, ControlWithContext=7 |
| `num_buf_x_descriptors` | [16,20) | 1 palavra cada |
| `num_buf_a_descriptors` | [20,24) | 3 palavras |
| `num_buf_b_descriptors` | [24,28) | 3 palavras |
| `num_buf_w_descriptors` | [28,32) | 3 palavras |

| raw_high | bits | |
|---|---|---|
| `data_size` | [0,10) | |
| `buf_c_descriptor_flags` | [10,14) | Disabled=0, Inline=1, One=2 |
| `enable_handle_descriptor` | [31] | |

## 2. `ConnectToNamedPort` devolve uma SESSÃO, não uma porta

`svc_port.cpp:19-55`:

```cpp
Result ConnectToNamedPort(Core::System& system, Handle* out, u64 user_name) {
    ...
    // Reserve a handle for the port.
    // NOTE: Nintendo really does write directly to the output handle here.
    R_TRY(handle_table.Reserve(system.Kernel(), out));

    // Create a session.
    KClientSession* session;
    R_TRY(port->CreateSession(system.Kernel(), std::addressof(session)));

    // Register the session in the table, close the extra reference.
    handle_table.Register(system.Kernel(), *out, session);
    session->Close(system.Kernel());
}
```

Duas coisas que se leram ao contrário:

**Cria uma sessão por dentro.** É por isso que o guest vai directo do
`ConnectToNamedPort` para o `SendSyncRequest` **sem `OpenSession`** — e é
por isso que o handle 0x1000 tem de servir de sessão. O nosso handle
fabricado está estruturalmente certo; o que falta é a tabela de objectos.

**Escreve o handle no ponteiro do guest (`out`, que é o nosso x0), e não
só em x1.** Implementado. Não mudou o resultado porque o SDK lê de x1 (o
handle que passou a seguir foi o nosso) — mas o comentário do Eden é
explícito e não havia razão para o ignorar.

## 3. `SendSyncRequest` usa o buffer da thread

```cpp
Result SendSyncRequest(Core::System& system, Handle session_handle) {
    R_RETURN(SendSyncRequestImpl(system.Kernel(), 0, 0, session_handle));
}
```

Mensagem e tamanho a **zero**: o kernel usa o `message_buffer` da thread. O
`x1`/`x2` que o SDK deixou no registo são lixo, e nós estávamos a tratá-los
como se fossem argumentos.

E `SendSyncRequestImpl` resolve o handle como `KClientSession` da tabela do
processo. **É a tabela de objectos que falta** — a parte grande.

## O que muda a arquitectura

Não é mais um offset. É: **portos, sessões e tabela de handles**, como o
Eden tem, e depois o registo de serviços. Deixado escrito porque é a decisão
que muda o resto do projecto, e porque sem ela cada serviço novo obriga a
inventar um caminho novo.

## Estado

Suite: **6 testes, todos a passar**. GPU medido: 1.500000 na G610. O jogo
continua a abortar no primeiro IPC, e continua a não ser por causa do
formato da resposta — as 16 variantes já o provaram.


---

# O que o programa de teste é, afinal

`IA_STRINGS=main` extrai o texto ASCII de 18.059 strings da `.rodata` e da
`.data` do `main`, já descomprimidas. O que lá está:

```
C:\jenkins\workspace\orion\RomBuild\program\output\NX64\Release\main.nss
c:/projects/env/NintendoSDK/NintendoSDK/Libraries/NX-NXFP2-a64/Release//glslc.nss
nnSdk.nss
_ZN2nn4diag6detail18OnAssertionFailureENS0_13AssertionTypeEPKcS4_S4_i
nndetailDiagAbortFormatImpl

RigidBody::setRigidBodyFlag: dynamic meshes/planes/heightfields are not supported!
GPU cloth creation failed. Falling back to CPU implementation.
CPostEffect::Initialize(): m_pMotionBlurReducedWork->InitializeDevice() succeeded.
==== TEXSHADERSET::InitializeDevice() succeeded. ====
IGPUIndexBuffer::...BuildIndexBuffer(Size: %d, nBuffers: %d, %s, Usage: 0x%x ...) failed
SiGfx failed to initialize.
```

## O que isto muda

**Não é um jogo comercial.** É um programa do **SDK da Nintendo** com PhysX
(monorcloths, articulações), **SiGfx** — a API de gráficos da Nintendo,
equivalente ao NVN — pós-processamento, texturas e protobuf.

Três consequências, todas boas:

1. **Tem shaders Maxwell, e many.** `NX-NXFP2-a64` é o perfil Maxwell+ e
   `glslc.nss` é o compilador de shaders da NVIDIA. O banco `GshD` não está
   no `main` porque os shaders vivem nos objectos da SDK — mas estão lá.

2. **É código de nicho.** As mensagens de asserção estão em texto claro na
   `.rodata`. Um jogo comercial esconde tudo; este diz o que espera. É
   exactamente o oposto do problema que tive antes.

3. **A função de abort está identificada:**
   `nn::diag::detail::OnAssertionFailure(AssertionType, const char* cond,
   const char* file, const char* func, int line)`. E os argumentos chegam em
   x0..x4 — o que casa com a função de log de erro que desassemblei
   (`mov x24, x0` ... `mov w21, w3`, mais os registos de `va_list`).

## O abort, outra vez

Continua a ser depois do primeiro `SendSyncRequest`, e continua a não ser por
causa do formato da resposta — as 16 variantes já o provaram, incluindo
"não escrever nada".

Com o `nn::diag` identificado, o passo certo deixou de ser adivinhar o
formato e passou a ser **ter a tabela de objectos** (portos, sessões, handles)
como o Eden tem, e dar ao `sm:` um servidor de verdade. `ConnectToNamedPort`
cria uma sessão por dentro — está no `svc_port.cpp` — e é por isso que o
`SendSyncRequest` a seguir funciona sem `OpenSession`.

## Estado

Suite: **6 testes, todos a passar**. GPU: 1.500000 na G610.

Telemetria nova: `IA_STRINGS=main`.


---

# A tabela de objectos, copiada do desenho do Eden

`include/ia/kobject.hpp`. O Eden é GPL e este projecto não é: o que se copiou
é a **arquitetura**, e cada campo diz de onde veio.

## O handle

`k_handle_table.h:262-278`:

```
bits [0,15)   index
bits [15,15)  linear_id     <- a GERAÇÃO
bits [30,2)   reserved = 0
```

E `IsValidHandle` (linha 201-217) exige `raw != 0`, **`linear_id != 0`**,
`reserved == 0`, `index < tamanho`.

**Os handles de antes (`0x1000`, `0x1001`, ...) tinham `linear_id == 0`.**
Nenhum passaria um `IsValidHandle` do Eden. Funcionavam porque o nosso
dispatcher nunca os validava — e uma tabela que valida o que valida sozinha
não é uma tabela, é um `switch` grande.

O `linear_id` é uma geração: é o que faz um handle fechado ser reconhecido
como fechado quando o índice já foi reatribuído. Sem ele, um handle antigo
apontava para o objecto errado em silêncio.

**Reserve e Register são dois passos** (`svc_port.cpp:37-50`): reserva-se o
índice, cria-se o objecto, e só depois se regista. Se a criação falhasse com
o índice já registado, ficava um handle válido a apontar para meio objecto.

## Port ≠ Sessão

`ConnectToNamedPort` cria uma **porta** e uma **sessão**, e devolve o handle
da **sessão**. Antes havia um contador que fabricava os dois ao mesmo tempo,
e um handle de porta acabava a ser usado como sessão sem ninguém dar por
isso. É também por isso que o guest vai directo do `ConnectToNamedPort`
para o `SendSyncRequest` sem `OpenSession` nenhum.

O handle passou de `0x1000` para **`0x10001`** — índice 1, geração 2.

## Onde estão os shaders SiGfx — e onde NÃO estão

Procurei o banco de shaders e **não está em nenhum NSO**:

| ficheiro | flags | `GshD` |
|---|---|---|
| main | 0x3f | não |
| rtld | 0x3f | não |
| sdk / subsdk0 / subsdk1 | 0x3f | não |

E `IA_FIND=GshD` na imagem **já descomprimida** em memória também não
encontra. O `0x3f` dos flags são as flags de compressão dos segmentos — um
banco de shaders num `rtld` de 7 KB seria absurdo, e é o que é.

O que as strings mostram é o mecanismo real:

```
SiGfx_NX_Common.mi_shader_pkg        <- o de Switch
SiGfx_Vulkan_Common.mi_shader_pkg
SiGfx_OGL_Common.mi_shader_pkg
SiGfx_D3D11_Common.mi_shader_pkg
SiGfx_PS4_Common_Subdiv.mi_shader_pkg
...
nvnCommandBufferDiscardColor
nvnCommandBufferSetRasterizerDiscard
IGPUTexture::CreateVolumeTexture()
==== TEXSHADERSET::InitializeDevice() succeeded. ====
```

O SiGfx é uma camada da Nintendo **por cima do NVN**, com pacotes de shaders
de biblioteca (`.mi_shader_pkg`) para cada backend — Vulkan, OGL, D3D11,
PS4. E o programa liga o **`glslc.nss`**, o compilador de shaders da NVIDIA.

## O que isto muda para o tradutor

**A SASS que vamos traduzir não está num ficheiro — é gerada em runtime**
pelo `glslc` da NVIDIA, a partir do GLSL que o programa embute. E as
bibliotecas de shaders estão em pacotes `.mi_shader_pkg` que vivem noutro
NCA, que não temos.

Isto não desfaz o que está feito — a cadeia SASS → IR → GLSL → SPIR-V está
medida na G610 e é exactamente a cadeia certa. Muda só **de onde vem a
entrada**: em vez de um blob estático, vem do que o guest enviar à GPU.

O caminho passa a ser:

1. Deixar o guest passar o abort (falta o `sm:` a sério, e os serviços que o
   `SiGfx` usa).
2. Apanhar o comando NVN em que o shader é enviado, e despejar a SASS
   **antes** de a traduzir.
3. Passá-la pelo tradutor e devolver o GLSL.

E é um caminho melhor, porque o passo 2 dá-nos a SASS de um shader que
chegou a ser compilado de facto — não um que alguém possa ter gerado com
outras flags. **É a mesma lógica do `ArrayStride`: medir o que o hardware
vai ver em vez de supor.**

## Estado

Suite: **6 testes, todos a passar**. GPU: 1.500000 na G610.

Telemetria nova: `IA_FIND=<magic>` procura um magic na imagem
descomprimida, e imprime os 64 bytes a seguir.

# A costura: ligar o motor nativo ao sistema do Eden

Estado medido com `python3 tools/verificar.py`, no Pi:

    Eden vendorizado   119 ficheiros, 23956 linhas
    a ponte (nosso)      4 ficheiros,   1263 linhas
    cortes documentados  38 sitios com PONTE/CORTE no código
    emulador no vendor   nenhum (lista negra de 19, nenhuma apanhou)
    sintaxe              22 de 23 .cpp compilam

O que mudou desde a última entrada: **os 23 `.cpp` do Eden estão vendorizados e
compilam contra a nossa ponte.** O IPC, os objectos de porta e sessão, o
`ServiceManager` e o serviço `sm:` são o código do Eden, não uma reescrita.

## A ponte, em quatro ficheiros

| ficheiro | linhas | substitui |
|---|---|---|
| `k_kernel_core_bridge.h` | 305 | `kernel.h` + `kernel.cpp` |
| `k_process_bridge.h` | 477 | `k_process.h` + `k_thread.h` |
| `system_ref.h` | 229 | `core/core.h` |
| `memory_bridge.h` | 252 | `core/memory.h` |

Mais dois ficheiros nossos, que não são ponte mas estão no mesmo caminho:

    src/eden_bridge.cpp   a cadeia de montagem e os dois SVCs
    src/eden_log.cpp      substitui o logging.cpp do Eden

**1263 linhas contra 23956.** A desproporção é o resultado, não o ponto de
partida: o Eden traz a semântica do sistema e nós trouxemos a forma de lhe
dar a memória do guest.

## O que a ponte tem de mentira, e porquê

Cada uma está escrita no sítio onde vive. A lista completa:

1. **`KScopedDisableDispatch` e `KScopedSchedulerLock` são no-ops.** Não há troca
   de contexto: o guest corre noutra thread, de forma nativa e síncrona.
2. **`RunServer` e `RunOnHostCoreThread` não correm nada.** Não há thread onde
   correr; os pedidos são atendidos de dentro do `SendSyncRequest`. Guardam a
   posse, que é obrigatório — sem isso o `KPort` da porta `sm:` fica pendurado.
3. **`KLightConditionVariable::Broadcast` é um no-op e `Wait` só destrava.**
   É o único ponto onde «ninguém bloqueia» é load-bearing. Ver «A parede da
   espera».
4. **`KLightLock::Lock`/`Unlock` é o caminho rápido do Eden sem o
   `compare_exchange`.** O caminho rápido é o caminho todo quando não há uma
   segunda thread. O Eden com contenção chama `LockSlowPath`, que precisa de
   filas e temporizadores; a contenção é o que não existe.
5. **`KThreadQueue` tem três virtuais vazias.** Mesma razão.
6. **`KScheduler::IsSchedulerLockedByCurrentThread` devolve `true` sempre.** A
   pergunta é sobre um bloqueio que é um no-op; um no-op sempre tomado é
   sempre tomado.
7. **`GetSpan` devolve `nullptr` sempre.** Previsto pelo Eden: `TrySetSpan()`
   devolve `false` e o código cai para a cópia.
8. **`Core::Memory::Memory` é plana.** A reserva é um `mmap` de gama fixa, o
   endereço do guest é o do host, e não há tradução. Um «user buffer» traduz-se
   pela identidade.
9. **`IsPoweredOn` é `true` sempre.** Esta máquina está ligada.
10. **`KThread::IsSignaled` e `KProcess::IsSignaled` são `false`.** Nada os
    sinaliza. Devolver `true` seria pior: `Wait` trataria o processo como
    pronto e devolveria sem ter esperado.

## Quatro armadilhas desta costura, todas medidas

### 1. Um `sed` meu criou o ciclo de includes

O meu `sed` atirou `#include "k_process_bridge.h"` para dentro de
`k_auto_object.h` e `k_synchronization_object.h` — e isso **era** o ciclo
`ponte -> x.h -> ponte`. Com `#pragma once` um dos dois fica com o corpo vazio
e o compilador diz que o tipo não existe, sem nenhuma pista de que a causa é um
ciclo.

A regra que fica: **o ficheiro que define o tipo-base vem primeiro, e quem
precisa dele sobe na cadeia.**

### 2. `KSlabHeap` não precisa de páginas

Espera-se que precise — e portanto de uma page table, e portanto do emulador.
Não precisa:

    // k_slab_heap.h:177-186
    template <typename T>
    class KSlabHeap final : public KSlabHeapBase<false> {   // <-- false
        void Initialize(void* memory, size_t memory_size)

`KSlabHeapBase<false>` é **sem expansão dinâmica**. A página era precisely o
que o Eden usava para *expandir*, e a expansão é a variante que não está em uso.
Um slab é um bloco de memória e uma lista livre.

### 3. `Core::System::Kernel()` não é virtual

O Eden (`core.h:300`):

    [[nodiscard]] Kernel::KernelCore& Kernel();

Declarado `virtual` na primeira versão da nossa classe, **por(diff)** — e na
ligação:

    undefined reference to `Core::System::Kernel()'
    undefined reference to `Core::System::ServiceManager()'

Ser virtual muda o que o compilador emite, e o linker vai atrás de um símbolo
que ninguém escreveu. Não é erro de compilação; é erro na última fase, com um
nome de função que parece o certo.

E ao defini-los **dentro** da classe, passam a ser `inline` por defeito, e uma
função `inline` só é emitida se o compilador precisar — com `-O2` ele inlineia
e não emite. Por isso vivem em `eden_bridge.cpp`.

### 4. Um `unique_ptr` de tipo incompleto obriga a declarar, não a definir

Esta deu quatro vezes seguidas, e em quatro formas diferentes:

- o **destructor** implícito de `SvcDispatcher` — gerado em todos os TUs que
  incluem o cabeçalho, e cada um deles a fazer `sizeof` de um tipo incompleto.
- o **construtor** de `SvcDispatcher` — tem de poder destruir os membros se
  algo disparar uma excepção a meio.
- a **cópia e o movimento** de `Core::System` — apagar a cópia não chega, o
  construtor de movimento continua a ser gerado.
- o **corpo inline de `RunServer`** — `servidor_ = std::move(...)` chama
  `default_delete<ServerManager>::operator()`, que faz `sizeof`. E o corpo de
  uma função inline é verificado **mesmo que ninguém a chame**.

Mais dois do mesmo tipo, por outra via:

- uma declaração à frente **dentro** da classe nomeia `Classe::Tipo`, que é um
  tipo diferente. Aconteceu duas vezes: `KernelCore::KObjectNameGlobalData` e
  `SvcDispatcher::EdenSystem`. A mensagem de erro mostra um nome de tipo que
  parece o certo — e esse é o pior tipo de erro, o que não denuncia nada.
- `namespace Core { ... }` **dentro** de `namespace ia { ... }` cria
  `ia::Core`. E `Core::System` dentro de `namespace Kernel` resolvia para
  `Kernel::Core` — a classe do Eden que tentávamos substituir. Por isso a
  classe chama-se `HostCore` e todos os nomes externos são qualificados.

## A parede da espera

`KSynchronizationObject::Wait` **dorme uma thread**. Usa
`KScopedSchedulerLockAndSleep`, que é o escalonador. É a única coisa do Eden
que este motor **não pode ter**: não há troca de contexto para dormir, e não há
nada a acordar.

O que medi antes de parar aí:

    nm sobre a biblioteca ligada, depois de tirar o .cpp:
      U Kernel::KSynchronizationObject::KSynchronizationObject(KernelCore&)
      U Kernel::KSynchronizationObject::~KSynchronizationObject()
      U Kernel::KSynchronizationObject::Finalize(KernelCore&)
      U Kernel::KSynchronizationObject::NotifyAvailable(KernelCore&, Result)

**`Wait` não está na lista.** Nenhum ficheiro que compilamos chama
`KSynchronizationObject::Wait`. O que falta são quatro funções.

Isto suggests a via curta: escrever o `k_synchronization_object.cpp` nosso com
essas quatro e sem a espera. E é o que vou fazer a seguir.

O que falta ao lado: `KServerSession` (6 métodos) e `KSpinLock` (2).
`KServerSession` **é** preciso — é o despacho do lado servidor, e sem ele o
`sm:` não atende ninguém. O seu `.cpp` usa a page table em 21 sítios, todos do
caminho de *shared memory* (map aliases), que os quatro comandos do `sm:` não
tocam.

## O que fiz mal, e o que ficou escrito por causa disso

Quatro vezes nesta sessão escrevi código com uma palavra de outra língua no meio
de uma frase em português. Nenhuma delas partiu a build — todas as outras
palavras eram válidas — e portanto nenhuma teria aparecido num teste. Passei a
varrer o projecto em busca de caracteres CJK depois de cada edição.

O mesmo dia, uma frase minha ficou a meio em inglês: «all the other words
compiled». Escrevo em português com palavras de inglés misturadas há semanas e
nunca reparei. As duas coisas são o mesmo sintoma: **a minha atenção é um
recurso escasso, e o que escapa não aparece em nenhum teste.**

O `str.replace` do Python **não avisa** quando o texto não está lá. Passou-me
três vezes nesta sessão, sempre com a mesma consequência: um ficheiro que
continuava a ter o código antigo e um erro de compilação a trinta linhas de
distância. Passaram a ser `assert` antes de escrever.

## Um erro de escrita que valeu a pena registar

O meu `podar_vendor.py` declarava

    def incluir_de(fichheiro, visitados, raizes_extra):

e usava `ficheiro` no corpo. Um `h` a mais. Numa terminal monocromática os
dois parecem iguais, e `NameError: name 'ficheiro' is not defined` não diz nada
sobre o que aconteceu. Foi o `ast.parse` a mostrar a diferença que o terminal
não mostrava — e um parameter rename tratou disto.

## Ferramentas que ficaram

    tools/vendorizar.py    copia um .cpp do Eden e fecha o fecho de headers
    tools/podar_vendor.py   mede o fecho por ALCANÇABILIDADE e apaga o órfão
    tools/verificar.py     conta o que é nosso, lista negra, compila tudo
    tools/cortar_server_session.py   os cortes de page table, com o porque

`podar_vendor.py` é o mais útil e é a resposta directa a «não copiar o
emulador». Ele não apaga por critério; apaga o que **nenhuma fonte que
compilamos alcança**, que é uma medida. Foi ele que apanhou 7.062 linhas de
escalonador e page tables que o fecho transitivo tinha trazido na primeira
volta.

Regra que saiu daí: **não excluir uma hipótese sem a testar.** Podar, depois
compilar os 23 `.cpp`, e só believing se continuarem todos a compilar.
