# Análise de estado — 29/10/2026

> Objetivo do projeto: **pixels reais de Pokémon Shield na tela física** da
> Orange Pi 5 Pro (RK3588S, Mali-G610), pelo caminho
> guest ARM64 nativo → SVC/IPC → vi/nvdrv → GPFIFO → Maxwell → Vulkan → tela.
>
> Este documento responde a duas perguntas: **o que já está feito** e **o que
> falta**, com evidência no código/binários/logs. Não substitui o
> `PROGRESSO.md`; corrige-o onde ele ainda descreve o modo artefacto.

---

## 1. Resposta curta

O projeto **não está perto do fim**. Está no **primeiro terço do caminho**,
e o terço mais difícil (GPU) ainda não foi realmente começado — existe um
esqueleto, não um motor.

O que está sólido: a **cadeia de arranque nativa** (rtld frio → SDK) e a
**fundação de IPC**. O que falta: terminar a **inicialização do SDK**
(bloqueada hoje por patches nossos), **threads reais**, **fs**, **GPU a
sério** e **apresentação ligada ao jogo**.

Três factos que resumem a distância:

1. O guest fez **46 SVCs** e **1 único `SendSyncRequest`** (o `sm:GetService("lm")`).
   Um jogo comercial em arranque faz dezenas de milhares, em várias threads.
2. O guest **nunca chamou** `nvdrv`, `vi`, `fsp-srv`, `CreateThread` nem `MapMemory`.
   Todo o código desses serviços existe, mas **nunca foi exercitado pelo guest**.
3. O motor Maxwell do upstream (o que traduz comandos e shaders a sério) está
   **vendorizado mas fora do build** (`CMakeLists.txt:120-240`). O que está no
   build é um tradutor de 31 KB. A diferença é de ordem de magnitude.

---

## 2. Onde o guest pára hoje (evidência)

Run de referência: `/tmp/opencode/last_run_text.log`.

| Medida | Valor |
|---|---|
| SVCs | 46 |
| traps | 47 |
| faults | **0** |
| tipos distintos de SVC | **8** (QueryMemory, GetInfo, GetThreadPriority, GetThreadId, ConnectToNamedPort, SleepThread, SendSyncRequest, SignalProcessWideKey) |
| `SendSyncRequest` | **1** (`sm:GetService("lm")`, respondido pelo HLE) |
| último PC | `0x83ab15e4` (self-loop) |
| `MapMemory` | 0 |
| serviços registados | 24 (`nvdrv`, `vi:m/s/u`, `fsp-srv`, …) |

Sequência final: `ConnectToNamedPort("sm:")` → `sm:GetService("lm")` OK →
`SignalProcessWideKey` (`0x8437e20c`) → **aborto do SDK** em `0x83ab1500` →
loop em `0x83ab15e4`.

### O loop é nosso, não do jogo (provado)

```
0x83ab15e0:  bl 0x83c9d030        ; AbortImpl via PLT → GOT[0x8438ec80]
0x83ab15e4:  (zeros → patchado para `ret`)
```

* `GOT[0x8438ec80]` foi patchado para `0x83aec290` (`ret 0`) — `guest_program.cpp:808`.
* `bl` deixa `x30 = 0x83ab15e4`.
* `0x83ab15e4` foi patchado para `ret` — `guest_program.cpp:515-525`.
* Logo: `ret` salta para `x30 = 0x83ab15e4` → **loop infinito**.

O exemplo confirma: `[sample] pc=0x83ab15e4 x30=0x83ab15e4 lr1=0x83ab1500
lr2=0x83c2b854 …`. E `lr2=0x83c2b854` é o endereço de retorno **dentro da
função em que o patch `0x83c2b844` nos fez entrar**.

Conclusão: **o bloqueio atual é 100 % auto-infligido**. O primeiro passo não é
"corrigir o guest", é **tirar as máscaras e ver o erro verdadeiro**.

---

## 3. O que está feito (e é sólido)

| # | Componente | Evidência | Estado |
|---|---|---|---|
| 1 | Loader NSO/NCA (LZ4, segmentos, MOD0, dynsym/dynstr, RELATIVE + JUMP_SLOT) | `src/nso_loader.cpp`, log de carga dos 5 módulos | ✅ provado |
| 2 | Layout/mapping do VA de 39 bits (43 GiB reservados, heap, stack, TLS, alias) | `src/guest_program.cpp`, `guest_placement.cpp` | ✅ provado |
| 3 | Execução nativa + traps por sinal (SVC patchado a BRK) | 46 SVCs, 0 faults | ✅ provado |
| 4 | **Arranque frio pelo rtld do próprio jogo** (SC1–SC5) | `PROGRESSO.md:348-460`; zero forçamento de PC | ✅ provado |
| 5 | Despachante SVC | 77 `case Svc::` em `src/svc_dispatcher.cpp` | ✅ funcional p/ o subconjunto usado |
| 6 | IPC CMIF + `sm:` (Initialize/GetService) | 1 pedido real respondido com handle | ✅ provado (1 caso) |
| 7 | 24 serviços HLE registados | log linhas 40–68 | ✅ registados, quase todos **não exercitados** |
| 8 | Kernel HLE do Eden vendorizado (`KClientSession`, `KConditionVariable`, …) | `vendor/eden/core/hle/kernel/` | ✅ usado no IPC |
| 9 | Vulkan no Mali-G610 (device, clear, triângulo, readback offscreen) | `g610_vulkan_pipeline`, `g610_rasterizer_clear` | ✅ provado, **offscreen** |
| 10 | Apresentação Wayland/sway + Vulkan WSI | `tests/mostra.cpp` (triângulo laranja medido com `grim`) | ✅ provado, **desligado do emulador** |
| 11 | RomFS/NSP + `fsp-srv` | `src/fs/romfs.cpp`, `src/services/fsp_srv.cpp` | 🟡 código existe, **nunca chamado** |
| 12 | Threads de guest (criar/arrancar, TLS por thread) | `svc_create_thread`/`svc_start_thread` | 🟡 scaffolding, **nunca exercitado** |
| 13 | `nvdrv` (nvmap, canal, GPFIFO, syncpoints) | `src/services/nvdrv.cpp` (162 KB) | 🟡 código existe, **nunca chamado pelo guest** |
| 14 | Tradutor Maxwell | `src/gpu/maxwell_translator.cpp` (31 KB) | 🟡 **0 DrawCommands** no pushbuffer real |

Legenda: ✅ provado em execução · 🟡 existe mas não provado.

---

## 4. O que parece feito mas não está (máscaras e atalhos)

Esta é a parte que **tem de ser paga antes de avançar**, senão cada passo
seguinte é construído sobre areia.

### 4.1 Treze patches no SDK (7 de imagem + 6 de runtime)

Imagem (`src/guest_program.cpp:405-577`):

| Patch | Endereço | O que faz | Risco |
|---|---|---|---|
| `unwind-patch` | `0x83cb75e8` | `__cxa_throw` → `ret x0` | **mata todas as exceções C++**; salta para o objeto de exceção |
| `cbnz-patch-image` | `0x83aec244` | `cbnz` → `mov w0,wzr` | esconde falha do Hipc2ClientCore |
| `abort-path-patch-image` | `0x83aec8c0/d4` | `adrp`→`add sp,#0xb0`; `bl`→`ret` | devolve de um aborto com pilha remendada |
| `abort-epilogue-patch-image` | `0x8390d1e4/e8` | `svcBreak`→NOP; `b self`→`ret x0` | aborto deixa de abortar |
| `after-abort-ret-patch-image` | `0x83ab15e4` | zeros→`ret` ×3 | **causa o loop atual** |
| `ret-after-signal-patch-image` | `0x83c2b844` | `ret`→`b 0x83c2b848` | **corrompe o fluxo**: salta para a função seguinte |
| `ret-after-signal2-patch-image` | `0x83c2b85c` | `ret`→`b 0x83c2b860` | idem |

Runtime (`src/guest_program.cpp:756-860`): `vtable[4]`→`0x83aec290`,
`GOT[0x843935a0]`→`0x83aec290`, `cbnz` repetido, `GOT[0x8438ec80]`→`0x83aec290`,
`svc #0x26`→NOP, epílogo de `AbortImpl` reescrito.

**O "boot natural" (SC1–SC5) só cobre forçamentos de PC em runtime.**
Estes 13 patches aplicam-se **sempre** — não têm gate `natural_boot()`. Ou
seja: o SC5 ("zero [patch]/[illeg]") é verdadeiro para o controlo de fluxo de
arranque, mas **não** para o binário do SDK que está a correr.

### 4.2 Sucessos falsos no IPC

* `src/eden_bridge.cpp:1047-1070`: quando `ProcessPendingSynchronously()`
  devolve `ResultNotFound`, escreve **sucesso genérico** no buffer do guest.
  O guest passa a acreditar que o serviço respondeu.
* `src/ipc_min.cpp:658-663`: bypass `lm:` com `write_empty_reply(..., 0)`.
  Hoje dormente (o handle do Eden não está na tabela local), mas é uma bomba
  relógio: se o nome voltar a resolver, volta a mentir.
* `src/eden_bridge.cpp:1022-1035`: o diagnóstico lê o `cmd_header` **depois**
  de `ProcessPendingSynchronously()`, portanto imprime a **resposta** como se
  fosse o pedido (`cmd_id=0 type=0`). Foi isto que gerou a confusão do
  "comando 0".
* `assert data_payload_header->magic == SFCI` dispara em `hle_ipc.cpp:148+` —
  o layout CMIF pedido↔resposta ainda não está fechado.
* `vendor/eden/core/hle/service/sm/sm.cpp` (`GetServiceImpl`) adiciona a
  sessão à tabela de handles **antes** do `PushMoveObjects` — possível handle
  duplicado/fuga sob `IA_IPC_TRACE`.

### 4.3 Documentação desatualizada

As secções do `PROGRESSO.md` de 25/10 (spin `0x805e7060`, vsync→CV, produtor
`x20+0x308`, `SubmitGPFIFO #0`) foram medidas em **modo artefacto**, com PC
forçado. Não são a fronteira atual e não devem ser usadas para planear.

---

## 5. O que falta — fases, com critério de saída

### Fase 0 — Auditoria: tirar as máscaras (1 sessão)

* Remover/`gate` os 7 patches de imagem e os 6 de runtime, guardando `.bak`.
* Corrigir `eden_send_sync_request` (ler o pedido **antes** de processar,
  nunca fabricar sucesso em `ResultNotFound`).
* Remover o bypass `lm:`.
* Fechar o layout CMIF (SFCI/SFCO) contra o parser do guest.

**Saída:** run mostra o **primeiro aborto verdadeiro**, com mensagem real do
`svcBreak` (string em `x1`), sem `[*-patch]` no log. Nada de loop.

### Fase 1 — Terminar a inicialização do SDK (bloqueio atual)

O aborto em `0x83ab1500` compara `[x1+0x10]` (dono do mutex) com
`[tpidrro_el0+0x1F8]` (`tls_tp = 0x84432bc0`). É uma verificação de posse de
`InternalCriticalSection`. Com as máscaras fora, saber-se-á se:
(a) o dono é 0 porque o `Enter()` foi saltado por um stub, ou
(b) a identidade de thread do SDK não é o `tls_tp` que estamos a fornecer.

**Saída:** o guest passa dos 46 SVCs e começa a pedir `fsp-srv`/`set`/`appletOE`,
`SetHeapSize`/`MapMemory` e `CreateThread`.

### Fase 2 — Threads reais e arbitragem

Hoje `CreateThread` é um **stub** (devolve handle, não cria thread) e o
despachante tem **um único estado** (`g_dispatch_owner`). O jogo cria a thread
de render e a de áudio; sem elas não há frame.

**Saída:** duas threads de guest a correr em simultâneo, cada uma com o seu
TLS e o seu estado de despacho, 60 s sem deadlock. `SignalProcessWideKey` /
`ArbitrateLock` corretos com 2+ threads.

### Fase 3 — `fsp-srv` / RomFS / dados do jogo

O NSP existe (`~/piDroid/roms/Pokemon Shield …9.53 GB.nsp`), o parser existe,
o `fsp-srv` existe — mas o guest **nunca abriu um ficheiro**. Sem isto o jogo
não carrega os NCA/romfs e não chega ao `nn::oe::Initialize`.

**Saída:** `OpenFileSystem`/`OpenFile`/`Read` reais, com conteúdo a bater
certo com o NSP; o guest lê o primeiro ficheiro de dados.

### Fase 4 — `nvdrv` real (nvmap, canal, GPFIFO)

**Saída:** o guest aloca nvmap, abre canal, faz `SubmitGPFIFO` com um push
buffer real, o syncpoint avança — e o tradutor produz **> 0 DrawCommands**.

### Fase 5 — Maxwell → Vulkan (o grosso do trabalho)

Aqui há uma **decisão estratégica** (ver §6). O que existe hoje: 31 KB de
tradutor, 15 KB de rasterizador, 37 KB de backend — suficiente para o
triângulo sintético, **não** para o Maxwell 3D de um jogo comercial.

**Saída:** um frame renderizado offscreen cujos pixels **não** são o clear
(verificação por hash/região, não por "correu sem erro").

### Fase 6 — Apresentação ligada ao jogo

O `g610_mostra` já põe um triângulo no HDMI via Wayland/panvk (medido com
`grim`). Falta ligar a textura do jogo ao swapchain e o compositor (`vi` /
`binder`) ao buffer da camada.

**Saída:** **pixels do jogo na tela física** — o objetivo. Captura `grim`
não-preta e não-estática.

### Fase 7 — Estabilidade

Frame pacing, vsync ligado ao evento do guest, input (`hid`), áudio (não
necessário para o objetivo), 30+ fps.

---

## 6. Decisão estratégica: que motor gráfico?

| | Caminho A — motor do upstream | Caminho B — tradutor da casa |
|---|---|---|
| Código | `vendor/eden/src/video_core` (316 `.cpp`, 5,1 MB) + `shader_recompiler` (2,3 MB) | `src/gpu/*` (~85 KB) |
| Estado | vendorizado, **fora do build** (`CMakeLists.txt:120-240`) | no build, **0 draws reais** |
| Bloqueios | precisa do `KernelCore` completo (`GetMemoryManager`, `GetCurrentProcess`, `Core::Timing`, `CpuManager`) e de um `sirit` com a API nova | precisa de reimplementar Maxwell 3D + SSA + SPIR-V (ordem de 10⁵ linhas) |
| Prazo realista | semanas/meses, mas caminho conhecido | meses/anos, caminho inédito |

**Recomendação:** Caminho A. O `core/` do Eden já está vendorizado; o que
falta é a infra de emulação (`Core::Timing`, `CpuManager`, `Memory`), que é
bem menor do que reescrever o `video_core`. O `RasterizerG610` da casa passa
a ser **harness de teste**, não o motor.

Esta decisão deve ser tomada **antes** da Fase 4, porque muda as interfaces
do `nvdrv`/`vi`.

---

## 7. Riscos principais

1. **Threads**: o despachante é single-state; o modelo de execução nativa por
   sinais tem de passar a multi-thread com TLS por thread. É o maior risco
   arquitetural depois da GPU.
2. **Kernel HLE incompleto**: os serviços do Eden assumem objetos de kernel
   que a casa só tem parcialmente (memória, agendador).
3. **PanVK/Mali**: sem geometry shaders; o `shader_recompiler` tem de baixar
   geometria para compute/vertex — já é o que o Eden faz, mas é superfície.
4. **Memória guest↔host**: o `video_core` espera `GuestMemory` do Eden, não o
   nosso `GuestMemory` — camada de adaptação a escrever.

---

## 8. Próxima ação concreta (ordem)

1. **Tirar as máscaras** e re-correr → obter o aborto verdadeiro.
2. Corrigir o `eden_send_sync_request` e o layout CMIF.
3. Só depois: diagnosticar a posse do mutex em `0x83ab1500`.
4. Decidir o motor gráfico (§6) e começar a Fase 4 em paralelo com a 1–2.

**Não** voltar a "avançar" o PC, a stubbear abortos ou a escrever sucesso
genérico. Cada uma dessas linhas custou, nas sessões anteriores, uma
regressão disfarçada de progresso.
