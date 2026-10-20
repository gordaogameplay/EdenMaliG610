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
