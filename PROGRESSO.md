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
| Patch de `MRS TPIDRRO_EL0` | **em curso** |
| Passar o crt0 | **não** |
| Serviços / nvdrv / GPU | fora de âmbito |

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

---

## Não confirmado (não inventar)

- `__tls_align` do SDK oficial → usamos 16 como mínimo.
- `Handle` válido para a thread principal → usamos `1`.
- Como o kernel do Switch dimensiona o bloco estático de TLS da thread
  principal a partir do NPDM → **não investigado**.