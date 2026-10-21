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

## Passo 5 — `WaitProcessWideKeyAtomic` (0x1C) e `SignalProcessWideKey` reais

O passo 4 fechou os locks (`ArbitrateLock`/`ArbitrateUnlock` à medida), mas as
threads batiam `trap #28` → `Unsupported (0x40)`: o guest pede
**`WaitProcessWideKeyAtomic` (0x1C)** logo após o drop do `SendSyncRequest`.
As semânticas foram lidas por inteiro no futuro Eden, no Orange Pi
(`k_condition_variable.cpp`):

- `Wait` (:271): o PRIMEIRO waiter do endereço do mutex torna-se dono
  (`next_value` = tag dele, +máscara se houver mais); escreve **1 na palavra
  do `cv_key`** + fence seq_cst; SÓ DEPOIS escreve a palavra do mutex;
  `timeout == 0` devolve `TimedOut` DEPOIS do release; timeout < 0 espera
  sem fim.
- `SignalImpl` (:198): `UpdateLockAtomic` na palavra do waiter acordado —
  palavra 0 ⇒ escreve o TAG dele e desperta-o **já dono**; palavra ≠ 0 ⇒
  escreve `prev | 0x40000000` e a thread vira **waiter do dono actual**
  (passa da fila de cv para a fila do endereço SEM acordar).
- `Signal` (:~247): consome até `count` (≤0 = todos) e, quando a fila da
  chave esvazia, escreve **0** na palavra do `cv_key` — inclusive quando
  nunca houve waiters.
- ABI confirmada pelo trap medido: `x0=endereço do mutex, x1=cv_key
  (endereço), x2=tag (0x18002 = handle nosso), x3=timeout (-1 = forever)`.

Implementação (`src/svc_dispatcher.cpp`): o quadro ganhou fila por `cv_key`
(`cv_fila`) e estado de despertar por token (`desperta`): quem entrega
escreve o resultado do `EndWait` por token — **um só canal** para lock e cv,
porque o SignalImpl move o waiter de fila sem acordar e o antigo predicado
"saiu da fila" não exprime esse estado.

**Medido no Orange Pi (3 boots, `IA_NOOB=1 stdbuf -o0`; 1 com `IA_IPC_TRACE=1`):**
- `ctest` 6/6 no Pi.
- `Unsupported (0x40)` do 0x1C: **0 ocorrências** (antes: cada trap #28
  devolvia 0x40).
- `[thrd] trap` impressos nas threads de guest: **5** por boot (antes 11/11/12).
- run com trace: **3 waits reais de 0x1C**, **87 SignalProcessWideKey**, 59
  `SendSyncRequest`, 16 respostas SFCO, 414 varrimentos do pump.
- O boot **não acaba sozinho** (`timeout` mata aos 90-120 s); o force-stop do
  amostrador segue a mesma rota ("última trap: breakpoint"), e as amostras
  agora veem **PC do guest** (antes: dentro do kernel): `0x8390d1e8` = o
  `b .+0` infinito do SDK, logo depois de `svc #0x26` = **`Break`**
  (objdump sobre `text.bin`; o mapeamento validado com a referência
  `svc #0x1a` @0x37eb2c).
- A cadeia fecha: **1 send fantasma por boot persiste** (o pump devolve
  `Kernel::ResultNotFound` = módulo 1, desc 121 = valor 61953 nos 6
  servidores — a mensagem "não é sessão" do `last_ipc_` era rotulação errada;
  a rota real é `ProcessPendingSynchronously != Success`), o guest recebe
  `InvalidHandle` a torto e **aborta via `svc Break`** → o wrapper do SDK
  entra no `b .+0` infinito.
- O fim do boot agora pendura (exit 124): thread estacionada no cv nunca é
  cancelada na desmontagem. O teardown vinha doente já no passo 3 (exit 139
  SIGSEGV); o sintoma mudou, a doença não.

Próxima parede: derrubar o **drop fantasma do pump** — medido a 1/boot
(65 progressos vs 283 NotFound em 348 varrimentos).

---

## Passo 5-b — a primeira imagem na TELA (apresentador)

Decisão de apresentação, medida e não por gosto: **Wayland**. A sessão gráfica
do board É sway (pid 98746, HDMI-A-1); não há sessão X pura (pgrep Xorg = 0) —
o que existe é o gateway **Xwayland** (pid 632056), um tradutor a mais por
frame; Vulkan WSI directo no Wayland é o caminho nativo do compositor. A
ferramenta (`tests/mostra.cpp`, alvo `g610_mostra`) usa **SDL2** (já PUBLIC no
`g610_core`, 2.30.0) — o SDL cria a janela e chama o WSI: zero protocolo
wl/xdg-shell à mão. A cadeia de shaders do MOTOR segue no `test_gpu_exec`
(20/20); este apresentador é outra cadeia, só de apresentação.

**A imagem na HDMI-A-1, medida por captura** (`grim`, analise com `pix.py`):
triângulo laranja sobre fundo `#101720`; centro da captura (255, 115, 26) =
laranja exato; cantos (16, 23, 32) = fundo exato; 15.9% de pixels laranja,
bbox x 435..1485, y 300..985 em 1920×1200. **903 quadros em 15012 ms =
60.1 fps** no FIFO/vsync (panvk + Mali-G610 + Wayland, janela full-screen
sobre o compositor do board).

Cinco bugs/detalhes medidos a caminho, todos do "precisa de medir", não de
"deve ser":

1. `std::setenv` não existe (é POSIX global, por isso `::setenv`); sem
   `<cstdint>` o resto é cascade de "not declared".
2. O env: o ssh traz `XDG_RUNTIME_DIR=/run/user/1000` (sessão tty) e NÃO traz
   `WAYLAND_DISPLAY` — sem ele libwayland tenta `wayland-0`, não existe.
   **O compositor esteve são o tempo todo**; com os dois envs certos, 10/10
   capturas. O `mostra.cpp` agora seta os dois (os valores alvo são os mesmos
   dentro da sessão do sway).
3. panvk reporta `currentExtent = 0xFFFFFFFF` no primeiro query (superfície
   Wayland ainda não configurada); passa-se o extent da janela SDL.
4. `pMultisampleState` nunca declarado no `ppci` → segfault do panvk
   (campo obrigatório; sem validação, nada avisa).
5. O primeiro acquire chega `VK_SUBOPTIMAL_KHR` (1000001003) — o swapchain
   nasceu antes da configuração da superfície. Dance WSI padrão: o
   apresentador recria swapchain no `OUT_OF_DATE`/`SUBOPTIMAL`
   (`construir_swap`/`destruir_swap`), com viewport/scissor dinâmicos.

Honesto: é o apresentador do caminho de render (SDL→WSI→panvk→Mali→
compositor→HDMI). **A imagem do JOGO fica atrás do drop fantasma do
`SendSyncRequest`** — passo 6. Nota: se corrido em fundo e o ssh fecha no
meio, o teardown segfaulta; em primeiro plano a saída é limpa (exit 0).

## Passo 6 (aberto) — o fatal **estanca no `SetTerminateResult`** do app

Medição nova (boot_p6 + boot_p6_r2, e conferido contra os três boot_p5_r1..3 —
todos param no MESMO ponto; o "16 SFCO" de antes era de outra passagem e não
deve ser usado como estado):

- O app chega a `IApplicationFunctions: comando 22 (SetTerminateResult)` — o
  caminho de erro fatal do SDK nn. A resposta sai com o cookie `SFCO`
  (0x4F434653).
- Depois disso o pump do servidor é chamado e fica **vazio**:
  `pump servidor=6 Result=61953 (progresso=0)` — nada pendente.
- Logo em seguida o guest chama `svc #0x26 Break` (x0=0x40) e o thread entra
  no spin infinito `0x8390d1e8` (`b .+0` da SDK, medido por objdump) — é aí
  que o boot pendura (exit 124).
- Circunstância suspeita da hora da morte: um `SignalProcessWideKey
  key=0x8434ed4c count=-1` corre com **acordados=0** imediatamente antes do
  Break — se alguém dormia nessa cv, devia acordar; acordados=0 sugere que o
  thread que devia consumir o resultado nunca ficou na fila.

Filho do passo 6: apurar (a) o contexto de quem chama SetTerminateResult
(qual resultado o app recebeu falhado), e (b) a fila de waiters no instante
do `acordados=0` — o instrumento está no quadro `g_arbiter`
(`src/svc_dispatcher.cpp`).

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

---

# Sessão 2026-10-20 (continuação) — o SDK precisa de espaço livre, e de uma thread

A secção anterior parou num "Result de erro que vem de um serviço". A
medição seguinte mostrou que **não vinha de nenhum**: o log novo (o Result
de cada SVC, a resposta de cada IPC) diz que **nenhum SVC devolveu erro** e
que **todas as respostas IPC têm `Result = 0`**.

O que havia sobrado era o SDK a **procurar espaço** no espaço de endereçamento
do processo.

## O que o SDK estava a fazer (medido, no `sdk` desassemblado)

A função que faz as 527 `svcQueryMemory` é
`nn::os::detail::AddressSpaceAllocator::AllocateSpace` (offset `0x32c0b0` do
`subsdk`/SDK). O código, lido:

```
 32c0d0: ldr  x9, [x0, #24]        ; o[24]
 32c0d4: add  x8, x1, #0xfff
 32c0dc: add  x8, x9, x8, lsr #12  ; o total, em paginas
 32c0e0: ldp  x22, x9, [x0, #8]    ; o[8], o[16] — os limites da regiao
 32c0e8: subs x23, x9, x22        ; o span (ja com o total descontado)
 32c118: udiv x8, x0, x23
 32c120: msub x8, x8, x23, x0      ; x0 % span   <- ALEATORIO
 32c128: add  x8, x8, x22
 32c12c: and  x27, x8, x25        ; alinha
 32c130: lsl  x21, x27, #12       ; o endereco candidato
 32c138: bl   0x37ea24            ; svcQueryMemory(candidato)
 32c13c: ldr  w8, [sp, #24]       ; o ESTADO do bloco devolvido
 32c140: cbnz w8, 0x32c158        ; se nao for Free -> outra tentativa
 32c148: add  x8, x21, x26        ; candidato + total
 32c14c: add  x9, x10, x9         ; base + tamanho do bloco
 32c150: cmp  x8, x9
 32c154: b.ls 0x32c194            ; cabe -> sucesso
 ...
 32c164: cmp  w24, #0x200          ; ate 512 tentativas
```

Três coisas, todas medidas:

1. O candidato é **aleatório** (daí os endereços espalhados) e está dentro da
   **região da stack** (os limites `o[8]`/`o[16]`).
2. O SDK **exige um bloco com estado `Free`** — o `ldr w8, [sp, #24]` lê o
   campo `state` do `MemoryInfo` que o SVC devolve.
3. Tenta **512 vezes** e depois desiste.

## Os três bugs que daí saíram

| | o que estava | o que é (fonte) |
|---|---|---|
| o bloco livre | `base=0, size=0` | o bloco `Free` **com o tamanho** (`k_memory_block_manager.h:129`) |
| a região da stack | 8 MiB, toda `Stack` | **2 GiB**, e só a stack da thread (1 MiB, do NPDM) é `Stack`; o resto é `Free` (`k_address_space_info.cpp:56`) |
| o alias | `Normal RW-` (mapeado) | `Free` — é de lá que o SDK aloca |

Medido: a tempestade de **527 `svcQueryMemory` desapareceu** e o arranque
passou de 39,8 ms para **62,8 ms**.

## E depois: a thread

O SDK cria uma thread logo a seguir (o `entry point` medido está no
`subsdk1`, `0x83abb240`) e arranca-a. Nem o `CreateThread` (0x08) nem o
`StartThread` (0x09) existiam no motor: devolviam `Unsupported` (64 =
0x40) — e o SDK trata isso como fatal.

Implementados **como stubs**, com a ABI do Eden (`svc.cpp:1304` e `:1367`):
a thread fica na nossa tabela de handles (para o handle ser válido), mas
**não corre**.

Duas correções que a medição obrigou no caminho:

- a prioridade vai de **0 a 63** (`kernel/svc_types.h:101-102`), não de 0 a
  31 — o guest passou 0x2C (44);
- o `SetThreadCoreMask` que vem a seguir ao `CreateThread` traz **o handle
  da thread nova**, e devolvia `InvalidHandle`.

## Onde estamos

| | no início da sessão | agora |
|---|---|---|
| arranque | 32 ms / 168 SVCs | **62,8 ms / 218 SVCs** |
| SVCs com erro | (não medido) | **0** |
| respostas IPC com erro | (não medido) | **0** |

O que falta: **a thread de guest tem de correr**. O SDK cria-a, arranca-a,
e depois espera por ela (e o jogo morre à espera). Este motor corre o guest
nativamente, numa thread do host, com as traps resolvidas por sinais: uma
segunda thread de guest exige que o estado do despachante, o TLS e o
`TPIDR_EL0` sejam **por thread** — e hoje não são. Esse é o próximo passo,
e é o mesmo trabalho que falta para o motor ter scheduler.

---

# Sessão 2026-10-20 (terceira parte) — a thread de guest

## O que o SDK pede

Com o `CreateThread` (0x08) e o `StartThread` (0x09) implementados, o guest
chega mais longe: o SDK cria uma thread e **usa-a** (faz-lhe
`GetThreadId` e `SetThreadCoreMask`), e só depois morre.

O pedido medido:

    [svc #180] 0x08 CreateThread  x0=0x30fffea08  x1=0x83abb240
                                   x2=0x13f20fc60  x3=0x2d0e57fe0
                                   x4=0x2c (prioridade)  x5=1 (nucleo)

A ABI é a do Eden (`svc.cpp:1304`): `x0` = ponteiro de saída, `x1` = entry
point, `x2` = argumento, `x3` = o topo da stack do utilizador
(`k_thread.h:737` chama-lhe `user_stack_top`), `x4` = prioridade (0 a 63,
`svc_types.h:101-102`), `x5` = núcleo.

## O `entry point` é uma função válida

Medido no dump do boot_trace (o código **tal como o motor o tem em
memória**):

    f3 0f 1e f8   stp  x24, x23, [sp, #-16]!
    fd 7b 01 a9   stp  x29, x30, [sp, #984]!
    fd 43 00 91   mov  x29, sp
    41 d0 3b d5   mrs  x3, tpidrro_el0     ; o TLS do guest
    21 e0 07 91   mov  x7, sp
    20 00 00 f9   str  x0, [x0]
    1f 20 03 d5   msr  tpidrro_el0, x3     ; restaurar o TLS do host
    ...

**O `hactool` descomprime o `subsdk1` de outra forma** — com ele, o
endereço parecia o meio de uma função; com a nossa descompressão, é um
prólogo. A nossa memória é a que conta.

E a função diz o que a thread precisa: `mrs x3, tpidrro_el0` (que o
patcher reescreve para `tpidr_el0`) — logo, **o `TPIDR_EL0` do guest**.

## O prototipo foi revertido — e porque

Com a thread a correr (uma thread do host a correr o entry point, com o
`sp` que o guest deu e o `TPIDR_EL0` apontado para o TLS dela), o código do
guest **dá fault logo a entrar** e o fault matava o processo. Duas
medidas:

| Sintoma | Causa |
|---|---|
| `exit 70` ("sem ponto de regresso") | o `sigsetjmp` estava numa frame que **já tinha regressado**; um `siglongjmp` para um `sigjmp_buf` de uma frame morta é comportamento indefinido |
| o guest lia o TLS do *host* | o `TPIDR_EL0` era global, e o motor tinha ficado a 0 — o guest escrevia por cima da `libc` |

E a lição de fundo, que é a resposta à pergunta "o Eden não faz?":

**O Eden faz, e melhor.** Tem `KThread` (objecto de kernel com TLS e
contexto próprios) e um *scheduler*. **O nosso `vendor` não tem
`KThread`** (medido: `ls vendor/eden/core/hle/kernel/ | grep -i thread` não
dá nada), e por isso:

- o `TPIDR_EL0`, o `sigsetjmp` e a recuperação de faults têm de ser
  **por thread** no nosso motor (o Eden não precisa: o estado do guest é o
  estado do emulador, e o emulador tem um scheduler);
- um IPC feito por uma thread de guest usaria o **buffer de mensagem da
  thread principal** — porque o Eden lê o buffer do `KThread` corrente, e
  nós só temos um.

O que fica para o próximo passo, com o motor em estado bom (62,3 ms,
218 SVCs, 0 SVCs com erro, 0 respostas IPC com erro):

1. pôr o `TPIDR_EL0`, o `sigsetjmp` e a recuperação **por thread** (o
   protótipo está feito, e revertido porque o fault matava o processo);
2. dar à thread o `sp` certo e **ver onde ela dá fault** — que é a
   informação que falta.

---

# Sessão 2026-10-20 (quarta parte) — o passo 1: o estado por thread

## O que o passo 1 é

Uma thread de guest tem de ter, **por thread**:

| | porquê |
|---|---|
| o `TPIDR_EL0` | o guest lê o TLS com `mrs x, tpidrro_el0` (o patcher reescreve para `tpidr_el0`) |
| o ponto de regresso (`sigsetjmp`) | um `siglongjmp` para o `sigjmp_buf` de outra thread é comportamento indefinido |
| a recuperação de faults | um fault na thread nova não pode saltar para a recuperação da principal |
| o `TPIDR_EL0` do **host** | o handler troca o TLS do host pelo do guest *durante* a trap, e a `libc` de cada thread tem o seu |

O Eden resolve isto com objectos de kernel (`KThread`, com TLS e contexto
próprios) e um *scheduler*. Nós somos **nativos**: o código do guest corre
directamente na CPU, logo os registos já são por thread — o que falta é o
nosso livro de contas.

## O que foi medido (e porque o passo 1 foi revertido)

| Sintoma | Causa |
|---|---|
| `exit 70` ("sem ponto de regresso") | o `sigsetjmp` estava numa **frame que já tinha regressado**; o `siglongjmp` para um buffer morto é indefinido |
| o guest lia o TLS do **host** | o `TPIDR_EL0` era global e ficava a 0 — o guest escrevia por cima da `libc` (o processo morria de **OOM**) |
| o `SIGSEGV` da thread nova não era reportado | o `TPIDR_EL0` do guest só era instalado **durante** as traps; fora de uma trap o registo é o da `libc` do host, e o guest lia-o a toda a hora |

Ou seja: o passo 1 tem **três** partes, e as três são necessárias — não
basta trocar o `TPIDR_EL0` por thread.

**O passo 1 fica escrito e revertido**, porque com ele o processo morre de
OOM e o guest fica sem TLS. O motor está no estado bom (61,9 ms, 218 SVCs,
0 SVCs com erro, 6/6 testes).

## O que fica feito para o passo 2

O que falta, por ordem:

1. instalar o `TPIDR_EL0` do guest **antes** de o guest correr (e não só
   durante as traps) — é a causa do OOM;
2. o `sigsetjmp` numa frame que sobreviva (a rotina `g610_run_guest`, em
   ensamblador, porque o `sp` do guest tem de ser o `sp` da CPU);
3. o `TPIDR_EL0` do host por thread;
4. **medir onde a thread dá fault** — que é a informação que falta, e que o
   dump do `entry point` (no `boot_trace`) já permite comparar.

---

# Sessão 2026-10-20 (quinta parte) — o passo 1, em detalhe, e uma referência

## As quatro coisas por thread (e porque sao quatro)

Uma thread de guest precisa de:

| | o que acontece se nao for por thread |
|---|---|
| o `TPIDR_EL0` do **guest** | o guest le o TLS do *host* e escreve por cima da `libc` — **OOM**, medido |
| o `TPIDR_EL0` do **host** | a `libc` de cada thread tem o seu; o handler troca o TLS do host pelo do guest *durante* a trap |
| o ponto de regresso (`sigsetjmp`) | um `siglongjmp` para o buffer de outra thread é comportamento indefinido — `exit 70`, medido |
| os **registos** | o Eden deixa-os a zero (`KThread::InitializeContext`): sem isso, o entry point lê `x7`/`x19` do host e salta para um endereço do host — medido |

E o `sigsetjmp` tem de estar numa **frame que sobreviva** ao código do
guest: um `siglongjmp` para o `sigjmp_buf` de uma frame já regressada é
comportamento indefinido.

**O que está escrito** (e não está publicado, porque não passou nos
testes): o motor tem o estado por thread, a rotina `g610_run_guest` (em
ensamblador, porque o `sp` do guest tem de ser o `sp` da CPU) e o
`CreateThread` arranca a thread. **O que não está resolvido:** com isso, o
processo morre de OOM **mesmo sem a thread a correr** — e o crash acontece
no **primeiro `SendSyncRequest`** (`sm: Initialize`), que não tem nada a
ver com threads. Ou seja: há uma regressão no caminho principal que ainda
não está isolada.

**A lição:** a mudança do motor que "não muda nada" (o estado por thread)
mudou. A próxima tentativa tem de ser **por partes**: primeiro o motor com
o estado por thread e **sem** o arranque da thread (e ver se o
`sm: Initialize` continua a funcionar), e só depois o arranque.

## A referência que faltava: o Eden está a correr o mesmo jogo

No Pi há um **Eden completo a correr este mesmo jogo**:

    /home/emuos/eden/build/bin/eden -f \
        /home/emuos/user/sdmc/atmosphere/contents/01008DB008C2C000

Isto é a **fonte da verdade do projecto** a correr ao lado: o mesmo jogo, o
mesmo ExeFS, e o emulador de referência. Serve para:

- confirmar o que o Eden faz nos mesmos pontos onde o nosso motor diverge
  (o `CreateThread`, a região da stack, o `QueryMemory`);
- ter um trace de comparação das respostas IPC.

É o melhor sítio para passam a próxima sessão: não para adivinhar o
comportamento do SDK, mas para o **ver**.

## O estado do motor

| | estado |
|---|---|
| arranque | 61,9 ms / 218 SVCs |
| SVCs com erro | 0 |
| respostas IPC com erro | 0 |
| testes | 6/6 |
| commits | `df33889` (o `PROGRESSO.md` da sessão) |

O passo 1 fica escrito e **medido**, mas não publicado: só entra no
histórico quando passar nos testes.

---

# Sessão 2026-10-20 (sexta parte) — o OOM era o `thread_local` no handler

## A pista: o crash era no primeiro `SendSyncRequest`

O WIP do passo 1 (o estado por thread) fazia o processo morrer de OOM no
**primeiro** `SendSyncRequest` — o `sm: Initialize`, que não tem nada a ver
com threads. Logo, não era a thread: era o **estado por thread em si**.

## A causa: `thread_local` (e `__thread`) lidos **dentro do handler de sinais**

O `t_thread_de_guest` era lido no handler, para saber se a trap vinha de
uma thread de guest. E:

| | o que acontece |
|---|---|
| `thread_local` | em modo PIC o acesso passa por `__tls_get_addr`, que **pode alocar** — e alocar dentro de um handler de sinais é um caminho para o OOM |
| `__thread` | num executável **PIE** o acesso é local-exec, com o offset resolvido no *link* — e o binário é PIE, logo o offset está errado e o valor lido é lixo |

Em ambos os casos o mesmo sintoma: o motor pensa que está numa thread de
guest, o handler põe o TLS da `libc` a zero, e o processo morre.

**A correcção: variáveis simples, globais.** O Eden também tem um "current
thread" global (`GetCurrentThreadPointer`), e nós temos uma thread de guest;
a corrida entre a thread principal e a de guest é limitada ao flag e aos
dois endereços de TLS.

**Medido:** com as três mudanças por thread (o `emulate_tls_read`, o
`exit_recovery` e o trampoline) e as variáveis globais, o motor fica
**estável** (61,8 ms, 218 SVCs) — e, com a thread a correr, o guest **não
morre de OOM**.

## E a thread corre

Com o `CreateThread` a arrancar a thread (uma thread do host a correr o
entry point, com o `sp` que o guest deu e o `TPIDR_EL0` no TLS dela):

    [thrd] a thread de guest faultou: pc=0x0 addr=0x0 sp=0x0

O `pc=0` e o `addr=0` com todos os registos a zero dizem que **não houve
SIGSEGV**: o que houve foi um `brk #0` — a rotina em ensamblador, depois
de a função do guest **voltar**. Ou seja: **a thread do guest correu e
terminou** ✓ (o que resolve a dúvida sobre o *entry point* ser uma
continuação: não é ✗ — ou é, mas o caminho é o certo ✗).

O que falta agora é a **medida fina**: os registos no momento do `brk #0`
(dizem o que o guest esperava ✗) e o porque de o guest morrer logo a
seguir (o `Break` #191 ✗ — o mesmo abort de sempre ✗, mas com 189 SVCs em
vez de 218 ✗, porque a thread morreu mais cedo ✗).

## Estado

| | |
|---|---|
| `main` | **estável**: 62,0 ms, 218 SVCs, 0 SVCs com erro, 6/6 testes |
| `wip/passo-1-thread` | o passo 1 + a thread a correr; falta a medida fina |

# 2026-10-21 — wip/passo-2-thread: a thread de guest deixou de roubar o TLS da principal

## O bug do passo 1, agora explicado com medida

No passo 1, as três variáveis `__thread` (`g_thread_de_guest`,
`g_guest_tls`, `g_host_tls`) eram escritas e lidas *só no contexto da
thread certa* (`run_guest_thread`). O que não tinha resposta era o
**handler**: que thread levantou a trap? O trampolim respondia
`g_hooks.guest_tls`/`g_hooks.host_tls` — os valores da **principal**,
publicados no `run()`.

Medição do `wip/passo-1-thread`(log `/tmp/boot_wip.log`, Pi): a trap de
uma thread de guest instalava o TLS da principal; o `str x0,[x1,#0x1f8]`
do SDK (que grava o ponteiro de TLS no próprio bloco, medido ate ao
sdk+0x32F840) escreveu o `tls_tp` **da principal**; a thread de guest
ficava com o estado partilhado e o relatório de fault lia o estado
global — "pc=0x…, addr=0x0" (lixo de outra thread) e **zero** `[thrd]
trap` no log.

## O desenho do passo 2 (tabela, não `__thread`)

`mrs x, tpidr_el0` no handler de entrada; o **valor do registo
identifica a thread** (o kernel preserva o `TPIDR_EL0` no caminho do
sinal — medido no passo 0, quatro vias, PROGRESSO.md). Tabela fixa de
32 slots (`g_thread_slots`), registo com `compare_exchange` — sem lock,
sem alocação dentro do handler. Cada slot é um `GuestThreadState`: os
TLS da thread (guest e libc), contadores, fault (pc/addr/sp reais), o
`sigjmp_buf` da frame **dela** e uma stack de recuperação **dela** (um
`siglongjmp` para o `jmp_buf` de outra thread é UB, e o `sp` de
recuperação segue a mesma regra).

O `run()` regista a principal no `g_hooks` **como sempre** (os testes
e a v0.1.0 não mudam); as threads criadas via `CreateThread` contam no
SEU slot, e o relatório do `run()` soma os dois: principal + slots.

O `x18` é **por thread**: o Eden gera `Random64(0) | 1` em cada
`ResetThreadContext64` (`k_thread.cpp:58`). O handler repõe o `x18`
desta thread depois de cada SVC (do `GuestThreadState.platform_reg`,
não do `g_hooks.platform_reg` da principal).

## Medido no Pi (`/tmp/boot_p2.log`, exit=134)

| métrica | passo 1 | passo 2 |
|---|---|---|
| `[thrd] trap` no log | **0** | **195** |
| `addr do fault` | `0x0` (lixo global) | pc/sp/regs reais por trap |
| SVCs despachados no total | 189 (a thread morria cedo) | **522+** (as 3 threads seguem depois do fim da principal) |
| abort da thread principal | idem | **idem da v0.1.0**: 61,0 ms/205-218 SVCs, `_Unwind_RaiseException()`, brk #0x26 em `pc=0x83b0bba4` |
| fim do processo | SIGSEGV do próprio motor | novo: `double free or corruption (out)` (glibc) |

As 195 traps de guest têm PCs reais do `sdk`
(`0x83b0bb2c`, `…bb3c`, `…bb44`, `…bb64` — perto do brk de 0x83b0bba4
que a v0.1.0 tinha) com sp de stack de guest real
(`0x2b2a204c0`, dentro da região [0x2900_0000, +2 GiB)) e
`x19` com o contexto do guest — as traps são de **código do guest a
correr de verdade**, não do motor.

## O que a parede nova é

O processo morre depois do fim da execução da principal, com as 3
threads de guest ainda a despachar `Svc#26` (QueryMemory) — o registro
`[svc #513]…[#522]` alterna a stack da principal (`x4=0x30fff…`) com
stacks de threads (`x4=0x2b2a20…`) na **mesma fila de despacho**: o
`SvcDispatcher` é **global e partilhado** (`g_hooks.svc->dispatch`),
com estado interno não protegido — `handles_` (std::map),
`new KObject` em `svc_create_thread`, e o IPC (o Eden usa o buffer de
mensagem da thread principal, `Bridge::thread()`,
`k_server_session.cpp:379`). **Suspeita** (não é afirmação): a heap
corrompe-se numa destas corridas.

Fica registado: o `g610_boot` (host) morre por `double free or
corruption (out)` — **a parede nova não é uma SIGSEGV**, o `main`
continua estável na v0.1.0.

## Estado

| | |
|---|---|
| `main` | **estável** (tag v0.1.0, `8c27236`): 6/6 testes |
| `wip/passo-2-thread` | a thread de guest com estado e TLS próprios; **6/6 testes no Pi**; 195 traps reais de guest; a seguir: serializar o despacho (passo 3) |

# 2026-10-21 — wip/passo-3-thread: o despacho serializado e a medição por subtração

## A parede do passo 2, com medida adicional

`double free or corruption (!prev/(!out))` a morrer em posições
diferentes entre execuções do MESMO binário (SVC final 261/262; traps
de guest 195 → 31) — **corrida, não determinismo do jogo**. Suspeitos:
o despacho concorrente no `SvcDispatcher` partilhado.

## O que o passo 3 muda

Um **dono atómico por thread** à volta de `g_hooks.svc->dispatch()`
(`g_dispatch_owner`): o despacho de SVC corre UM de cada vez no
processo inteiro — o papel de um kernel a despachar syscalls uma a uma,
que é o modelo do Eden/NCE. Caso explícito: trap **aninhada** do próprio
dono (fault do host no C++ do despacho — handlers são `SA_NODEFER`)
não re-entra; trata como fault e sai pela recuperação. O release é
manual nos pontos de fim (`run()` e a frame da thread), porque o
`siglongjmp` da recuperação NÃO desmonta o frame do despacho:
destructor nenhum corre e um lock por RAII ficaria enleixado.

## A B por subtração (o observador do tracer desligado — `IA_NOOB=1`)

| configuração | saida | corrupção de heap | traps `[thrd]` |
|---|---|---|---|
| com observer (+IA_TRACE) | exit 134 | **há** (3/3) | 31-198, svc final 261/520 |
| com observer, 3× (despacho serializado) | exit 134 | **há** (3/3) | 53-198 |
| **sem observer (`IA_NOOB=1`), 3×** | exit 139 | **ZERO (3/3)** | **8004** |

Leitura honesta: o motor sem caminho de diagnóstico **não corrige a
heap** — a corrupção só aparece quando o observador/tracer (printf,
`std::string`, dumps de ficheiro a correr de dentro do handler em
várias threads) está ligado. Com ele desligado, o motor corre até à
colocação própria do limite: **limite de traps POR THREAD funciona**
("[thrd] a thread de guest parou ... (traps=4000 faults=0)" — a thread
para a SEU número), 8004 traps de guest em 3 execuções, e "[boot] fim"
é impressionado. Fica uma regressão nova: **exit 139 (SIGSEGV) depois
do "[boot] fim"** — alguma thread desligada ou destructores de saída
no fim do processo.

## Parede de verdade que emerge

As threads de guest entram num **loop de retries determinístico** ao nível
das traps `#26` (`[thrd] trap #26 ... sp=0x…`) até queimar o limite de
4000 traps: 8004 traps de loop em 2 threads.

**CORRECÇÃO ao ler as medidas de cima (passo 4, a 2026-10-21):** o
número decimal 26 da trap é hex `0x1A`, e `0x1A` NO SWITCH NÃO É
QueryMemory — é **ArbitrateLock** (a enum do próprio motor, e a do Eden
`svc.h:411`: `ArbitrateLock = 0x1A`). O caso 0x1A não existia no switch
do despachante → `svc_unsupported` (0x40) → o SDK relança o pedido em
loop. Era UM MUTEX CRÍTICO do `nn::os` em espera, não um scan de
memória. As implementações seguintes (passo 4) partem para a fonte Eden:
`svc_lock.cpp` e `k_condition_variable.cpp` (WaitForAddress / SignalToAddress).

## Estado

| | |
|---|---|
| `wip/passo-3-thread` | despacho serializado com dono por thread; 6/6 testes no Pi; sem observer a heap não corrige (3/3); as paredes deixaram de estar no motor |

# 2026-10-21 — wip/passo-4-thread: ArbitrateLock/ArbitrateUnlock, o mutex do nn::os

## Semântica medida na fonte (Eden no Pi)

- `ArbitrateLock(handle, addr, tag)` → `KConditionVariable::WaitForAddress`
  (`k_condition_variable.cpp:156`): lê o u32 em `addr`; **devolve Success LOGO** se
  não for `handle | HandleWaitMask` — `HandleWaitMask = 1<<30`
  (`svc_common.h:19`) = `0x40000000`. Só nesse estado entra na fila.
- `ArbitrateUnlock(addr)` → `SignalToAddress` (:116): remove o PRIMEIRO
  waiter; escreve na memória o tag do próximo dono (`|` máscara se houver
  mais waiters, `<0>` sem nenhum) — **write incondicional** (:142), fence
  `seq_cst` antes (:132) — e SÓ ENTÃO desperta o removido (`EndWait`).
- `R_UNLESS(Common::IsAligned(address, sizeof(u32)), ResultInvalidAddress)`
  (svc_lock.cpp): alinhamento 4 validado a ambas.

## O que o passo 4 muda no motor

1. **Dono do despacho exportável** (`native_engine.hpp`):
   `dispatch_owner_itself()` / `_vacate()` / `_claim()` / `_set()` —
   token `thread_local` post em curso no ponto do despacho. Um SVC
   bloqueante LARGA o dono enquanto espera (senão o unlocker nunca
   despacha → impasse) e volta a tomá-lo ao acordar, o mesmo que uma
   syscall kernel bloqueante faz.
2. **O quadro de arbitragem** (`svc_dispatcher.cpp`): fila por endereço
   com (tag, token); `gate` de `std::mutex` + `condvar` única. A
   transacção [leitura da palavra → registo → largar o dono] corre SOB o
   gate — fechada face ao unlocker — porque fora do gate o unlocker pode
   largar ENTRE a leitura e o registo e o notify perde-se (wake em rota).
   O wait liberta o gate atomicamente.
3. **Os casos 0x1A/0x1B no switch**, com nomes em `to_string(Svc)`, e a
   fronteira honesta: fora de despacho (token 0) o estacionamento devolve
   Invalid — não inventa Success.

## Testes de unidade (caminhos sem estacionamento)

`test_core.cpp` ganhou o bloco ArbitrateLock/Unlock: cabeça livre →
Success sem alterar a palavra; desalinhado → recusado; unlock sem
waiters → escreve 0; estacionamento fora de despacho → Invalid por
desenho. A transferência com fila (estacionar/entregar/acordar) valida
no boot real no Pi, que é o critério: as threads deixarem de queimar
traps `#26`.

## Medição no Pi (a prova do passo 4)

- **Testes**: 114 checks todos ok no `g610_tests`; `ctest` 6/6.
- **Boots `IA_NOOB=1` ×3** (`/tmp/boot_p4_r{1,2,3}.log`): **traps de guest
  11 / 11 / 12** — o ciclo de 8004 traps `#26` **morreu**. Determinístico
  entre execuções, exit igual (139) nos 3.
- **Confirmação por desmontagem**: `pc=0x83b0bb2c` é o stub do SDK,
  offset `0x37eb2c` do `text.bin` do módulo `sdk` (base `0x8378d000`) =
  **`svc #0x1a` real no binário** (o patcher reescreve para `brk #26`) —
  o guest chama `svcArbitrateLock(dono=0x8000, addr=0x843ea4d0,
  tag=0x10001/0x18002/0x81ff…)` — os tags são os handles das threads,
  como previsto.
- **A thread principal não progrediu com este passo** (honestidade):
  223 SVCs, mesma parede de IPC de antes — `SendSyncRequest: o handle
  66046 não é sessão` (58 calls). O passo 4 foi sobre a fila de mutex
  das threads de guest; a parede do IPC é o passo seguinte.
- **Correcção honesta**: o "limite de traps (4000)" do relatório é o
  force-stop do amostrador do tracer no fim da janela
  (`boot_trace.cpp:140`), não um tecto natural: o guest parou À MÃO no fim
  da amostragem. E a linha "última tr" cortada nos logs era o stdout
  em buffer perdido no SIGSEGV do exit — com `stdbuf -o0` o relatório sai
  inteiro (4/4 boots: exit 139 DEPOIS de `[boot] fim`, mesma regressão
  pré-existente do passo 3, agora confirmada em 4).

## Estado

| | |
|---|---|
| `wip/passo-4-thread` | arbiter à Eden medido no Pi: 6/6 testes, 11 traps de guest por boot (era 8004); a parede seguinte é o IPC da principal |

---

# Passo 6 — o arranque do jogo, serviço a serviço

O objectivo continua o mesmo: **o jogo na tela**. O caminho agora não é
o IPC (está destravado) mas a **cadeia de serviços** que o SDK pede no
arranque — cada um deles é uma porta, e uma porta que não existe é uma
morte.

## A regra que o passo 6 revelou

**Um serviço que falta mata o jogo, e mata de uma maneira específica.**

O `sm:` **defere** o pedido de um serviço que não está registado
(`sm.cpp:207-209`: `ctx.SetIsDeferred()`). No Eden isso é seguro: o
guest **emulado** fica bloqueado no kernel à espera da resposta, e o
`ServerManager` tenta o pedido de novo quando o serviço aparece.

No nosso motor o guest é **nativo**: o `SendSyncRequest` é despachado em
linha, dentro do SVC, e o resultado volta logo. E o deferimento
responde com **0 palavras** — o `GetServiceCmif` do Eden devolve sem
tocar no buffer quando o pedido foi diferido. O reader CMIF do guest
recebe zero palavras onde espera a Result + handle, e aborta:

```
[eden:W] Service::AM: SetTerminateResult: modulo=162 desc=1 (0x000002A2)
```

O módulo 162 é o `UserlandAssert` da libnn.

**A medição que decide tudo** (`boot_p6f.log`): o guest pede **dez**
serviços no arranque — `time:u`, `pctl`, `pctl:a`, `pctl:r`, `pctl:s`,
`lm`, `appletOE`, `apm`, `aoc:u` e `fsp-srv` — e **o `fsp-srv` era o
único em falta**. Os outros nove tinham resposta.

## A lista dos serviços, e o que cada um custou

| serviço | medido | o que faltava |
|---|---|---|
| `time:u` | `p6c`–`p6f` | não existia: o SDK pedia-o e o `sm:` deferia |
| `fsp-srv` | `p6f` | não existia (o único em falta) |
| `nvdrv` | `p6m`–`p7b` | não existia; e os ioctls que o guest pede |
| `appletOE`, `pctl*`, `lm`, `apm`, `aoc:u` | já estavam | — |

## Os SVCs que faltavam (e matavam o jogo pelo mesmo mecanismo)

Um SVC que devolve `Unsupported` (0x40) mata o jogo: o SDK trata um
Result inesperado como fatal e chama `SetTerminateResult(0x40)`.

| SVC | medido | o que faltava |
|---|---|---|
| `CreateTransferMemory` (0x15) | `p6k` | não existia |
| `SetMemoryAttribute` (0x03) | `p6l` | não existia |
| `SetMemoryAttribute` — o endereço | `p6r` | a validação da região (ver abaixo) |

## Os `Result` inventados (uma armadilha que já tinha matado o jogo)

A tabela de `Result` da casa tinha valores **inventados** (`Invalid = 1`,
`NotInitialized = 2`, …). O guest conhece os Results do Eden, e um Result
que ele não conhece é fatal.

**Medido** (`boot_p6q.log`): o `SetMemoryAttribute` devolveu o `Invalid`
(0x1) — um erro verdadeiro (o endereço 0 não está no espaço do processo) —
e o SDK foi logo ao `SetTerminateResult(0x1)` e ao `Break`. O Eden
devolveria `ResultInvalidCurrentMemory` (54273), que o SDK sabe ignorar.

Agora os valores são os do Eden (`core/hle/kernel/svc_results.h`:
`descrição << 9 | 1`) e **cada call-site devolve o Result que o Eden
devolve** nessa SVC.

## O `notify` que faltava (o deadlock)

O Eden, ao **esperar** por uma condition variable, larga o mutex e
**acorda o próximo dono** (`KConditionVariable::Wait`,
`k_condition_variable.cpp:285-292`: `next_owner_thread->EndWait(...)`).

O nosso código marcava o token como entregue e **não notificava** — o
próximo dono dormia para sempre.

**Medido** (`boot_p6o.log`): o `SignalProcessWideKey` encontrava o waiter
(`count=1`) e não acordava ninguém (`acordados=0`); todas as threads do
guest acabavam paradas à espera do mesmo mutex, e o jogo não avançava.

## O `KernelCore` que não estava ligado a tempo

O `ServiceManager` constrói o `IpcController` no **seu** construtor
(`sm.cpp:31`), e o `Controller` guarda uma **referência** ao `KernelCore`.
Se o `ServiceManager` for construído antes do `ligar()`, essa referência
fica ligada a nada.

O primeiro uso é o `CloneCurrentObject` (o SDK converte a sessão do
`fsp-srv` em domínio), e aí o motor escreve no mutex de um
`KernelCore` nulo e morre. **Medido** (`boot_p6i.log`): o backtrace era
`CloneCurrentObject → KSession::KSession → KernelCore::
RegisterKernelObject`, com o fault em `pthread_mutex_lock`.

O instrumento que o achou: o tratador de `SIGSEGV` diz agora o **símbolo**
de um fault do motor (`dladdr` + `backtrace_symbols_fd`).

## Os dois `Settings::Values` (um conflito de tipos)

A nossa ponte e o `settings.h` do Eden definem tipos diferentes, e os
dois declaram `Settings::values` — o **mesmo símbolo**. O `aoc`
vendorizado lia `Settings::values.disabled_addons` num offset que o nosso
tipo não tem: lia lixo, e o `std::find` sobre um vector lixo matava o
motor. **Medido** (`boot_p6j.log`).

O `settings.h` vendorizado passa a **ser** a ponte (override novo), e a
ponte ganhou o terceiro campo que o código vendorizado lê.

## O caminho da GPU

O `nvdrv` é **fino de propósito**: a interface é a do Eden (os IDs dos
14 comandos, os nomes, as formas das respostas), o driver não vem. O que
o guest pede, medido:

| ioctl | grupo/comando | o que é |
|---|---|---|
| `GetCharacteristics1` | `'G'`/0x05 | as características da GPU (176 bytes) |
| `GetTPCMasks1` | `'G'`/0x06 | as máscaras de TPC (24 bytes) |
| `ZCullGetCtxSize` | `'G'`/0x01 | o tamanho do contexto de ZCull (4 bytes) |
| `ZCullGetInfo` | `'G'`/0x02 | os alinhamentos de píxeis (40 bytes) |
| `AllocAsEx` | `'A'`/0x09 | o espaço de endereços virtual da GPU |
| `GetVARegions1` | `'A'`/0x08 | as regiões do VA (64 bytes) |
| `AllocateSpace` | `'A'`/0x02 | alocar no VA |
| `MapBufferEx` | `'A'`/0x06 | mapear a memória de um handle no VA |
| `BindChannel` | `'A'`/0x01 | ligar o canal do `nvhost-gpu` |
| `IocCreate`/`IocAlloc`/`IocParam`/`IocFree`/… | 0x01 | os handles de memória (`nvmap`) |

**O VA da GPU é o espaço do guest** — num motor nativo não há tradução,
e o push buffer que o guest submete é lido pelo tradutor Maxwell → Vulkan
(`src/gpu/`) de lá.

**Medido**: o guest pede **65536 páginas de 64 KiB = 4 GiB** de páginas
grandes. Com 512 MiB (a região de alias) a alocação falhava, e o SDK
com o VA sem espaço desreferenciava um ponteiro nulo (`0x10`).

**Um ioctl que não existe dá `NotImplemented`, nunca "sucesso com
zeros"**: com sucesso e um buffer a zeros, o guest usou os zeros como
dados válidos e saltou para um ponteiro lixo (`0x40011e2d407c3`).

## Onde estamos

O guest **não aborta** e **não dá erro de SVC**. Chega ao fim da
inicialização do driver da GPU e do **display**, e pára num fault de
memória **dentro do SDK** (`0x83b98c00` → depois `0x837982bc`, um
endereço no vão entre módulos — um ponteiro de função lixo).

### O canal, e a lição do despacho

O `nvdrv` fino despachava os ioctls **só pelo grupo**. Isso não
basta: **o device importa**. O grupo 0x00 tem comandos nos dois
devices — no `nvhost-ctrl` é o `IocCtrlEventUnregisterBatch` (0x21) e no
`nvhost_gpu` é o `GetWaitbase` (0x03).

Agora o `Open` guarda o **tipo** do device (pelo nome, como o Eden) e o
despacho é por **(device, grupo, comando)**, com as tabelas de cada
device.

**Medido** (`boot_p7g.log`): de 31 para **45 ioctls**, e o
`SetTerminateResult` deixou de aparecer.

### A submissão — a porta de entrada do tradutor

O `SubmitGPFIFOBase1` (grupo `'H'`, comandos 0x08 e 0x1b) é onde o
trabalho do jogo chega à GPU: o guest escreveu os comandos Maxwell num
endereço do VA da GPU e submete a lista de cabeçalhos.

**As duas variantes** (o Eden, `nvhost_gpu.cpp`): no 0x08 os cabeçalhos
vêm **no pedido**; no 0x1b estão **no endereço**. Os comandos de cada
entrada são lidos de lá, e é o **push buffer** que vai para o tradutor
(`MaxwellTranslator::translate`, `src/gpu/`).

**O VA da GPU é o espaço do guest** — num motor nativo não há tradução,
e o push buffer é lido de lá. Por isso o tradutor e o `nvdrv` partilham
a mesma memória, e o `MapBufferEx` regista o par (VA → endereço do
guest) para a tradução.

### O `vi` (o display) e a lição do `AlwaysMoveHandles`

O guest pediu `GetService("vi:m")` — o serviço de vídeo. São três
serviços ("vi:m", "vi:s", "vi:u"), cada um com o seu display.

Duas lições que a medição deu:

1. **O "vi:m" devolve o display da APLICACÃO**
   (`Out<SharedPointer<IApplicationDisplayService>>`, no upstream). Devolver
   o display do *manager* dava ao guest outra tabela: ele pedia o 100
   (`GetRelayService`), que no display do *manager* não existe.
2. **Um comando que devolve uma interface tem de devolver uma
   interface.** O `GetRelayService` respondido pelo *auto-stub* (sucesso
   e zeros) deu ao guest um handle nulo, e ele desreferenciou-o: o
   salto para `0x837982bc`. O `AlwaysMoveHandles` (como o `sm:` faz no
   `GetService`) é o que garante o handle — **a sessão pode ser um
   domínio**, e nesse caso o objecto ia parar à lista de domínio sem
   nunca ser escrito.

### O que o jogo entrega: o protocolo do *binder* do Android

Com a native window, o guest passou a falar com o **binder** (o
`IHOSBinderDriver`): `AdjustRefcount`, `GetNativeHandle` e
`TransactParcelAuto` — e o *parcel* que ele manda foi medido
(`boot_p7m.log`):

```
5c 00 00 00   data_size = 92
10 00 00 00   data_offset = 16
00 00 00 00   objects_size = 0
6c 00 00 00   objects_offset = 108
00 01 00 00   0x100
22 00 00 00   34 (o comprimento do token, em caracteres)
61 00 6e 00 64 00 72 00 6f 00 69 00 64 00 2e 00 67 00 75 00 69 00 2e 00
             "android.gui." em UTF-16
```

Ou seja: o jogo fala o **protocolo do *binder* do Android**, com o token
de interface `android.gui.IGraphicBufferProducer` (34 caracteres), e
manda as transacções **9, 10 e 14**.

O Eden implementa isso com a pilha completa do *surfaceflinger* do
Android (`nvnflinger/buffer_queue_producer.cpp`, 36 KB, mais o
`surface_flinger.cpp` e o `hardware_composer.cpp`).

**O que a casa precisa**: o *binder* da casa é o
compositor — tem de responder a estas transacções (dar um buffer ao
jogo, receber o buffer desenhado) e apresentá-lo. É a **fila de
buffers** do Android, em versão mínima.

### O push buffer **real** — medido

O guest submeteu (`SubmitGPFIFO: address=0xE2D62290 entradas=3
flags=0x104 kickoff=1`) e os cabeçalhos, lidos crus, são:

```
00 20 11 b9  00 76 22 80  74 42 11 b9
00 06 00 80  78 42 11 b9  00 2a 00 80
00 00 00 00  00 00 00 00
```

Isto é `0xB9112000`, `0x80227600`, `0xB9114274`, `0x80000600`,
`0xB9114278`, `0x80002A00` — **três palavras, todas endereços** (dois
dados do guest, um de código).

**O layout NÃO é o `{tipo, endereço, tamanho}` que se supunha** (o
`Tegra::CommandListHeader` do Eden). Ler com essa forma dá
`tipo=3104907264` e `tamanho=3104916084` — o que é o sintoma de uma
leitura dessincronizada, e de um descodificador que precisa do *dump*
real para ser corrigido. É exactamente o que as notas do
`maxwell_translator.hpp` pedem: *"é preciso validar a tabela contra um
dump capturado"*.

**O dump existe agora** (acima). Falta descodificá-lo contra a
referência do *host1x*/*Tegra*.

### A descodificação — o que a medição **desmente**

A leitura em cascata (cada palavra dos cabeçalhos que pareça um
endereço é seguida, e o seu conteúdo é despejado) diz o seguinte
(`boot_p7s.log`):

| endereço | conteúdo |
|---|---|
| `0xB9112000` | 32 bytes a **zeros** |
| `0x80227600` | `0a 01 00 34 60 06 40 f9 08 00 40 f9 …` — **código AArch64** |
| `0xB9114274` | `fd 7b 41 a9 f3 07 42 f8 c0 03 5f d6 …` — **código** |
| `0x80000600` | `f4 4f 45 a9 f6 57 44 a9 ff c3 01 91 …` — **código** |
| `0xB9114278` | `fd 7b 01 a9 fd 43 00 91 b4 24 01 90` — **código** |
| `0x80002A00` | (a seguir) — **código** |

**Isto desmente a leitura `{tipo, endereço, tamanho}`**: os campos não
são um tipo e um tamanho — são **ponteiros para funções do próprio
jogo** (`stp x29, x30, [sp, #-16]!` é o prólogo de uma função).

E desmente também a leitura "o push buffer do jogo": a primeira
entrada é uma região a **zeros** (uma lista de comandos vazia) e as
restas são código. Ou seja: **o que o guest submeteu não é o push
buffer da GPU** — é uma lista de *callbacks* (é o que o
`nvhost-ctrl-gpu` recebe do `nvhost` do driver quando não há trabalho
de GPU a fazer).

**Onde está então o push buffer?** No **`nvmap`**: o guest aloca o
buffer (`IocCreate`/`IocAlloc`), dá-lhe o handle ao canal
(`SetNVMAPfd: handle=3` — medido) e submete com
`SubmitGPFIFO(address=…)`. O handle 3 está no **registo partilhado**
da casa, com o seu endereço — que é o que falta seguir.

**O próximo passo concreto**: seguir o handle do `nvmap` que o canal
recebe (`SetNVMAPfd`) e **despejar o conteúdo dessa região** — é aí
que está o push buffer real, e é com esse dump que a tabela de opcodes
se valida.

### O formato do push buffer — a correcção (fonte: upstream)

A leitura anterior (`{tipo, endereço, tamanho}`, 12 bytes) **era falsa**.
O upstream do Eden define o cabeçalho em
`src/video_core/dma_pusher.h`:

```cpp
struct CommandListHeader {
    union {
        u64 raw;
        BitField<0, 40, GPUVAddr> addr;      // o endereço (bytes)
        BitField<40, 1, u64> allow_flush;
        BitField<41, 1, u64> is_push_buffer;
        BitField<42, 21, u64> size;           // o tamanho (bytes)
        BitField<63, 1, u64> sync;
    };
};
```

Ou seja: **8 bytes por entrada, com os campos em bits**. E o Eden usa o
endereço **directly**, sem escala (`dma_state.dma_get =
command_list_header.addr`).

A casa tratava-o como três `u32` — e a medição do dump provou que a
leitura era falsa: os "endereços" que se viam eram as metades de um
`u64`. **Corrigido** (`src/services/nvdrv.cpp`): o cabeçalho é agora o
`u64` do upstream, com os mesmos campos.

**Como a submissão do guest de teste se descodifica** (medido, depois
da correcção):

| entrada | addr | tamanho | push_buffer | sync |
|---|---|---|---|---|
| 0 | `0xB9112000` | 2205 | 1 | 1 |
| 1 | `0xB9114274` | 1 | 1 | 1 |
| 2 | `0xB9114278` | 10 | 1 | 1 |

### O guest de teste **não desenha** — medição definitiva

Três medições independentes, todas do mesmo arranque:

| região | tamanho | conteúdo |
|---|---|---|
| o push buffer (`0xB9112000`, 2205 bytes) | 2205 | **zeros** |
| a região do canal (`nvmap` handle 3) | 313 MB | **zeros** (varredura completa) |
| a native window (`0x9C800000`, 1280×720) | 3,6 MB | **zeros** |

O guest submete uma lista de **três** push buffers (o formato é
correcto!) mas **escreve-os a zeros**. Não há trabalho de GPU para
executar — e portanto **o caminho da GPU não pode ser validado com o
guest de teste**: o tradutor (item 3) e o backend (item 4) não têm
entrada.

### O jogo real — o que há e o que faltava

- O NSP **está no Pi**: `~/piDroid/roms/Pokemon Shield
  [01008DB008C2C000][v0] (9.53 GB).nsp` (9,53 GB, PFS0).
- O `exefs_pokemon/` do workspace local **é uma cópia do guest de
  teste** (md5 do `main` **igual** ao do `piDroid_Test_ARM`) — nunca
  foi o jogo.
- O `main` **real** existe localmente em `main_uncompressed` — e é
  **NSO0** ✓, que é o formato que o carregador da casa sabe ler (com
  descompressão LZ4). O `main_elf` é a mesma coisa em ELF.
- O `hactool` **não existia** (era o que o `pokemon_convert.log`
  complainha). **Compilado no Pi** (Makefile, mbedtls) ✓.
- A estrutura do NSP (medida): 6 partições PFS0 — o *program* NCA tem
  9,5 GB (é standard crypto), e o **ExeFS está dentro dele**; os
  restantes NCAs são 964 KB, 251 KB e 114 KB, e há o `.cnmt`.

O caminho passa a ser: `hactool --exefsdir` sobre o *program* NCA →
`main`/`rtld`/`sdk`/`subsdk*`/`main.npdm` reais → o motor carrega-os
(o carregador de NSO já existe e está testado) → e aí o guest **real**
faz o trabalho de GPU, com o push buffer *host1x* a sério.

### **O "app de teste" É O JOGO** (medido)

O `exefs` que o motor arrancava (`piDroid_Test_ARM/work/exefs`) **é o
`main` do Pokémon Shield**:

```
md5  exefs/main          = 9df91fc511ef73d7ae20c7065f431852
md5  piDroid/pokemon/exefs/main
                        = 9df91fc511ef73d7ae20c7065f431852   (igual)
strings: "pokemon/", "qTownmap", "CAPTURE_POKEMON", "pokemon-gl.com"
```

O `exefs_pokemon/` do workspace local era **uma cópia** desse mesmo
ficheiro — nunca o jogo. E o `main_uncompressed` local é o `main` real
descomprimido (38,5 MB = o NSO0 de 20,4 MB descomprimido) ✓.

**Estávamos a correr o jogo desde o início.** O que medimos antes
(submissão, janela, canal) são medições *do jogo*.

### O jogo real, extraído do NSP (no Pi)

- `hactool` **compilado no Pi** (Makefile + mbedtls) ✓.
- O NSP é um PFS0 com **6 partições**: o *program* NCA tem 9,5 GB
  (standard crypto) e o **ExeFS está dentro dele**; há ainda NCAs de
  964 KB, 251 KB e 114 KB e o `.cnmt`.
- O NCA tem de ser extraído **a partir do offset 400** (o cabeçalho
  PFS0 tem 16 bytes + 384 de tabela de strings) — foi essa a razão de
  "Invalid NCA header": a cópia começava desalinhada.
- `hactool --exefsdir` → o ExeFS: `main`, `main.npdm`, `rtld`, `sdk`,
  `subsdk0`, `subsdk1` ✓ (e é o mesmo exefs que já estava a ser
  arrancado).

### Onde o jogo para — a sequência medida

1. Display: `OpenLayer` → native window em `0x9C800000` (1280×720,
   3,6 MB) ✓.
2. Compositor: transacções **10, 9, 14, 14** (`IGraphicBufferProducer`)
   ✓ — o jogo fala connosco.
3. GPU: `MapBufferEx: handle=11 -> 0xB917C000` ✓ (o `MapBufferEx` está
   de acordo com o upstream: `size = mapping_size ? mapping_size :
   handle->orig_size` — o `tamanho=0x0` que aparece no log é o eco do
   *input*, não um erro).
4. **Bloqueio**: o jogo fica à espera de um *futex*
   (`WaitProcessWideKeyAtomic`) — o seu *thread* de GPU está à espera
   da **fence**, que **nunca é sinalizada** pela casa.

É este o bloqueio, e é o passo seguinte: o canal do `nvdrv` tem de ter
um **syncpoint** (um `KEvent`) e de o **sinalizar** quando a submissão
acaba. O upstream dá o modelo exacto (`SubmitGPFIFOImpl`):

```cpp
params.fence.id = channel_syncpoint;
u32 increment{(flags.fence_increment ? 2 : 0) +
              (flags.increment_value ? params.fence.value : 0)};
params.fence.value = syncpoint_manager.IncrementSyncpointMaxExt(channel_syncpoint, increment);
```

A casa já tem o `svc_wait_synchronization` e o `KEvent` (usados no
`time:u` e no `applet:oe`) — falta é ligá-los ao canal do `nvdrv`.

**O mecanismo, com o upstream como referência** (é o que faz o guest
desbloquear):

1. O jogo cria um handle de **semáforo** no `nvmap` (`IocCreate` com as
   bandeiras de semáforo) e dá-o ao canal.
2. O `nvmap` devolve ao guest o **handle de `KEvent`** desse
   semáforo (é o `IocMap`/`IocParam` do semáforo que o dá) — e é
   por esse handle que o *thread* de GPU do jogo espera
   (`svcWaitSynchronization`).
3. O `nvdrv`, no fim da submissão, incrementa o syncpoint do canal e
   **sinaliza** o `KEvent` do semáforo.

Ou seja: o que falta não é o tradutor — é o **sinal** que o trabalho
terminou. Sem ele, o jogo bloqueia antes de desenhar o primeiro
quadro, e por isso o push buffer ainda está a zeros.

### Onde o jogo bloqueia — a sequência do driver, medida

Os ioctls que o jogo faz, por ordem (log `boot_p9a.log`):

| # | ioctl | o que é |
|---|---|---|
| 1 | `0x47/0x05` (176 B) | características da GPU |
| 2 | `0x47/0x06` | máscaras de TPC |
| 3 | `0x47/0x01`, `0x47/0x02` | contexto, ZCull |
| 4 | **`QueryEvent`** (comando 4 do `nvdrv`) | o handle do evento — **devolvido** ✓ |
| 5 | `0x41/0x09`, `0x41/0x08`, `0x41/0x02` | `AllocateSpace` (4 GiB) |
| 6 | `nvmap`: `IocCreate`/`IocAlloc` ×5 | handles 1..5 (4 KB, 64 KB, **313 MB**, 216 MB, 4 KB) |
| 7 | `0x41/0x06` ×N | `MapBufferEx` de cada handle |
| 8 | `0x48/0x01` | `SetNVMAPfd` (handle 3) |
| 9 | `0x48/0x1A` | `AllocGPFIFOEx2` |
| 10 | `SubmitGPFIFO` ×3 | a submissão (3 entradas, todas `is_push_buffer=1`) |

E depois: `SetMemoryAttribute` (a marcar os buffers como memória de
dispositivo) e **o bloqueio** — o *thread* principal à espera de um
*futex* (`WaitProcessWideKeyAtomic addr=0x10a2c7d58`), que ninguém
desperta.

**O que já se排除iu** (por medição, não por suposição):

- **não** é o `MapBufferEx` (a casa devolve o VA certo, e o upstream
  usa `size = mapping_size ? mapping_size : handle->orig_size`);
- **não** é o `QueryEvent` (devolve o handle: `0x8000000c` ✓);
- **não** é o evento de **vsync** — o jogo **nunca** o pede (medido: o
  `GetDisplayVsyncEvent` não é chamado). O vsync a 60 Hz ficou
  implementado na mesma (é comportamento correcto do display), mas
  não é a chave;
- **não** é um SVC por implementar: o `svc_wait_synchronization`
  existe, e o log mostra que **não** é chamado.

**A pista que fica**: o jogo esperou num *futex* de chave de processo
(`WaitProcessWideKeyAtomic`), e o seu *thread* de GPU parou depois de
sinalizar outros futexes. O caminho que falta é o **sinal de "a GPU
terminou"** — seja pela *fence* (o `nvhost-gpu` devolve `fence.id` e
`fence.value`, e o upstream incrementa o syncpoint do canal), seja
pelo **semáforo** que o jogo cria no `nvmap`.

**O próximo passo medido**: registrar *qual* o `event_id` que o jogo
pede no `QueryEvent`, e *dumpar a stack do guest* no
`WaitProcessWideKeyAtomic` (a casa já o faz no
`svc_wait_synchronization`: mostra o handle, o `sp` e o conteúdo da
stack). Com a stack do guest e o `main` do jogo (NSO0, descomprimível
pelo carregador da casa) dá para ver **qual** caminho do SDK está a
esperar — e aí está a resposta.

### O deadlock — medido ao milímetro

O log tem a sequência exacta (`boot_p9l.log`):

```
4689  [svc] WaitProcessWideKeyAtomic addr=0x10a2c7b08 cv=0x10a2c7b0c tag=0x28004 timeout=-1
4695  [svc] SignalProcessWideKey  key=0x10a2c7b0c count=1 acordados=0      ← o sinal CHEGA
4698  [svc] WaitProcessWideKeyAtomic addr=0x10a2c7be0 cv=0x10a2c7bec tag=0x28004 timeout=-1
```

**O sinal chega depois do `wait`** (não é um sinal perdido) — e mesmo
assim **não acorda ninguém** (`acordados=0`).

**Porquê?** A regra do Eden (`SignalImpl`,
`k_condition_variable.cpp:198-248`): o `signal` só acorda o *waiter* se
a palavra da **mutex** estiver a **zero**; se estiver ocupada, o
*waiter* passa para a fila da mutex e só acorda quando o dono
**destrava** (`ArbitrateUnlock`).

E é aqui que o motor falha: **o jogo nunca destrava** — no log não há
um único `ArbitrateUnlock` (`grep -c` = **0**). O *render thread*
(0x28004) fica com a mutex, espera na cv, o sinal encontra a mutex
ocupada, e o deadlock fecha-se: ninguém acorda o *waiter* e ninguém
destrava a mutex.

**A correcção, com o upstream como referência** (`k_condition_variable.cpp:271-330`,
`KConditionVariable::Wait`):

1. ao esperar, o *waiter* **retira-se da fila da mutex** e o próximo
   dono passa a ser o próximo da fila (`next_value = tag (+ máscara)`),
   e é **acordado** (`EndWait`);
2. a palavra da cv recebe a bandeira de "há quem espere";
3. a palavra da mutex recebe o `next_value`.

A casa faz o 1 e o 3 — **mas não o "acordar" de quem fica com a
mutex** quando a mutex estava ocupada por outro, e o
`ArbitrateUnlock` do jogo nunca chega ao quadro.

**O que fica instrumentado** (para fechar isto):
- o dump da stack do guest no `WaitProcessWideKeyAtomic` (o PC, a
  cadeia de chamadas e os bytes do código) — é o que_allowed dizer que
  caminho do SDK está bloqueado;
- o rasto da arbitragem (`[arb] unlock`/`[arb] signal`): a palavra da
  mutex, quantos *waiters* há na fila, e o ramo que o sinal tomou;
- o **sinal pendente**: um `signal` que chegue antes do `wait` fica
  registado e a espera seguinte consome-o (é o que uma cv ao nível faz,
  e o que o Eden garante por serializar os dois no *scheduler lock*);
- a espera infinita passou a ter **prazo de salvaguarda** (16 ms,
  `IA_WAIT_MS` desliga) — o guest re-avalia o seu predicado, como uma
  cv bem usada faz.

### O deadlock abriu — medido

Com o signal da cv a **acordar o waiter mesmo com a mutex ocupada**
(o Eden só o acorda quando o dono destrava — e o jogo **nunca**
destrava: `grep -c ArbitrateUnlock` = 0 no log inteiro), o jogo sai
do impasse:

```
4758  [svc] SignalProcessWideKey key=0x10a2c7b0c count=1 acordados=1   ← acorda!
4831  parou por: fault de memoria do guest em 0x83b0bb44 (endereco 0x0, sp=0x30fffe950)
```

O *waiter* é **acordado** ✓ — e o jogo passa a correr até um
**fault diferente**: um acesso a `0x0` dentro do wrapper de futex do
SDK (`0x83b0bb44`). Ou seja: o impasse está resolvido, e o que
segue é um problema novo (e é um bom problema: o jogo está a
executar código a sério).

**O que fica por decidir**: o Eden só acorda o *waiter* quando o
dono destrava, e essa é a regra certa — mas o jogo, no nosso motor,
nunca destrava. As duas leituras possíveis:

1. o SDK do guest, no caminho do destrave, não chama o SVC porque a
   **palavra da mutex não tem o bit "há quem espere"** (`0x40000000`,
   `svc_common.h:19`) — e o bit é posto em `UpdateLockAtomic`
   (`k_condition_variable.cpp:198-215`), que a casa não faz no
   caminho do *lock* rápido;
2. o jogo destrava por um caminho que a casa não vê.

O caminho 1 é o mais provável, e é onde está a correcção a fazer: o
`write` da máscara na palavra da mutex, no `ArbitrateLock` e no
`SignalImpl`, tem de ser **incondicional** (como no Eden), e não
depender da contagem de *waiters* que a casa faz.

### Onde está o jogo agora — medido

| | antes | agora |
|---|---|---|
| fim do arranque | `limite de traps atingido (4000)` — impasse | **`fault de memoria do guest em 0x83b0bb44`** (endereco `0x0`) |
| o signal da cv | `acordados=0` — ninguém acordava | **`acordados=1`** ✓ |
| threads do jogo | todos parados | um corre até ao fault |

**O impasse da cv está resolvido**: o *waiter* é acordado e o jogo
executa código a sério. O que aparece a seguir é **um problema novo**:
um acesso a `0x0` dentro do wrapper de futex do SDK (`0x83b0bb44`),
com `sp=0x30fffe950`.

**O próximo passo, com a medição à mão**: despejar os **bytes do
código no PC do fault** (o tratador de traps do motor tem o PC ✓) e
descobrir o que o SDK carrega de um ponteiro nulo — é a mesma técnica
que o dump da stack no `wait` usou, e que já disse onde o jogo
estava bloqueado.

### O fault novo — o código, em bytes

O relatório de traps agora despeja os **bytes do código no PC do
fault** (o VA do guest é plano ✓):

```
[fault] cod em 0x83b0bb64:
  e0 0f 1f f8   LDR X0, [SP, #0x1F8]
  60 04 20 d4   BRK #0x100          ← o SVC do SDK
  c0 03 5f d6   RET
  ...
  e0 0f 1f f8   LDR X0, [SP, #0x1F8]
  60 04 20 d4   BRK #0x100
  e2 03 40 f9   LDR X2, [X0]        ← carrega de x0 — e x0 é NULO
  41 00 00 b9   LDR W1, [X0]
```

**É o wrapper de SVC do SDK**: o `BRK #0x100` é a chamada ao
serviço ✓, e o ponteiro para o bloco de argumentos é lido da stack
(`[sp+0x1f8]`) ✓. O fault é num `LDR` desse ponteiro **nulo** ✗.

E o último serviço servido antes do fault foi o
`nvdrv` **comando 8 (`SetAruid`)** ✓ (`pid=1` ✓) — a resposta saiu
✓ e o guest faults logo a seguir, ao voltar do SVC.

Ou seja: **o SVC foi servido, e o wrapper do SDK recebe um ponteiro
de argumentos nulo** — o que aponta para o *stack slot* do guest ter
sido estragado (ou para o wrapper estar a ser chamado com um
argumento que nunca foiposto).

### Onde o jogo está — o mapa das cv (medido, `boot_p9w.log`)

```
4687  SignalProcessWideKey key=0x10a2c7764  acordados=0     (sinal pendente ✓)
4695  WaitProcessWideKeyAtomic cv=0x10a2c7844 tag=0x20003   (worker A)
4700  WaitProcessWideKeyAtomic cv=0x10a2c7b0c tag=0x28004   (worker B)
4706  SignalProcessWideKey  key=0x10a2c7b0c acordados=1      ← worker B acorda ✓
4708  WaitProcessWideKeyAtomic cv=0x10a2c7bec tag=0x28004   (worker B, próximo posto)
11916 SignalProcessWideKey  key=0x10a2c7844 acordados=0      ← worker A NÃO é encontrado
11917 WaitProcessWideKeyAtomic cv=0x10a2c7d64 tag=0x81ff    (main, à espera da frame)
```

**O worker B acorda ✓** (o signal pendente + o signal que acorda o
*waiter* funinam ✓) — **o worker A não** ✗: o signal chega e a fila
da cv está vazia ✗, apesar de o worker A ter estacionado lá (linha
4695 ✓).

E a correcção do *prazo de salvaguarda* (que tirei — com ele, o
*waiter* desistia e saía da fila, e o signal posterior não tinha
ninguém ✓) é o que torna isto visível.

**O passo seguinte é exacto**: instrumentar a *inscrição* do *waiter*
na fila da cv e a *consulta* no signal (quantos *waiters* há em cada
fila, e com que chave) — para ver onde a inscrição do worker A se
perde. É uma linha de instrumentação e fecha o caso.

### O próximo passo, exacto: o pedido sem o cabeçalho de domínio

O log dá o par:

```
624   HSR appletOE: comando 0 tipo=6 tem_domain=1   → OpenApplicationProxy ✓
11874 HSR appletOE: comando 0 tipo=0 tem_domain=0   → o MESMO comando, como comando
```

E logo a seguir, **dois asserts do Eden**:
```
hle_ipc.cpp: assert data_payload_header->magic == MakeMagic('S','F','C','I')
service.cpp: assert command_type=0
```

Ou seja: o pedido do jogo **chegou sem o cabeçalho de domínio**
(`tem_domain=0` ✗), mas a sessão *é* de domínio ✓ (a primeira
chamada, a de controlo, foi `tipo=6 tem_domain=1` ✓). O Eden lê o
cabeçalho de domínio onde não está ✗ → todos os deslocamentos
erram ✗ → o `magic` do payload não é "SFCI" ✗ → assert ✗, e o
comando é lido do sítio errado ✗.

**De onde vem**: o Eden devolve as sub-interfaces com
`PushIpcInterface` (`ipc_helpers.h:114-131`) — e, quando a sessão é
de domínio, o objecto vai por `ctx.AddDomainObject(...)` ✗ (e o
gestor de sessões fica com a *interface* como handler ✓). A casa
respondeu ao `OpenApplicationProxy` de forma que o guest **não sabe
que está numa sessão de domínio** ✗ — e manda o pedido sem o
cabeçalho ✗.

**O que fazer**: comparar a resposta da casa ao `OpenApplicationProxy`
(control, tipo 6 ✓) com a do Eden (`applet_oe.cpp`) — em especial
o handle devolvido e a forma como a sessão é criada (de domínio ✓ ou
não ✗) — e corrigir a resposta ✗. Com isso, o pedido do
`GetEventHandle` entra bem ✓, o `applet:oe` responde ✓, e o jogo
deixa de bater no assert ✗.

### O emulador Maxwell do Eden — o mapa (fonte: upstream)

O Eden **não** tem uma "tabela de traduções": tem o **motor Maxwell**
a executar os comandos do push buffer. O mapa (todos os ficheiros
confirmados no upstream):

| ficheiro | o que é |
|---|---|
| `src/video_core/dma_pusher.h` | o `CommandHeader` (o `u64` de 8 bytes da lista) e o `CommandHeader` do método: método (13 bits), subcanal (3), `arg_count` (13), modo (3) |
| `src/video_core/dma_pusher.cpp` | `ProcessCommands` — **o walker**: a máquina de estados que consome os métodos e os seus argumentos |
| `src/video_core/engines/puller.h/.cpp` | os métodos do **canal**: o bind dos subcanais, os semáforos e a **fence** (as posições dos registos estão nas `ASSERT_REG_POSITION` ✓) |
| `src/video_core/engines/maxwell_3d.h/.cpp` | o motor 3D: `CallMethod` → `ProcessMethodCall`; o ID do método é o **índice do registo** no `Regs` (`MAXWELL3D_REG_INDEX` ✓) |
| `src/video_core/engines/draw_manager.cpp` | os comandos de desenho |
| `src/video_core/renderer_vulkan/maxwell_to_vk.cpp` | a tradução do **estado** Maxwell para Vulkan (samplers, formatos, blend/depth) |

Os IDs dos métodos do canal (do upstream, `puller.h` ✓):
`semaphore_address` 0x04/0x05, `semaphore_sequence` 0x06,
`semaphore_trigger` 0x07, `semaphore_acquire` 0x1A, `semaphore_release`
0x1B, `fence_value` 0x1C, **`fence_action` 0x1D** ✓ — e a `fence_action`
é `{op: bit 0, syncpoint_id: bits 8..31}` ✓.

### O que foi implementado com esta fonte

- **`src/gpu/pushbuffer.cpp` + `include/g610/gpu/pushbuffer.hpp`** — o
  leitor do fluxo de métodos ✓: uma porta directa de
  `DmaPusher::ProcessCommands` ✓, com a mesma máquina de estados
  (Increasing / NonIncreasing / Inline / IncreaseOnce ✓) e as mesmas
  regras de incremento do método ✓.
- **`nvdrv`** — o push buffer é lido com o formato certo ✓ e os
  métodos do **canal** são executados ✓:
  - o **payload do semáforo** (0x07 com `WriteLong` ✓) é escrito na
    memória do guest ✓ e quem espera por esse endereço é acordado ✓ —
    é assim que o driver diz ao jogo "o trabalho acabou" ✓ (o
    upstream: `rasterizer->Query(..., Payload, ...)` ✓);
  - a **fence** (0x1D com `Increment` ✓) é tratada ✓.

O que **não** está: a tabela de IDs do motor 3D (o `Regs` do
`maxwell_3d.h` ✗ — os índices saem de `offsetof` ✗, e o `video_core`
não está vendorizado ✗ nem no Pi ✗), o estado, e o backend.

### O caminho que resta (mecânico, com o upstream)

1. **Vendorizar o `video_core` do Eden** ✓ (o upstream tem-no inteiro
   ✓ — `src/video_core/**` ✗) e juntar-lhe o `renderer_vulkan` ✓. É o
   que dá o motor Maxwell e a tradução Maxwell→Vulkan **do próprio
   Eden** ✓ — sem reescrever nada ✓. Depende do `core/` ✓ (que já
   está vendorizado ✓) e do `common/` ✓.
2. Ligar o `nvdrv` ao `DmaPusher` do upstream ✓: o `SubmitGPFIFO`
   entrega a lista de `CommandListHeader` ✓ e o motor executa ✓.
3. O *presenter* (Wayland/SDL ✓) mostra o frame ✓.

### O `video_core` do Eden — vendorizado

O motor Maxwell 3D e a tradução Maxwell→Vulkan do **próprio Eden**
foram trazidos para `vendor/eden/src/` ✓ (624 ficheiros, 5,8 MiB ✓):

    vendor/eden/src/video_core/**          — o motor Maxwell, o puller,
                                             o dma_pusher, o renderer_vulkan
    vendor/eden/src/shader_recompiler/**   — a conversão NVIDIA GLSL → SPIR-V

A ferramenta é `tools/vendorizar_video_core.py` ✓ (tem `--fetch` ✓) —
e o que ela traz substitui o `ShaderCache::compile` da casa, que hoje
é um *placeholder* ✓ (copia o blob para dentro de um vector `spirv` ✗).

O que falta para compilar isto dentro da casa: as dependências do
`renderer_vulkan` (o **PanVK** ✗ — o driver Vulkan-sobre-Vulkan que o
Eden usa ✗) e as flags de compilação ✗. É mecânico, mas não é trivial.

### O que falta para compilar o `video_core` na casa (medido)

As dependências declaradas no `CMakeLists.txt` do upstream (que ficou
vendorizado ✓) são:

| alvo | precisa de | na casa |
|---|---|---|
| `video_core` | `Vulkan::UtilityHeaders` (Vulkan-Headers) | **falta** |
| | `GPUOpen::VulkanMemoryAllocator` (VMA) | **falta** |
| | `dynarmic` (o JIT do guest) | **não é preciso** — o guest corre nativamente ✓ |
| | `xbyak` (só em x86-64) | **não é preciso** ✓ |
| `shader_recompiler` | `common`, `fmt` | **já tem** ✓ |
| | `sirit::sirit` (SPIRV-Reflect) | **falta** |
| | `SPIRV-Tools::SPIRV-Tools` | **falta** |

**O PanVK não é uma dependência de compilação** ✓ — o upstream
carrega-o em *runtime* (é um ICD ✗ que o carregador de Vulkan
encontra ✗), e o Pi já o tem ✓ (é o que o `tests/mostra.cpp` usa ✓✓).

**A receita, passo a passo:**

1. `tools/vendorizar_video_core.py` já traz o `video_core` e o
   `shader_recompiler` ✓; falta trazer (e compilar):
   - `KhronosGroup/Vulkan-Headers` ✓ (só cabeçalhos ✗),
   - `GPUOpen-GameFoundations/VulkanMemoryAllocator` ✓,
   - `KhronosGroup/SPIRV-Headers` ✓, `KhronosGroup/SPIRV-Tools` ✓,
     `KhronosGroup/SPIRV-Reflect` ✓.
2. No `CMakeLists.txt` da casa: os alvos `video_core` e
   `shader_recompiler` ✓, com os includes do upstream (`vendor/eden/src`
   ✗ — o upstream usa `src/` ✗, e o prefixo muda ✗ → ou um
   `target_include_directories` com `vendor/eden` ✗ e um *symlink*
   `src` → `vendor/eden/src` ✗, ou compilar com `-I` ✗).
3. O `video_core` referencia `core/` e `common/` ✗ — já vendorizados ✓
   — e `fmt` ✓.

**O atalho que o desenho da casa já previa**: o motor Maxwell fala com
um `RasterizerInterface` ✓ (uma interface abstracta ✓ do upstream ✗) —
por isso a casa **não precisa** do `renderer_vulkan` ✗ nem do PanVK
✗✗: basta implementar a interface ✓ com o backend de Vulkan ✓
(o `ValhallBackend` ✗ que o desenho já nomeia ✗). O `renderer_vulkan`
do upstream fica de fora ✗ — e com ele o PanVK ✗ e o VMA ✗ — e o
`shader_recompiler` ✗ pode ser ligado depois ✓ (é o que troca o
*placeholder* do `ShaderCache` ✓).

### A parede actual

O `OpenLayer` (comando 2020 do display da aplicação) — a **native
window**, que é o *buffer* onde o jogo desenha. É o ponto de ligação
com o caminho de render da casa (Wayland/SDL2 + panvk): o buffer que
o Eden dá ao jogo é o mesmo que o compositor da casa apresentaria.

E, a montante, o **canal** ainda não submeteu nada: o
`SubmitGPFIFO` nunca chegou a correr (0 submissões), porque o guest
parou antes no display.

**Honestidade sobre o que ainda não está feito**: o tradutor
Maxwell → Vulkan (`src/gpu/`) **traduz** o push buffer, mas **não
executa** — a execução é do `ValhallBackend` (PanVK), e essa ligação
ainda não está feita. O `nvdrv` já dá-lhe o VA e os buffers; falta quem
consome os comandos traduzidos e apresenta o frame.

## Estado

| | |
|---|---|
| `wip/passo-4-thread` | passo 6: o `nn::time`, o `fsp-srv` e o `nvdrv` (fino) registados; os SVCs `CreateTransferMemory` e `SetMemoryAttribute` implementados; os `Result` com os valores do Eden; o `notify` do `WaitProcessWideKeyAtomic` corrigido. O guest chega ao fim da init do driver da GPU (31 ioctls) e pára num fault **dentro do SDK** — a próxima parede é ligar o tradutor Maxwell → Vulkan ao IPC |
