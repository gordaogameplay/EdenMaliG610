# PROGRESSO — EdenMaliG610

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
3. Instrumentar o motor (`g610_boot --raw`, dumps de blocos de memória).
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

`include/g610/gpu/maxwell_isa.inc` — 280 opcodes com máscara e valor, transcrita
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

24 verificações no `g610_translate_test`, 20 no `g610_gpu_exec`, 6 suites a
passar. O pixel na G610 continua a dar 1.500000 com o shader agora escrito em
`ISCADD`, que é o idioma real.


---

# O jogo a correr: onde para, e o que se confirmou

Corrido com `g610_boot ~/piDroid/piDroid_Test_ARM/work/exefs`. O guest chega ao
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

Sem símbolos e sem ponto de paragem no debugger, a pergunta e' "que comparacao falhou".
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

`include/g610/kobject.hpp`. O Eden é GPL e este projecto não é: o que se copiou
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

---

# A parede do `SdkMutexType::TryLock` — e o que estava por trás

## O sintoma

O guest abortava em `nn::os::SdkMutexType::TryLock`, com a expressão
`this == NULL` na mensagem do assert. O sintoma não aponta para nada: um
assert sobre "this == NULL" num `TryLock` de um mutex é absurdo.

E o abort era o **único** sintoma. O IPC já funcionava (`sm:` a sério,
`lm:` a sério), o TLS estava montado, o rtld correria.

## A medição que abriu a porta

O `TryLock` do SDK, desassemblado:

```
82d2a900: mrs  x8, tpidr_el0          ; x8 = TPIDR_EL0
82d2a904: ldr  w9, [x0]               ; w9 = o dono do mutex
82d2a908: ldr  x8, [x8, #504]         ; x8 = *(x8 + 0x1F8) = tv->tls_tp
82d2a90c: and  w9, w9, #0xbfffffff    ; limpa o bit 29
82d2a910: ldr  w8, [x8, #432]         ; w8 = *(tls_tp + 0x1B0)
82d2a914: cmp  w9, w8
82d2a918: cset w0, eq                 ; devolve "o dono sou eu?"
```

E o `TryLock` aborta quando a resposta é "sim". Ou seja: aborta quando
**dono do mutex == o meu identificador**.

Medido: o dono era **0** (mutex livre) e o meu identificador também **0**.
Zero == zero, portanto "já é meu", portanto assert.

**O zero não é um valor neutro aqui: é exactamente o valor de um mutex
livre.** Era esse o bug — não o mutex, o identificador.

## Onde o identificador vem

O SDK inicializa a thread assim (medido, 0x82d28e80):

```
ldr  x8, [x21]            ; x8 = g_OsBootParamter[0]
str  w8, [x19, #432]      ; tv->tls_tp[0x1B0] = esse valor
bl   0x82d79b8c           ; svcGetThreadId(&local, handle)
str  x8, [x19, #440]      ; tv->tls_tp[0x1B8] = o ID (u64)
```

E `g_OsBootParamter[0]` é o **handle da thread principal**, escrito pelo
crt0 do SDK:

```
82d2a35c: adrp x8, 0x8360d000
82d2a360: ldr  x8, [x8, #1632]   ; x8 = g_OsBootParamter
82d2a364: str  x0, [x8]          ; g_OsBootParamter[0] = x0  <<<<
```

O `x0` vem do **rtld**, que por sua vez o recebe no `x1` do entry point:

```
80000008: cmp  x0, #0x0        ; o entry do rtld
8000000c: b.ne 0x800000c4
80000010: mov  w19, w1         ; <<<< o handle da thread, no x1
...
80000088: mov  w0, w19         ; e passa-o ao SDK
```

**A cadeia inteira, e onde ela se partia:**

| # | passo | quem faz | estado antes |
|---|-------|----------|---------------|
| 1 | handle em TLS+0x110 | kernel | **não existia** |
| 2 | handle no x1 do entry | kernel | **não existia** |
| 3 | rtld lê o x1, monta o bloco | rtld (Nintendo) |ok|
| 4 | crt0 copia para `g_OsBootParamter` | SDK | ok |
| 5 | `TryLock` compara dono com esse valor | SDK | abortava |

## O que o Eden faz, e nós não

`k_process.cpp:1006-1015`, medido:

```cpp
} else {                                  // entrada NSO, sem ABI homebrew
    main_thread->GetContext().r[0] = 0;
    main_thread->GetContext().r[1] = thread_handle;   // <-- o x1
}
this->GetMemory().Write32(GetInteger(main_thread->GetTlsAddress()) + 0x110,
                          thread_handle);                // <-- o TLS
```

**O mesmo handle nos dois sítios.** Não dois parecidos: o Eden usa uma
variável só, e é o que o loader espera.

O nosso motor não fazia nem um nem outro. Agora faz os dois, com o handle
que a tabela de handles do Eden devolve, registado pela thread principal.

## Três armadilhas no caminho

**1. O `x1` do guest não pode viajar num registo.** A primeira versão punha
o handle em `x16` (`mov x16, x3`) e o loop de limpeza de registos do
trampoline apagava-o a seguir. O x1 chegava ao rtld a zero — exactamente o
sintoma original, com a correcção aplicada. O handle viaja agora pela
stack, numa palavra que fica **acima** do sp do guest, e o sp do guest não
muda (reserva de 48 e `add sp` de 32, contra 32 e 16).

**2. A ordem das chamadas importa.** `set_guest_x1` antes de `attach_eden`
devolve zero — o handle ainda não existe. A ordem correcta é
`attach_eden` → `set_eden_tls` → `set_guest_x1`.

**3. `GetInfo(12)` e `GetInfo(13)` não são o heap.** São a região **ASLR**
(`svc_info.cpp:76-82`, `GetAliasCodeRegionStart/Size`) — a região do
**código**. Devolvíamos o heap. O SDK usa essa região para se instalar, e
com o heap no sítio errado o `g_OsBootParamter` ficava a zeros.

## O que mudou no comportamento

Antes: abort em `SdkMutexType::TryLock`.

Agora: o SDK **inicializa**. `sm:Initialize`, `lm:OpenLogger`, e depois
pede o `apm` — que o motor serviu, e depois o `appletOE`, que é o
serviço do `am`.

## `apm`: o primeiro serviço do Eden, e o padrão

O `apm` são 5 ficheiros do Eden e **280 linhas**. A única dependência
específica do Eden era o `APMController`, que por sua vez queria um
`Core::Timing::CoreTiming&` — **que nunca usava** (o Eden diz isso nos
TODOs do fim do ficheiro) e um `Settings::IsDockedMode()`.

O controller foi reescrito por nós (`tools/overrides/`), sem o `CoreTiming`
e com o modo de performance fixo em `Normal` (este motor corre um jogo, e
o jogo não está docked).

**O padrão de um serviço do Eden**, que vai repetir-se:

- o serviço é fino (o `apm.cpp` do Eden tem 29 linhas);
- o estado vive num *controller*, que o `Core::System` possui;
- a interface CMIF (os IDs dos comandos e as estruturas) é a parte que
  **tem de ser exacta** e vem do Eden tal e qual;
- o `Core::System` do Eden **não** é adoptado: tem ~40 acessores a
  subsistemas reais (GPU, áudio, HID, temporização) e adoptá-lo seria
  adoptar o emulador.

Por isso o `Core::System` nosso (`vendor/eden/core/system_ref.h`) ganhou um
accessor por serviço, e os controllers vivem nele.

## AOverride: porque é preciso

A árvore `vendor/` é uma cópia do Eden, e uma cópia volta a ser a cópia do
Eden sempre que se sincroniza. As nossas substituições precisam de
sobreviver a isso, por isso vivem em `tools/overrides/` — **fora** do
vendor — e `tools/aplicar_overrides.py` copia-as para o sítio. É o único
ponto do projecto onde se decide que um ficheiro do Eden é substituído por
um nosso, e porquê.

## O `ankerl` e o `<cstring>`

O `unordered_dense.h` do Eden, em modo STD_MODULE, usa a biblioteca padrão
directamente e precisa que ela esteja incluida. O Eden compila-o com
`stl.h` ao lado; nós não tínhamos o `stl.h`. Vendorizá-lo resolveu, de uma
vez, todos os serviços — a falha aparecia no `ankerl`, a milhares de
quilómetros do problema.

## Estado

Suite: **6 testes, todos a passar**. `ia_translate` 24 verificações,
`g610_gpu_exec` 20 (pixel 1.500000 na G610), `ia_maxwell` 13.

O guest inicializa o SDK e vai buscar os serviços. O próximo na fila é o
`appletOE` — que é o `am` do Eden, e o `am` é grande (8946 linhas) e
arrasta o núcleo de HID e o sistema de janelas.

---

# Do `apm` ao `appletOE`: o SDK entra no loop do jogo

## Onde estávamos

O SDK inicializava (`sm:Initialize`, `lm:OpenLogger`) e pedia o `apm`.
Depois pedia o **`appletOE`** — e o `sm:` ficava à espera dele, para sempre.

## O `apm`: cinco ficheiros do Eden, 280 linhas

O `apm` são `apm.cpp` (29 linhas), `apm_interface.{h,cpp}` e
`apm_controller.{h,cpp}`. A única dependência específica do Eden era o
`APMController`, e esse queria um `Core::Timing::CoreTiming&` que **nunca
usava** — o Eden diz isso nos TODOs do fim do ficheiro:

    // TODO(DarkLordZach): Actually signal core_timing to change clock speed.
    // TODO(Rodrigo): Remove [[maybe_unused]] when core_timing is used.

O controller foi reescrito por nós: sem o `CoreTiming`, e com o modo de
performance fixo em `Normal` (este motor corre um jogo, e o jogo não está
docked). O comportamento observável é o do Eden: `Config7` por omissão,
validação da configuração, e o mapa `Normal→Config7`, `FastLoad→Config13`,
`Partial→Config15`.

**O padrão de um serviço do Eden, que se repete:**

- o serviço é fino (o `apm.cpp` do Eden tem 29 linhas);
- o estado vive num *controller*, que o `Core::System` possui;
- a interface CMIF (os IDs dos comandos e as estruturas) é a parte que
  **tem de ser exacta** e vem do Eden tal e qual;
- o `Core::System` do Eden **não** é adoptado: tem ~40 acessores a
  subsistemas reais (GPU, áudio, HID, temporização) e adoptá-lo seria
  adoptar o emulador.

Por isso o `Core::System` nosso ganhou um accessor por serviço, e os
controllers vivem nele.

## O `appletOE`: nosso, e porquê

O `am` do Eden tem 8946 linhas e arrasta o **núcleo de HID** (o
`ButtonPoller` precisa de `hid_core/frontend/emulated_controller.h`) e o
**sistema de janelas** (`WindowSystem`). Trazer o `am` inteiro seria trazer
o emulador.

O que o SDK precisa no arranque é um conjunto pequeno de respostas. E o
ficheiro `src/services/applet_oe.cpp` é a versão fina: a **interface CMIF é
a do Eden, tal e qual** (os IDs dos comandos e o formato das respostas são o
contrato com o guest), e o **conteúdo** é o mínimo que um jogo precisa.

A estrutura (do Eden, `core/hle/service/am/service/`):

    appletOE  (o serviço)
      └─ IApplicationProxy        (comando 0 = OpenApplicationProxy)
           ├─ 0  GetCommonStateGetter
           ├─ 1  GetSelfController
           ├─ 2  GetWindowController
           ├─ 3  GetAudioController
           ├─ 4  GetDisplayController
           ├─ 10 GetProcessWindingController
           ├─ 11 GetLibraryAppletCreator
           ├─ 20 GetApplicationFunctions
           └─ 1000 GetDebugFunctions

**Os nove foram pedidos pelo SDK, e os nove foram entregues.**

## Quatro coisas que só se descobrem medindo

### 1. O `WaitSynchronization` não estava implementado

SVC 0x18 não existia no nosso despachante. O SDK ficou a chamá-lo, e o
motor respondia "não sei", e o guest repetia — 4000 traps, e a mensagem
era "o guest está em ciclo", que não parececia nada.

A ABI é a do Eden (`svc.cpp:1420`): x0 = o prazo em nanossegundos, x1 = o
ponteiro para o array de handles, x2 = o número de handles; a saída é o
índice do handle sinalizado.

**O motor não tem eventos que disparem.** O `am` do Eden sinaliza o evento
de mudança de estado do applet, e quem o sinaliza é o `WindowSystem` e o
`LifecycleManager` — subsistemas de vídeo e de janelas que aqui não
existem. Por isso a espera é devolvida como **expirada**
(`ResultTimedOut`, o que o Eden devolve quando o prazo passa sem nada
acontecer) e não como "satisfeita".

A primeira versão devolvia sucesso, e o SDK — que faz
`ReceiveMessage` + `WaitSynchronization` em ciclo — voltava a pedir
imediatamente: 1311 pedidos em poucos segundos, e o motor a 100% de CPU a
fazer nada.

### 2. O `MRS x, cntpct_el0` é ilegal no EL0 do Linux

O SDK lê o contador com `mrs x0, cntpct_el0` (em 0x82d22fd0) e o motor
respondeu "instrução ilegal".

O `cntpct_el0` é o contador **físico**, que só o EL1 lê; o `cntvct_el0` é
o **virtual**, que o Linux expõe ao EL0. O Eden faz a mesma coisa
(`nce/patcher.cpp:134-144`).

**E é o mesmo bit que o do `TPIDRRO_EL0`.** O `sysreg` do MRS está nos
bits [20:5], e os dois pares diferem no bit 0 do `sysreg`:

    MRS Xt, TPIDRRO_EL0 = 0x5E01  ->  MRS Xt, TPIDR_EL0 = 0x5E00
    MRS Xt, CNTPCT_EL0  = 0x5F01  ->  MRS Xt, CNTVCT_EL0 = 0x5F00

Um bit, nos dois casos — e por isso há **uma** regra e não duas.

O Eden ainda multiplica o valor por um factor, para converter de ticks do
host para ticks do Switch. **A escala não é feita aqui**, e a razão está
escrita no código: o que o jogo faz com o valor é medir tempo e semear um
gerador aleatório, e para as duas coisas o que importa é que o contador
seja **monótono** — e o `cntvct_el0` é.

### 3. Um Result inválido é pior do que um Result de erro

O SDK aborta ante um Result **inválido**:
`nn::result::detail::ConvertInvalidImpl` (medido em 0x82d37748) chama o
abort. E o nosso `ResultUnknown` — que é `0xFFFFFFFF` — é exactamente
isso.

Medido: o SDK pediu `ReceiveMessage`, o nosso serviço respondeu
`ResultUnknown`, e o SDK morreu num assert.

**A regra que daqui saiu, e que vale para todos os comandos deste
serviço:** um comando que sabemos responder dá `ResultSuccess` e o valor
certo; um comando que não sabemos responder dá `ResultSuccess` e saídas a
zero. **Nunca um Result de erro.** O Eden pode dar-se a esse luxo porque
implementa tudo; aqui, um Result de erro é um assert no guest.

A excepção é o `ReceiveMessage`, em que o próprio Eden devolve um Result
de erro **específico** — `Service::AM::ResultNoMessages` (`am_results.h`),
que é um Result **válido** (módulo 128) e que o SDK sabe interpretar.

### 4. Uma tabela de comandos que o grep apanhou mal

A primeira extração das tabelas do `am` do Eden usou um padrão que só
apanhava as entradas com `D<&...>`, e perdeu as entradas com `nullptr` —
que são comandos que o Eden regista e **não implementa**. A tabela do
`IWindowController` ficou com 5 comandos em vez de 8, e o SDK bateu logo
nela.

A tabela certa:

    {0,  nullptr, "CreateWindow"},
    {1,  GetAppletResourceUserId},
    {2,  GetAppletResourceUserIdOfCallerApplet},
    {10, AcquireForegroundRights},
    {11, ReleaseForegroundRights},
    {12, RejectToChangeIntoBackground},
    {20, SetAppletWindowVisibility},
    {21, SetAppletGpuTimeSlice},

**A regra para extrair tabelas: o padrão tem de apanhar as entradas com
`nullptr` também.** Um comando registado e não implementado ainda é um
comando que o guest pode pedir, e a resposta tem de ser um Result, não
silêncio.

## O log por comando

O `LOG_TRACE` do Eden só existe em compilações de depuração, e o `TRACE` é
o nível mais baixo: o filtro deixa-o de fora. E sem um log, um guest que
chama um comando em ciclo não diz **qual** comando é.

Por isso o `ServiceFrameworkBase::InvokeRequest` tem agora um
`LOG_WARNING` por comando. **O Eden não precisa disto** porque implementa
todos os comandos; aqui, cada comando que o jogo pede é uma peça em falta,
e o log é a lista de trabalho.

Foi ele que mostrou, de uma vez, o que o jogo pede:

    1311  ICommonStateGetter: comando 1 (ReceiveMessage)
       3  sm:: comando 1 (GetService)
       1  IWindowController: comando 1 (GetAppletResourceUserId)
       1  ISelfController: comando 91 (GetAccumulatedSuspendedTickChangedEvent)
       1  IpcController: comando 3 (QueryPointerBufferSize)
       1  IpcController: comando 0 (ConvertCurrentObjectToDomain)

## Onde estamos

O SDK:

1. inicializa (`sm:Initialize`, `lm:OpenLogger`);
2. pede o `apm` e recebe-o;
3. pede o `appletOE` e recebe-o;
4. pede as **nove** sub-interfaces do proxy do `am` e recebe-as;
5. sobrevive aos asserts (o `ResultUnknown` deixou de matar o guest);
6. e entra no **loop de mensagens do jogo**: `ReceiveMessage` → sem
   mensagem → esperar → repetir.

O passo 6 é o ponto onde o motor precisa de mais: o `am` real entrega
mensagens de applet (a mudança de foco, o pedido de saída, o botão HOME), e
o nosso `appletOE` não tem por onde as entregar — não há `Applet`, não há
`LifecycleManager`, não há `WindowSystem`. O `ReceiveMessage` devolve
`ResultNoMessages`, que é o que o Eden devolve quando não há mensagens, e
o jogo fica à espera de uma que nunca chega.

**É este o próximo passo, e está escrito com a precisão que a medição
permitiu:** o `am` precisa de um estado de applet (foco, mensagens) que
seja consultável, e o `ReceiveMessage` precisa de uma fila.

## O estado da suite

Suite: **6 testes, todos a passar**. `ia_translate` 24 verificações,
`g610_gpu_exec` 20 (pixel 1.500000 na G610), `ia_maxwell` 13.
a fila de mensagens do am, e o multi-wait de dominio que falta

O SDK ja recebe a mensagem de entrada no primeiro plano, e ja nao esta em
loop. O que falta e' a parede de espera: a fila de mensagens do SDK espera
por um multi-wait de dominio, e nao por um evento.

## A mensagem de entrada no primeiro plano

O SDK ficou preso a pedir `ReceiveMessage` sem parar. A mensagem que
faltava e' a de entrada no primeiro plano — `AppletMessage::ChangeIntoForeground`
(= 1), do enum do Eden (`am_types.h:125-147`).

No Switch, um applet e' lanado, o `am` mete-o em primeiro plano e
entrega-lhe essa mensagem. O SDK inicializa, e so depois de a receber e'
que continua.

Aqui nao ha `Applet`, nem `LifecycleManager`, nem `WindowSystem`. Mas a
mensagem e' uma **primeira vez**: o applet e' criado, e vai para o primeiro
plano. Por isso a fila tem uma mensagem, e so uma.

A fila vive no `AppletOE` — o objecto do servico — e e' partilhada por
referencia com o proxy e com as sub-interfaces. **Nao pode viver na
sub-interface**: cada `PushIpcInterface` cria um objecto novo, e a
mensagem seria entregue uma vez por sub-interface.

## O evento do sistema, ligado a mensagem

O Eden (`lifecycle_manager.cpp:31-34`):

    void LifecycleManager::PushUnorderedMessage(Kernel::KernelCore& kernel,
                                                AppletMessage message) {
        m_unordered_messages.push_back(message);
        this->SignalSystemEventIfNeeded(kernel);
    }

A mensagem diz **o que** mudou; o evento diz **que** mudou. O SDK
espera por um e le o outro. Por isso o `GetEventHandle` devolve **o mesmo**
evento que o `empurrar` sinaliza — e nao um evento novo a cada chamada.

O handle do evento vem do `KHandleTable::Add`: o `KEvent` nao tem
`GetHandle()`, porque o handle vive na tabela, e nao no objecto.

## A parede de espera: o multi-wait de dominio

O que o SDK espera no fim **nao e' um evento**. A fila de mensagens do SDK
— `nn::os::ReceiveMessageQueue` (0x82d21340) — espera por um
**multi-wait de dominio**:

    nn::os::InitializeMultiWaitHolder(MultiWaitHolderType*, MessageQueueType*,
                                      MessageQueueWaitType)   0x82d217c0
    nnosInitializeMultiWaitHolderForMessageQueue               0x82d24590

E o `IpcController` aparece na traça, com dois comandos:

    IpcController: comando 0 (ConvertCurrentObjectToDomain)
    IpcController: comando 3 (QueryPointerBufferSize)

**Isto e' a parede de espera**, e e' a mesma que ja estava escrita no
`PROGRESSO.md`: o `Wait` do Eden usa `KScopedSchedulerLockAndSleep`, que
e' o escalonador, e dorme a thread. Nao existe nada disso num motor de
execucao nativa.

O que falta, em concreto:

1. **os objectos de dominio** — o `IpcController` (`ConvertCurrentObjectToDomain`
   transforma um objecto normal num objecto de dominio, com o seu proprio
   ciclo de vida);
2. **o `MultiWait`** como objecto de dominio — o Eden tem
   `core/hle/service/os/multi_wait.cpp` e `multi_wait_holder.cpp` (ja
   vendorizados), e o `MultiWaitHolder` do SDK e' o mesmo mecanismo com
   outro nome;
3. **uma espera que bloqueie** — hoje o `WaitSynchronization` devolve
   `ResultTimedOut` porque nao ha como dormir a thread do guest sem
   escalonador.

**A peca 3 e' a difficult.** As pecas 1 e 2 sao o Eden a fazer o que o
Eden faz.

## O `WaitSynchronization` que devolve `ResultTimedOut`

A primeira versao devolvia sucesso — a espera dizia que tinha passado sem
ninguem ter sinalizado nada. O SDK, que faz `ReceiveMessage` +
`WaitSynchronization` em ciclo, voltava a pedir imediatamente: 1311
pedidos em poucos segundos, e o motor a 100% de CPU a fazer nada.

O Eden devolve `ResultTimedOut` quando o prazo expira sem nada
acontecer, e o SDK sabe o que isso significa. E' essa a resposta honesta.

## A receita das sub-interfaces

O `Devolve<T>` do proxy passou a receber uma **fabrica**, e nao a
construir o objecto ele proprio: so o `CommonStateGetter` precisa do
estado (a fila de mensagens), e um `make_shared<T>(system, estado)` a
todos seria uma assinatura que nenhum dos outros tem.

## O que o log por comando mostrou

    1311  ICommonStateGetter: comando 1 (ReceiveMessage)
       3  sm:: comando 1 (GetService)
       1  sm:: comando 0 (Initialize)
       1  lm: comando 0 (OpenLogger)
       1  IWindowController: comando 1 (GetAppletResourceUserId)
       1  ISelfController: comando 91 (GetAccumulatedSuspendedTickChangedEvent)
       1  IpcController: comando 3 (QueryPointerBufferSize)
       1  IpcController: comando 0 (ConvertCurrentObjectToDomain)
       1  ICommonStateGetter: comando 0 (GetEventHandle)

E o `IpcController` no meio dos comandos do `am` e' a pista: e' a
machine de dominios do IPC, e e' o que a fila de mensagens espera.

## Onde estamos

O SDK inicializa, pede o `apm` e o `appletOE`, recebe as nove
sub-interfaces, recebe a mensagem de entrada no primeiro plano, e espera
pelo multi-wait de dominio.

Suite: 6 testes. ia_translate 24, g610_gpu_exec 20 (pixel 1.500000), ia_maxwell 13.

---

# A resposta do IPC estava errada: o `rb{ctx, 2}`

O SDK parou de andar. E a causa nao era o mecanismo de dominio — era
**o tamanho da resposta**.

## A formula do Eden

`core/hle/service/cmif_serialization.h:475`:

    IPC::ResponseBuilder rb{ctx, 2 + Common::DivCeil(layout.cmif_raw_data_size, sizeof(u32)),
                            layout.copy_handle_count,
                            layout.move_handle_count + layout.domain_interface_count};

O `2` inicial e' o **Result**, e o Result ocupa **duas** palavras de 32
bits (`ipc_helpers.h:201-205`: o `raw` e um zero). O que vem depois
e' o *raw data* — e o que e' um handle **nao vai para o raw data**:
vai para a handle table, e o handle e' escrito no descritor de
handles.

O nosso `appletOE` usava `rb{ctx, 2}` em **todos** os handlers. O
Result ocupava as duas palavras, e o dado seguinte escrevia por cima
da segunda.

Medido, a resposta ao `GetEventHandle` era:

    00000000 0000000e 00000000 00000000 4f434653 ...

O `0000000e` (14) era o handle, no sitio do zero do Result. O guest
lia lixo — e o sintoma era "o guest esta em ciclo".

**A correccao:** 103 handlers. O script que a faz (`/tmp/verificar_rb.py`)
conta as palavras de cada `rb.Push` — `u32`/`s32` = 1, `u64`/`s64` =
2, `Result` = 2 — e ajusta o numero.

## O handle do evento era o objecto errado

O Eden devolve o **`KReadableEvent`** — o *filho* do `KEvent` — e e'
sobre ele que o guest espera (`os/event.cpp:33-35`):

    Kernel::KReadableEvent* Event::GetHandle() noexcept {
        return std::addressof(m_event->GetReadableEvent());
    }

Nos devolviamos o `KEvent`, o *pai*. E usavamos `rb.Push(handle)`
em vez de `rb.PushCopyObjects(ctx, obj)`, que e' o que o Eden faz
(`cmif_serialization.h:397`): o objecto e' posto na handle table do
processo, e o handle escrito no descritor de handles.

**O sintoma disto era `0x8000`** — o pseudo-handle da thread corrente
(`svc_common.h:29`), lido do sitio errado. Um numero que parece
valido e nao e'.

## A espera consultava o estado, ou nao consultava

O `WaitSynchronization` devolvia `ResultTimedOut` **sempre**. O Eden
(`k_synchronization_object.cpp:85-160`) consulta `IsSignaled` em cada
objecto da lista e, se algum estiver sinalizado, **termina ali, sem
dormir**:

    // Check if any of the objects are already signaled.
    for (auto i = 0; i < num_objects; ++i) {
        if (objects[i]->IsSignaled(kernel)) {
            *out_index = i;
            slp.CancelSleep();
            R_THROW(ResultSuccess);
        }
    }

O `KReadableEvent` do Eden tem o estado (`m_is_signaled`) e o
`IsSignaled()` — e o vendor ja os tinha, porque o `k_event.cpp` e' o
codigo do Eden. So faltava **usar**.

**E o `Wait` nao consome o sinal.** O `Clear` e' do **`am`**, no
`SignalSystemEventIfNeeded` (`lifecycle_manager.cpp:158-171`), e
acontece quando o `ShouldSignalSystemEvent()` deixa de ser
verdadeiro. A primeira versao fazia o `Clear` no `Wait` — e o guest
consumia o sinal e ficava a espera para sempre.

## O evento e' de nivel, e a regra e' do Eden

`lifecycle_manager.cpp:125-141`:

    bool LifecycleManager::ShouldSignalSystemEvent() {
        if (m_focus_state_changed_notification_enabled) {
            if (!m_is_application) {
                if (m_requested_focus_state != m_acknowledged_focus_state) {
                    return true;
                }
            } else if (m_has_focus_state_changed) {
                return true;
            }
        }
        return !m_unordered_messages.empty() || m_has_resume || ...;
    }

Traduzida para o que este motor tem: **a mudanca de foco** conta, e
**ha mensagens na fila**. E o `PopMessage` re-sinaliza depois de
tirar a mensagem (`lifecycle_manager.cpp:186-192`).

O que faltava no nosso motor era a **mudanca de foco**: o `am` do
Eden poe o `m_has_focus_state_changed` quando o applet e' lancado
(`applet_manager.cpp:342`):

    // Applet was started by frontend, so it is foreground.
    applet->lifecycle_manager.SetFocusState(m_system.Kernel(), FocusState::InFocus);

## O que mudou

O SDK deixou de pedir `ReceiveMessage` em ciclo — **1311 pedidos** —
e passou a **esperar por um evento**: e' o que o Eden faz, e o que o
guest faz num Switch.

Medido: `ICommonStateGetter: comando 1 (ReceiveMessage)` — **uma
vez**.

E o guest passou do loop de mensagens para o **loop de pedidos** do
`am` (`SendSyncRequest` + `SignalProcessWideKey`), que e' o passo
seguinte do arranque do SDK.

## O que ficou por explicar

O SDK espera por um **objecto de dominio** — o `object_id` que o
`ConvertCurrentObjectToDomain` devolve. E o `am` tem de o
**sinalizar** quando o estado muda, que e' o mecanismo de notificacao
push do Switch.

A pista: a sessao do `am` e' um **dominio** (medido: `dominio=1` no
`GetEventHandle`), e o `DomainMessageHeader` entra na resposta. No
Eden, o `am` nunca e' chamado por um dominio — o `ConvertToDomain`
converte a sessao do **`sm:`**, e o SDK usa esse dominio para
**pedidos ao `am`**.

**A regra do Eden, que e' a que resolve:** um objecto substituido
pela ponte nao pode coexistir com ela. O Eden compila o
`k_process.cpp` e o `k_thread.cpp` **reais**; nos usamos a ponte
`k_process_bridge.h`, e por isso os originais nao podem coexistir
com ela.

---

## A sessao `0x481f7`: o `am` fala por `object_id`

A sessao `0x481f7` nao respondia, e era a razao de o guest abortar.

**Medido.** O `AddMoveObject` do Eden (com o `PushIpcInterface` do
`ipc_helpers.h:114-131`) devolve a sessao nova, e o handle que o
guest ve e' o da **4.a** interface devolvida pelo `appletOE`. Nao e'
um servico nosso.

E o mecanismo e' o **dominio** do Eden: o `am` e' chamado por
`object_id`, nao por comando de servico. Medido:

    [ipc] HandleDomainSyncRequest: comando 0, object_id 5, handlers 10
    [service] HandleSyncRequest: servico "ICommonStateGetter" comando 0

Cada `object_id` responde com o **nome** do servico a que pertence.
Isto ja funcionava.

## A resposta de 0 palavras: `aoc:u`

A resposta de 0 palavras nao era do `am`. Era do **`sm:`**, a
receber `GetService("aoc:u")`:

    [eden:W] Service::SM: GetService: nome lido = "aoc:u"
    [eden:W] Service::SM: Server is not registered! service=aoc:u
    [eden:I] Service::SM: Waiting for service aoc:u to become available
    [ipc] resposta (0 palavras):

O `sm:` **deixa o pedido pendente** (`ctx.SetIsDeferred()`,
`sm.cpp:206`) ate o servico aparecer. Como o `aoc:u` nao existe
neste motor, o pedido fica pendente para sempre — e o guest
aborta.

**Os servicos que o guest pede e que este motor nao tem:**

| Servico | Onde |
|---|---|
| `aoc:u` | `sm:` — o `IAddOnContentManager` do Eden |
| `pctl`, `pctl:s`, `pctl:a`, `pctl:r` | `sm:` — o parental control |

**Medido, os servicos registados no `sm:`** (contador no
`RegisterService`): `lm`, `apm`, `apm:am`, `apm:p`, `apm:sys`,
`appletOE`. **Seis.** O `pctl` vendorizado nao aparecia: o
`pctl.cpp` do Eden regista-se por `LoopProcess`, e nao pelo
`RegisterNamedService` — e o `pctl` que adaptamos precisa de uma
registo explicito.

## A ABI do `SetHeapSize`: o guest e' a fonte da verdade

A implementacao tratava `x0` como o **tamanho** e `x1` como o
**endereco de saida**, com a justificacao de que "a fonte da verdade
para a ABI do guest e' o guest". **Medido no SDK, e o guest diz o
contrario.**

O modulo `sdk` desassemblado (`IA_DUMP_MODULE=sdk`):

    0x82d799ec — a wrapper do SVC 1:
        str  x0, [sp, #-16]!   ; guarda o ponteiro de saida
        brk  #0x1
        ldr  x2, [sp]
        str  x1, [x2]          ; escreve x1 no endereco x0

    0x82d29ee0 — nn::os::detail::MemoryHeapManagerImplByHorizon::SetHeapSize:
        mov  x0, x1            ; x0 do SVC = 1.o argumento
        mov  x1, x2            ; x1 do SVC = 2.o argumento
        bl   0x82d799ec

Ou seja: **x0 e' o ponteiro de saida, x1 e' o tamanho, e o endereco
base volta em x1.** E o Eden concorda (`svc.cpp:1250-1255`):

    static void SvcWrap_SetHeapSize64(Core::System& system, std::span<uint64_t, 8> args) {
        uint64_t size = Convert<uint64_t>(GetArg64(args, 1));
        uint64_t out_address{};
        Result ret = SetHeapSize64(system, std::addressof(out_address), size);
        SetArg64(args, 0, Convert<uint64_t>(ret));
        SetArg64(args, 1, Convert<uint64_t>(out_address));
    }

**O sintoma da versao errada tambem bate certo.** Com `x0` tratado
como o tamanho, o SDK lia `x1` (0x7e00000 — o tamanho) como se fosse
o endereco base, e escrevia la. Era o fault em 0x82d25ca0, com a
escrita a **0x7e00010**.

E o teste `test_core.cpp` **validava a ABI errada** — e passava. Um
teste que valida uma ABI inventada da confianca a uma coisa falsa.

**Correcção:** `x0` = ponteiro, `x1` = tamanho, o base escrito no
ponteiro **e** devolvido em `x1`. O motor ja copia `x1`-`x5` de
volta (`native_engine.cpp:604-605`), por isso nao foi preciso mexer
no motor.

## O `SetHeapSize` ja nem e' chamado

Com o `GetInfo(21)` a devolver o **tamanho** (e nao o fim da
reserva), o SDK **deixa de pedir** o heap: o `SetHeapSize` nao
aparece no trace. Antes pedia 2,4 GB, e o assert de crescimento de
vector do SDK (`!safe_add(&new_capacity, new_size, (new_size / 2))`)
transbordava.

E' por isso que a medicao antiga da ABI **nao provava nada**: o
caminho nunca foi exercitado depois da correccao do `GetInfo`.

## Onde estamos

O guest chega ao `am`, pede as sub-interfaces, e fica a espera dos
servicos que **nao existem**: `aoc:u` e `pctl:*`.

O proximo passo e' vendorizar o `aoc:u` do Eden
(`core/hle/service/aoc/`, 374 linhas) e registar o `pctl` pelo
`RegisterNamedService`.

---

## A limpeza da telemetria: uma regressao queIntroduziu

Ao limpar a telemetria de debug injectada no `vendor/`, o script de
remocao levou **linhas a mais**: o `return strong_ptr->HandleSyncRequest(...)`
do `hle_ipc.cpp` e o `if (info == nullptr ...) return ReportUnimplementedFunction(...)`
do `service.cpp` desapareceram com o log que os rodeava.

**Como se viu:** o `am` deixou de responder (30 pedidos de IPC caíram para
9), e a suite continuava a passar — porque nenhum dos testes exercita o `am`
por dominio.

**A verificacao que apanhou:** comparar **todo** o `vendor/` com a versao
anterior, e nao so' os ficheiros que se mexeram. O `git diff` face ao commit
anterior tem de mostrar **apenas** as diferencas intencionais.

Este e' o custo de um `str.replace` com um bloco grande: o bloco removido
leva com ele o que estava **a volta**. A regra que sai daqui: **quando se
limpa instrumentacao, compara-se o resultado com a base, linha a linha.**

## A ferramenta de separacao, e um bug dela

O `/tmp/separar.py` separava a historia em commits por assunto, e foi
promovido a `tools/separar.py`.

Tinha um bug: uma remocao que **nao** e' seguida de uma adicao (a remocao
de um bloco de linhas) **nao saia** do ficheiro. A regra interna era "a
remocao sai se a proxima linha nova foi escolhida" — e um bloco de remocoes
sem adicao nenhuma nao tem "proxima linha nova".

A correccao e' o modo `faixas`: `{"0": {"h": [[1, 999]]}}` marca as posicoes
do corpo do hunk que entram, e as remocoes dentro da faixa saem.

**Como se viu:** a arvore final da separacao diferia da original, e a
ferramenta **verifica isso** — a soma das fatias tem de dar o commit
original. Sem essa verificacao, a separacao publicava uma historia que
nunca existiu.

## A publicacao

O projecto foi publicado em **https://github.com/gordaogameplay/EdenMaliG610**
(publico, GPL-3.0-or-later), com a historia separada por assunto:

```
cfaad82  o `.gitattributes`: normalizacao de fim de linha
e3a0bca  o `PROGRESSO.md`: as medicoes de hoje
94adc95  a telemetria: o dump do modulo, a assercao do SDK e o endereco do fault
325ec00  o `am`: a mensagem de foco e o tamanho da resposta
1eddd28  o heap do guest: a ABI do `SetHeapSize` e o `GetInfo(21)`
262c219  o `pctl` do Eden, vendorizado: o parental control
3d3372e  o README: o que e' isto, o que e' do Eden e o que e' nosso
bb65f12  a licenca GPL-3.0 e os cabecalhos SPDX nos ficheiros nossos
```

**Os 8 commits foram compilados no Pi, um a um** — e todos compilaram.

O `README` diz o que o projecto e', o que e' **nativo** e o que e'
**traduzido**, **o que foi tomado do Eden e o que foi escrito aqui**, e que
o projecto e' escrito com assistencia de IA e revisto por uma pessoa.

## Onde estamos

O guest chega ao `am`, abre o proxy da aplicacao e as sub-interfaces, e
fica a espera dos servicos que **nao existem**: `aoc:u` e `pctl:*`.

O `aoc:u` (`IAddOnContentManager`, 374 linhas no Eden) e' o proximo: o Eden
implementa-o com o `ContentProvider` e o `PatchManager` — o emulador de
ficheiros. Num motor nativo nao ha DLC, e a lista e' vazia; o que o guest
precisa e' que o **servico exista** e responda.

---

## O `aoc:u` e o `pctl`: os dois servicos que faltavam

**O `aoc:u` travava o arranque do guest.** O `sm:` respondia *"Waiting for
service aoc:u to become available"* e **ficava bloqueado**
(`sm.cpp:200-203`, `ctx.SetIsDeferred()`). Enquanto o `sm:` espera, nenhum
pedido seguinte e' atendido — e os pedidos do `pctl` ficavam na fila, sem
o log do `sm:` mostrar sequer o nome deles.

**E o `pctl` estava vendorizado mas nunca registado.** O `LoopProcess` — que
e' o que regista as quatro portas no `sm:` — nunca era chamado. O codigo
estava compilado e nunca executado, e o guest nao dizia nada: um pedido
pendente **nao e' um erro, e' uma espera**.

**E os handlers do `pctl` usam `D<>` (dominio), nao `C<>` (sessao
normal).** O Eden usa `D<>` em todos. Uma versao anterior deste projecto
trocou para `C<>` por inferencia — a resposta saia com 0 palavras — e a
inferencia estava errada. Medido: com `C<>`, o SDK recebia um ponteiro que
nao era um objecto de dominio e abortava com `this == NULL`.

## O layout: o `main` linkado para 0x80000000

**O jogo linkou o `main` para 0x80000000, e este motor punha-o em
0x80005000.** A diferenca — 0x5000 — era o que fazia o jogo abortar num
assert **seu**.

Medido: o `adrp` do jogo e' relativo ao PC, e o valor que ele dava era
0x5000 acima do linkado. A string da assercao do jogo (*"SuperStripe
instance is null"*) esta, no guest, em 0x81b06601; o linkador a puser em
0x81b01601.

Duas causas:

1. O `rtld` ocupava 0x5000 (a imagem **do ficheiro**, 0x3DA6, mais o
   `.bss`) quando devia ocupar 0x4000 (o **fim do ultimo segmento**,
   0x3280, mais o `.bss` — que e' o que o Eden mede, `nso.cpp:131`).
2. O `rtld` ocupava o sitio do `main`. O `rtld` (o `ldr`) e' linkado em
   **PC-relativo**; o `main` e' linkado para um sitio **fixo**, e o `ldr`
   le-o de uma tabela (medido: **nove ocorrencias de 0x80000000** nos
   dados do `rtld`).

## O heap de 8 GiB, e o `MAP_NORESERVE`

O Eden, para 39 bits, reserva **8 GiB** de heap
(`k_address_space_info.cpp:36`), e o `GetInfo(21)` devolve o
`HeapRegionSize` do processo — que sao esses mesmos 8 GiB.

**E o `mmap` de 8,75 GiB precisa de `MAP_NORESERVE`**: o overcommit
heuristico do Linux recusa a reserva num Pi de 7,7 GiB, e o guest nem
chegava a arrancar.

## Medicoes

| Medida | Valor |
|---|---|
| Despacho de SVC | **115,9 ns** por chamada (**8,6 M/s**) |
| Arranque do guest ate ao abort | **32,0 – 32,9 ms**, com **165 SVCs** |
| Faults do guest | **0** |
| Suite | **6/6** |

O despacho e' medido por `g610_bench_svc` (20 milhoes de chamadas); o
arranque, pelo instante do `Break` que o `boot_trace` regista.

## Onde estamos

O guest chega ao `am`, pede as sub-interfaces, e o `am` responde. Os
servicos todos respondem — `lm`, `apm`, `appletOE`, `aoc:u`, `pctl` e as
suas tres variantes.

O que falta e' o **polling**: o SDK pede e volta a pedir, a espera de um
evento que o `am` nao sinaliza. Medido: 74 `SignalProcessWideKey` e 53
`SendSyncRequest` antes do primeiro assert.

E o assert, que agora e' **do jogo** e nao um artefacto do motor: o jogo
verifica um byte do seu objecto (`ldrb w8, [x21, #8]; cbz w8, ...`) e
aborta se ele nao for zero.

---

## O `am` esta completo — e o bloqueio restante e' o sistema de ficheiros

**A regra do Eden para a mudanca de foco** estava a faltar, e o sintoma
era o mais enganador de todos: o guest **nao abortava**, ficava a espera.

Medido antes: **3936 `WaitSynchronization` seguidos**, ate ao limite de
traps. O `WaitSynchronization` devolvia `ResultTimedOut` (o motor nao
dorme) e o guest repetia — porque o evento do sistema do `am` **nunca era
sinalizado**.

Tres correccoes, todas da regra do Eden:

1. **`m_focus_state_changed_notification_enabled` comeca a `true`**
   (`lifecycle_manager.h:159`) — o nosso comecava a `false`;
2. o **`PopMessage` so' consome o `m_has_focus_state_changed` se o notify
   estiver ligado** (`:47-63`) — o nosso consumia-o logo no primeiro
   `ReceiveMessage`, e o `SetFocusHandlingMode` (que vem **depois**) ja nao
   encontrava nada para sinalizar;
3. o **`SetFocusHandlingMode` le os tres bools** (`notify`, `background`,
   `suspend`) — o Eden, `self_controller.cpp:187`. Os bools sao
   **empacotados** (um byte cada), e o `Pop<u8>` avanca uma palavra
   inteira: foi preciso um `PopBools`.

**E um bug nosso, medido:** o `WaitSynchronization` lia o **timeout de x0** —
que e' o ponteiro de saida. A ordem do Eden (`svc.cpp:1420`) e': x0 = ponteiro
de saida, x1 = array de handles, x2 = numero, **x3 = timeout**. O trace
mostrava "timeout=2222980920 ns", que era o ponteiro.

**Depois disto:** o `WaitSynchronization` passou a **1**, e o
`ReceiveMessage` devolve a `FocusStateChanged`. A sequencia de comandos do
`am` e' agora a do Eden, toda.

## O abort que resta e' do jogo, e e' um ficheiro em falta

> **Refutado** em 2026-10-20 — ver a sessão no fim deste ficheiro. O
> ficheiro nao esta no NSP, a string do abort e' do SDK e nao do jogo, e
> o abort e' uma excepcion C++ por apanhar. A leitura abaixo fica como
> registo do caminho, nao como diagnostico.

O guest aborta num assert **seu** — a mensagem e'
*"SuperStripe instance is null"*, mas o `x3` (o endereco que o SDK
reporta) aponta para o fim de outra string:

    pane_L_btlspot_button_00_T_button_l_00.bin

O codigo do jogo (desassemblado, `0x80005d70`):

    80005d88: ldr x8, [x23]          ; o singleton actual
    80005d8c: str x0, [x23]          ; guarda o novo
    80005d90: cbz x8, 0x80005dcc     ; se ja existia, nao destroi
    ...
    80005dc8: ldr x0, [x23]
    80005dcc: add x8, x0, #0x48
    80005dd0: cmp x0, #0x0
    80005dd4: csel x0, xzr, x8, eq
    80005dd8: bl 0x805cf9c0          ; <-- aborta

**O singleton e' nulo** (o `csel` escolhe 0), e a funcao aborta. O
`x21` (o objecto cujo byte e' lido) e' o **`SuperStripe`** do jogo.

**E o ficheiro:** o jogo precisa de
`pane_L_btlspot_button_00_T_button_l_00.bin` — um **ficheiro de dados**.
O nosso motor **nao tem sistema de ficheiros**: nao ha `fs`, nao ha `fss`, e
nao ha o **`bcat`** (o *delivery cache*), que e' o que da os ficheiros ao
jogo (`nn::bcat`, 25 simbolos no `.dynstr` do jogo).

**O `bcat` do Eden:** `core/hle/service/bcat/` — 1098 linhas, com um
`backend` que le do `FileSys` (o emulador de ficheiros). **Para um motor
nativo, o backend tem de servir os ficheiros que o jogo tem** — e, numa
copia do jogo, o *delivery cache* esta **vazio**. Logo, um `bcat` que
responda com um directorio vazio pode ser suficiente.

**O caminho ate a imagem, por ordem:**

| Passo | O que e' | Tamanho (Eden) |
|---|---|---|
| 1 | o `bcat` (delivery cache) | 1098 linhas |
| 2 | o `fs`/`fss` (sistema de ficheiros) | ? |
| 3 | o `nv::` (NVN) — a apresentacao | 3034 linhas |
| 4 | o gralloc e a apresentacao | 173 + ? |
| 5 | `bsd`/`ns`/`pm`/`time`/`hid`, conforme o jogo pedir | ? |

**O passo 1 e' o proximo.**

---

# Sessão 2026-10-20 — o abort não é um ficheiro em falta

## O que a hipótese anterior dizia, e o que a medição diz

A secção anterior fechou com *"o abort que resta é do jogo, e é um ficheiro
em falta"* (`pane_L_btlspot_button_00_T_button_l_00.bin`). **Essa hipótese
está refutada**, por três medições independentes:

1. O ficheiro **não está** no NSP. A listagem do PFS0 (script próprio no Pi)
   não devolve nenhum nome assim — o NSP é um PFS0 com NCAs, e a busca por
   essa string dá 0.
2. A string do abort **não é** do jogo. O `x7` do `Break` aponta para
   `0x831c7f6d`, que é o `.rodata` do **SDK** (offset do módulo `0x9c9f6d`).
3. O abort **não é um assert do jogo**: é uma **excepção C++ por apanhar**
   (ver abaixo).

O que o abort é, medido:

    [brk] registos: x0=0x0 x1=0x84c639f8 x2=0x4 x3=0xe ...
    [msg ] strings na stack do guest:
    () called _Unwind_RaiseException() which unexpectedly returned

O SDK lança uma excepção, o *unwinder* chega ao fim da stack sem encontrar
quem a apanhe, e o `std::terminate` aborta. A string
*"() called `_Unwind_RaiseException()` which unexpectedly returned"* é do
próprio *unwinder* (libunwind), e **não** é a mensagem do erro: está a
0x40 bytes de outra string no mesmo `.rodata`.

## Quem resolve as importações do `main`: o `rtld` do próprio jogo

Medido no GOT do `main` (`0x8249'49a0`, 821 slots de PLT):

* **43 dos 821** slots estão resolvidos; os outros 778 valem `0x817e8f90`,
  que é o **PLT0** — o *lazy binding* do `rtld`.
* Os resolvidos batem certo com os símbolos do SDK, verificado por
  desassemblagem: o slot 4 aponta para `0x82cf5a70`, que é a `memset` do
  SDK (código de `memset`, byte a byte); o slot 16 para `0x82b25f80`, que é
  `nn::os::GetTlsValue` (`mrs x8, tpidr_el0`); o slot 9 para
  `nn::os::QueryMemoryInfo`.

**Conclusão: o nosso motor não resolve importações — e não deve.** O `rtld`
do jogo faz isso, por nome, contra as tabelas `api_info` do SDK. (O
resolver do motor foi removido no commit `2d195c3`: a correspondência
PLT↔`dynsym` não é posicional.)

## O SVC 0x0f era o que matava o jogo

O SDK chama `nn::os::SetThreadCoreMask` (SVC `0x0f`). O motor não o tinha
implementado: caía no `default` e devolvia `Result::Unsupported` — que no
nosso enum é **64 = 0x40**.

E o SDK trata esse Result como fatal. Medido, pela ordem:

| SVC | o que é |
|---|---|
| `#164` | `0x0f` `SetThreadCoreMask` → `Unsupported` (0x40) |
| `#166` | `SendSyncRequest` → `IApplicationFunctions` comando 22 (`SetTerminateResult`) com **0x40** no pedido |
| `#168` | `0x26` `Break` — o abort |

O `0x40` do `SetTerminateResult` **é** o nosso `Unsupported`: não há outra
fonte para ele. Implementado o SVC (com a validação do Eden,
`kernel/svc/svc_thread.cpp:252-280`), o jogo passa a andar.

## Os sub-SDKs, e a stack

O ExeFS tem `subsdk0` e `subsdk1`; o Eden carrega-os
(`deconstructed_rom_directory.cpp:191`). Passaram a ser carregados, e a
stack passou a ser colocada **depois** do código (no Switch a região da
stack é sorteada pelo kernel — o que o guest sabe é o endereço que o
kernel lhe deu).

| | antes | depois |
|---|---|---|
| arranque | 39,8 ms | **68,2 ms** |
| SVCs | 680 | 686 |
| módulos mapeados | 3 | 5 |

## Onde estamos agora: uma excepção do SDK, sem nenhum Result de erro

Com o log novo (o Result de cada SVC, e a resposta de cada IPC):

* **Nenhum SVC devolve erro.**
* **Todas as respostas IPC têm `Result = 0`** (o Result está na palavra 10
  da resposta, depois do `DataPayloadHeader` "SFCO").

E, na mesma medida, o que o SDK faz antes de abortar:

* `IParentalControlService` comando 1 (`Initialize`) → **sucesso**
  (`[pctl] Initialize: capability=303`).
* `GetInfo(22)`, `GetInfo(21)`, `SetHeapSize(0x1ffe00000)`.
* **527 chamadas a `svcQueryMemory`** — esta é a recolha de informação do
  relatório de crash do SDK (`nn::diag::GetAllModuleInfo`, que varre o
  espaço do processo a procurar módulos com cabeçalho `MOD0`). É
  **sintoma**, não causa: é o SDK a preparar o relatório.
* `SetTerminateResult` com `0x2a2` (módulo Kernel, descrição 162) e o
  `Break`.

### Hipóteses refutadas hoje

| Hipótese | Como foi refutada |
|---|---|
| `GetInfo(10)` (`IdleTickCount`) é o que falha | O trace **não tem** nenhuma chamada `GetInfo` com tipo `0xa`. |
| O `pctl.Initialize` devolve um Result mau | A resposta tem `Result = 0`; o Eden registou `capability=303` e não houve log de erro. |
| O caminho é o das `nn::friends` | O GOT do `main` não tem **nenhum** símbolo de `friends` resolvido. As strings `nn::friends::GetFriendCandidateList` e `nn::friends::SyncUserSe` estão simply **vizinhas** no `.rodata` do SDK. |
| O `rtld` do jogo não resolve as importações | 43 slots resolvidos, verificados por desassemblagem. |

### O que falta saber

As 527 chamadas a `svcQueryMemory` do relatório de crash probed **endereços
espalhados dentro da região da stack** (medido: `0x8411a000`,
`0x8412a000`, `0x84185000`, `0x84200000`, `0x8428e000`, `0x842a4000`,
`0x8448f000`, `0x845e7000`, …). Endereços espalhados = o SDK está a
**seguir ponteiros**, e a lista está na stack. Isso aponta para uma
estrutura do SDK com conteúdo errado — mas *onde* ainda não está medido.

**O passo seguinte, e é uma decisão:** seguir a lista de ponteiros que o
SDK percorre (o `x30` de todas as 527 chamadas é o mesmo —
`sdk+0x332a13c`, uma função local, sem símbolo exportado), e ver o que
ela lê. Ou, em alternativa, comparar com o mesmo jogo correndo no Eden
completo, que é a fonte da verdade do projecto.
