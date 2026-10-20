
---

## Estado: 2026-10-18 — execução nativa validada

**109 verificações, 0 falhas.** O guest AArch64 corre nativamente no A76 com
interceptação de SVC medida ponta a ponta.

| Componente | Estado |
|---|---|
| Telemetria | feito — classificação por frame, 256 slots, flush JSON+CSV |
| Carregador NSO | feito — segmentos LZ4, validado no Pokémon Shield (41,9 MB) |
| NPDM | feito — 64-bit, ProcessAddress=3 (39 bits), NATIVA |
| Instruções | feito — encodings com static_assert contra o assembler real |
| Patcher estático | feito — SVC→BRK, HINT→NOP, **bug do bit 0 corrigido** |
| Motor de execução | **validado** — BRK→handler→retorno, args, resultado, faults |
| Recuperação de traps | feito — siglongjmp, restaura `sp` do host |
| Dispatcher SVC | feito — ExitProcess, SetHeapSize, portas nomeadas |
| Page tables 39 bits | **não implementado** — último obstáculo |
| `tpidr_el0` por thread | reservado — não é requisito para arrancar |

### Bugs encontrados pela execução (não pela leitura)

1. **`BRK` com bit 0 a 1** — o patcher produzia `d4200021` em vez de
   `d4200020`. Codificação **inválida**: o CPU respondia SIGILL em vez de
   levantar a trap, e o handler nunca via o primeiro SVC de um jogo.
   Três testes antigos codificavam o bug errado e passavam.

2. **Stack protector destruído** pela troca de `sp` — `run()` verificava o
   canário sobre a stack do guest. Resolvido fazendo o regresso patológico
   ser também uma trap, para passar pelo mesmo `siglongjmp`.

3. **`exited_` a contaminar execuções** — um fault de memória era reportado
   como saída limpa. `reset_run_state()` no início de cada `run()`.

### O mecanismo está documentado em `docs/ARQUITETURA.md` §9 a §12.

---

## Estado: 2026-10-18 (2ª sessão) — programa multi-módulo carregado

**142 verificações, 0 falhas** (103 core + 13 exec + 26 programa).

| | sessão 1 | sessão 2 |
|---|---|---|
| Execução nativa | validada | validada |
| Programa multi-módulo | — | **validado no ExeFS real** |
| Layout de 39 bits | — | **validado** |
| Números de SVC | errados | **corrigidos** |
| Despachante ligado ao guest | dereference directo | **validado** |
| `MapMemory` / `UnmapMemory` | ausente | **implementados** |
| `SetHeapSize` devolvia 0 | — | **devolve o endereço** |

### Três bugs nesta sessão, todos do mesmo padrão

**Gaps entre segmentos.** O carregador concatenava `text|rodata|data`, mas os
vaddr não são contíguos: há **3328 bytes de gap** no `main` medido. A rodata
aterraria deslocada — e é lá que vivem as tabelas de import que o rtld
percorre. Nenhum teste de invariante apanharia isto; só um teste que lê de
volta o que escreveu, pelo caminho que o guest usaria.

**Números de SVC errados.** `ExitProcess = 0x03` quando é `0x07`;
`GetSystemTick = 0x6E` quando é `0x1E`. Escritos de memória em vez de
copiados de `svc.h`. O `svc #3` do guest sintético era `SetMemoryAttribute`.

**`SetHeapSize` devolvia 0.** x0 à entrada é o *tamanho*; à saída é o
*endereço*. Devolver 0 fazia o rtld escrever o heap na página zero.

Todos: um valor suposto em vez de medido. Todos: sintoma idêntico — o guest
não arranca e a causa não é óbvia. A regra está em `ARQUITETURA.md` §21.

### Base de endereçamento corrigida

De `0x7FFF00000` (30 GiB acima) para `0x8000'0000`, que é o `code_address`
que o Switch dá a um processo 39-bit. O Eden carrega a ~2 GiB de propósito:
programas como Skyline dependem indevidamente de páginas ASLR antes do entry
point, e carregar alto deixa esse buraco para trás.

### Sway: instalado, desactivado, revertido

Medido: 4% de um núcleo (~0,5% dos 8). O modo legado em eglfs/KMSDRM está
**partido** neste device (`Could not restore CRTC` → pageflip em ciclo), e
dá 92% de CPU. Desligar o sway trocava 4% por 92% e um ecrã preto.
Documentado em `SESSAO_GRAFICA.md`.

### Caminho até ao jogo a correr

Faltam, por ordem de bloqueio:

1. `QueryMemory` — o rtld pergunta permissões antes de mapear
2. `WaitSynchronization` — o rtld espera pelo thread que carrega os módulos
3. depois: `ConnectToNamedPort("nvdrv")` → o jogo pede a GPU → entra o
   Maxwell→Valhall, e a telemetria passa a dizer se o gargalo é CPU, GPU ou
   sincronização

---

## Estado: 2026-10-18 (3ª sessão) — o rtld real arranca e fala

**142 verificações, 0 falhas** (103 core + 13 exec + 26 programa).

### O rtld real já corre

`g610_boot` corre o entry point real do rtld do Pokémon Shield. Antes desta
sessão o guest **girava para sempre sem levantar uma trap** — o sintoma mais
opaco possível. Agora:

```
[trap #1] sig=5 pc=0x8000170c insn=0xd4200500 (BRK = SVC patchado)
[svc #1] 0x28 Svc#40  x0=0xf801 x18=<canary> x19=0x0 x29=0x0 x30=0x800000f8
```

Código AArch64 válido,correto mapa de memória, primeiro SVC despachado.

### Quatro bugs nesta sessão

| | sintoma | causa |
|---|---|---|
| **Contrato de entrada** | gira para sempre, sem traps | `br x0` deixa x0 = endereço do entry; o entry faz `cmp x0,#0` e espera um argumento |
| **x18 (stack canary)** | primeiro SVC é `ReturnFromException` | platform register por inicializar |
| **`hooks_` vs `g_hooks`** | guest mudo, handler visto mas inerte | `hooks_ = g_hooks` apagava `armed` e os observers |
| **`SvcContext.x[18]`** | "registos com ASCII" | leitura fora dos limites — `x` tem 6 entradas |

O primeiro é o que vale a pena reter: **um entry de jogo não é uma função C**.
Lê registos antes de os escrever, porque o contexto preparado por quem o
chamou tem tudo a zero (`ctx = {}` em `ResetThreadContext64`). O nosso
trampoline saltava com x0 = endereço do entry.

### Instrumentação construída (vale mais que os bugs)

- **Amostragem de PC com `SIGALRM` + `setitimer`.** Um guest em ciclo de
  código puro não levanta traps. Como o guest corre na sua própria stack, o
  handler do sinal vê o contexto *do guest*. Respondeu "o guest está parado em
  `pc=0x800000f8`" quando nada mais dizia alguma coisa.
- **`g610_boot --raw <ficheiro>`** despeja a imagem do entry para desassemblar
  com o `objdump`, sem debugger.
- **Limite de traps** no motor: um guest em fuga deixa de ser um processo a
  100% de CPU sem saída, e passa a ser um relatório.

### Onde está agora

O rtld pediu `svc #0x28` com `x0 = 0xf801` e ficou à espera de uma resposta.
Investigado até ao fim: **não é um hypercall, é o registo de handlers de
excepção**.

O slot que o código lê (`adrp 0x3000; ldr [x2,#432]` = 0x30B0) vale `0x2610`,
e esse offset não é código — é a string `_ZN2nn2os6detail20UserExceptionHandlerE`.
O rtld está a instalar `nn::os::detail::UserExceptionHandler` antes de correr o
jogo, que é a primeira coisa que um runtime faz.

`0xf801` não é um número de serviço: é um **código de registo** (bit alto = classe,
resto = índice). Não está no `svc.h` do Eden porque **não é um SVC do kernel** —
é uma chamada à API de excepções do runtime `nn::os`.

**Isto não é um bug do motor.** É uma funcionalidade por implementar. O motor
despachou o SVC, registou-o, e o guest ficou à espera — que é o comportamento
correcto para um serviço por implementar.

Ver `ARQUITETURA.md` §24-25 para o raciocínio completo, incluindo a hipótese
que testei e refutei.

### Sequência real de arranque de um título

```
1. rtld arranca                                    <- estamos aqui
2. rtld regista handlers de excepção                <- BLOQUEIO ACTUAL
3. rtld mapeia memória para main e sdk
4. rtld resolve imports da sdk
5. rtld chama o entry do main
6. o jogo regista os seus handlers e arranca
7. o jogo pede a GPU por ConnectToNamedPort("nvdrv")
```

### Boot chain: o rtld fala (5ª sessão)

**153 verificações, 0 falhas.** O rtld deixou de girar em silêncio e **diz o
que não encontra**:

```
[rtld] Unresolved symbol: '_ZN2nn4init5StartEmmPFvvES2_'
```

É uma mensagem do próprio guest, lida das strings no binário — não uma
inferência nossa. `nn::init::Start` é o entry point do `main`.

**Dois bugs de ABI:**

1. **`x0` nunca recebia o Result.** O handler escrevia `regs[0] = c.ret` e logo a
   seguir o ciclo `regs[i] = c.x[i]` o sobrescrevia com `c.x[0]` — que para a
   maioria dos serviços é o *endereço de saída*. Na ABI do Switch `x0` é o
   registo de retorno.

   O rtld diz isso literalmente no offset 0x4dc do binário:
   ```asm
   4dc:  cbnz w0, 0x708    ; Result != 0 -> __builtin_unreachable()
   ```

2. **`dispatch()` apagava os valores de retorno.** Fazia `ctx.ret = resultado`
   no fim — mas `SetHeapSize`, `GetSystemTick` e `CloseHandle` devolvem um
   *valor*. Corrigido com `ctx.has_ret`.

**Sequência de serviços agora observada:**

```
#1 0x06 QueryMemory        x0=0x847fff68 x1=0x847fff60 x2=0x0
#2 0x27 OutputDebugString  x0=0x800027df x1=0x1b
#3 0x27 OutputDebugString  x0=0x8000263a x1=0x1c
#4 0x27 OutputDebugString  x0=0x800027fb x1=0x2
```

**Medido — e isto importa:** as tabelas de símbolos estão **todas carregadas**,
com offsets válidos dentro das imagens:

```
rtld    20 símbolos     main   905 símbolos     sdk  25 091 símbolos
```

Os dados estão lá e bem endereçados. Falta é o rtld **saber** onde estão: ele
procura o bloco marcado por `__nnDetailNintendoSdkRuntimeObjectFile`, que não
existe no espaço do guest.

Toda a informação necessária já está no nosso `NsoHeader`. Falta escrever o
bloco no argumentos de 0x9000 bytes. O formato (`NsModInfo`) não está no
Eden — porque o Eden não o precisa — e é a primeira peça sem referência local.

### Boot chain: os símbolos resolvem-se (6ª sessão)

**`[rtld] Unresolved symbol: '_ZN2nn4init5StartEmmPFvvES2_'` — deixou de aparecer.**

O que aolvia não era uma lista de módulos em falta. Era que o rtld **não
recebe lista nenhuma**: varre a memória com `svcQueryMemory` e aceita uma
região só se `permission == 5` e `state == 3`, e depois procura um cabeçalho
`MOD0` no início dela. Descompilei o `rtld` do jogo para confirmar, e o
formato confere com as imagens descomprimidas.

**Três bugs:**

1. O `svcQueryMemory` devolvia a reserva inteira como `Normal`/`RWX`. O rtld
   não aceitava nenhuma região. → agora devolve uma tabela por segmento, com
   `.text` como `Code`/`R-X`.
2. O `.bss` (3,4 MB no `main`) ficava **fora** do layout e escrebia por cima
   da `sdk`.
3. A tabela tinha buracos. O rtld pára no primeiro endereço não descrito.

**Estado do arranque, medido:**

```
QueryMemory x12   — a varrura encontra os três módulos
GetInfo     x3    — 0xb (entropia), 0xc (região ASLR)
```

O SDK do jogo arrancou. Passou a pedir coisas ao kernel.

**O obstacle seguinte:** o guest lê o seu TLS de `TPIDR_EL0`, que num guest
nativo é o `fs` da thread do Linux. Neste kernel não há forma de o
programar — `msr` dá SIGILL e `arch_prctl(ARCH_SET_FS)` devolve -1, ambos
medidos num programa de dez linhas. O motor passou a emular a **leitura**
(recuo de até quatro instruções até encontrar o `mrs`, escreve a base do TLS
do guest, retoma). Com isso o guest passa o fault anterior e falha no
seguinte: falta inicializar o bloco de TLS.

### Boot chain: destravado (5ª sessão)

**153 verificações, 0 falhas.** O rtld deixou de girar em silêncio e **diz o
que não encontra**:

```
[rtld] Unresolved symbol: '_ZN2nn4init5StartEmmPFvvES2_'
```

É uma mensagem do próprio guest, lida das strings no binário — não uma
inferência nossa.

**Dois bugs de ABI encontrados:**

1. **`x0` nunca recebia o Result.** O handler escrevia `regs[0] = c.ret` e logo
   a seguir o ciclo `regs[i] = c.x[i]` o sobrescrevia com `c.x[0]` — que para a
   maioria dos serviços é o *endereço de saída*. Na ABI do Switch `x0` é o
   registo de retorno.

   O rtld diz isso literalmente no offset 0x4dc do binário:
   ```asm
   4dc:  cbnz w0, 0x708    ; Result != 0 -> __builtin_unreachable()
   ```

2. **`dispatch()` apagava os valores de retorno.** Fazia `ctx.ret = resultado`
   no fim — mas `SetHeapSize`, `GetSystemTick` e `CloseHandle` devolvem um
   *valor*, não um Result. A escrita final deitava-o fora. Corrigido com
   `ctx.has_ret`.

**Sequência de serviços agora observada:**

```
#1 0x06 QueryMemory        x0=0x847fff68 x1=0x847fff60 x2=0x0
#2 0x27 OutputDebugString  x0=0x800027df x1=0x1b
#3 0x27 OutputDebugString  x0=0x8000263a x1=0x1c
#4 0x27 OutputDebugString  x0=0x800027fb x1=0x2
```

**Bloqueio actual:** falta a metadata dos módulos. O rtld resolve símbolos
pela GOT e pelas tabelas `rela_dyn`, cujos limites vem do cabeçalho NSO — e
esse cabeçalho não está no espaço do guest. O bloco de argumentos de 0x9000
bytes que o Eden reserva é onde isso vive.

### Boot chain: destravado (5ª sessão)

**O rtld agora segue o caminho correcto.** O primeiro SVC deixou de ser
`ReturnFromException` (com um x0 que não era endereço nenhum) e passou a ser
`QueryMemory` — o primeiro serviço que um rtld pede.

```
[svc #1] 0x06 QueryMemory  x0=0x847fff68 x18=0x800003c0
                      x19=0x80000000 x29=0x847fffe0 x30=0x800004dc
```

Todos os registos passam a fazer sentido: `x19` é a base do rtld, `x29` o
frame pointer, `x30` um endereço de retorno **no guest**. Antes eram lixo.

**A causa:** o trampoline fazia `mov x0, xzr` (o argumento tem de ser 0) e
logo a seguir `ldr x0, [sp]` (o endereço de entrada) — **a segunda linha
desfazia a primeira**. Como o entry faz `cmp x0, #0` na primeira instrução, o
guest ia pelo caminho errado.

Descoberto perguntando **"quem chamou esta função?"** e constatando que
ninguém — só a via `b.ne`, que depende de x0 ≠ 0. Três hipóteses anteriores
tinham sido testadas e refutadas antes disso.

`QueryMemory` implementado com o layout exacto de `Svc::MemoryInfo` (40 bytes).

**153 verificações, 0 falhas.**

### Input e foco (4ª sessão)

**153 verificações, 0 falhas** (103 core + 13 exec + 26 programa + 11 input).

| | |
|---|---|
| Save mais recente | `2026-10-17 23:38`, 1,5 MB, verificado byte a byte |
| DS4 | `/dev/input/event1`, GUID `0300d042...` bate com `SDL_GAMECONTROLLERCONFIG` |
| Botões | todos presentes; 2 analógicos + triggers + D-pad |
| Foco | **corrigido** — `place()` nunca focava a janela |

**Dois bugs reais encontrados:**

1. **A/B trocados.** O SDL segue a convenção PlayStation (o seu `A` é o botão de
   baixo, que na Nintendo é `B`). Usar directamente dá botões trocados. Corrigido
   num sítio só, em `apply_swap_a_b()`. O eixo Y também precisava de inversão, e
   os triggers não têm centro.

2. **A janela do jogo nunca recebia foco.** `emuos-display game` maximiza a
   janela mas não a foca, e `focus-primary` focava o menu. O wlroots envia o
   gamepad para a janela focada — ou seja, todo o input ia para o menu. É a
   razão pela qual um emulador claramente visível "não responde a nada".

Backup: `/usr/local/bin/emuos-display.bak-20261018`.
Patch (só adiciona): `tools/patch_focus.py`.

### Sobre os 30/60 FPS

Não há caminho para o overworld enquanto o passo 2 não estiver implementado.
O orçamento de frame do Switch é 30 FPS (60 é o modo de Competitive de alguns
títulos), e o gargalo vai aparecer na telemetria — que é precisamente o que
ela existe para dizer. Nada disto se decide antes de o jogo correr.
