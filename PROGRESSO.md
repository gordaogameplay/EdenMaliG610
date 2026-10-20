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

### Três tentativas, sem avanço

| # | O que mudei | Resultado |
|---|---|---|
| 1 | resposta com magic `SFCI`, `type = 6` | `Break(x0=0)` |
| 2 | limite de escrita à MessageBuffer (0x100) | `Break(x0=0)` — mas **fixou uma regressão real** |
| 3 | magic `SFCO`, `type = 0`, `data_size = 8` | `Break(x0=0)` |

A tentativa 2 valeu a pena por si: o buffer de IPC **é** a MessageBuffer do
ThreadLocalRegion, e a primeira versão limpava `size` = 0x1000 bytes a partir
do corpo — o que apagava os ThreadVars em +0x1E0 e partia o TLS. Um sucesso
falso teria sido pior que o `Break`.

### O que NÃO está resolvido

O `Break(x0=0)` continua. **Não sei se é**:
- o `CommandHeader` da resposta (a posição do payload depende de quantos
  descritores há, e a resposta não tem nenhum — o `ResponseBuilder` alinharia
  o payload em 0x18, não em 0x20);
- o SDK precisar de mais do que um Result de `Initialize`;
- ou a resposta ter de ser escrita noutro sítio (o Eden, em modo HLE, tem
  `ctx.CommandBuffer()` a apontar para o buffer do cliente — que é o que
  fazemos — mas não confirmei que a geometria é a mesma).

Paro aqui pela regra das três tentativas.

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