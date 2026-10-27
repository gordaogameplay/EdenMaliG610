# PROGRESSO — EdenMaliG610 / C8: JOGO NA TELA

## Estado atual (25/10/2026)

### O que foi alcançado

1. **Deadlock entre threads eliminado**: removida a chamada a `DespertarUmWaiterDeCadaFilaDeMutexECvs()` de `svc_signal_process_wide_key` quando `total==0`. Esta função acordava indiscriminadamente threads de TODAS as filas de cv (e uma de cada fila de mutex), violando a semântica de sincronização. Após a remoção, o guest avança sem deadlock (1015+ SVCs em 120s vs 423 antes). Sinais e waits agora correspondem corretamente (ex. cv=0x10a5096e4: wait em svc #270/#410/etc., signal em svc #1375/#1382). Arquivo: `src/svc_dispatcher.cpp`.

2. **Sinalização de cv pelo driver após conclusão da GPU**: adicionada chamada a `SvcDispatcher::AcordarFutexNoEndereco(rs_endereco)` em `SubmitGPFIFO` após o syncpoint avançar (linhas ~2689-2696 de `nvdrv.cpp`). Implementa o comentário existente no código: "o driver tem de acordar o guest". Testado, não mudou o comportamento observável — o guest não está esperando em `rs_endereco` (0xE2D3F000).

3. **`RasterizerG610` conectado ao tradutor Maxwell**: `src/gpu/draw_consumer.cpp` + `include/g610/gpu/draw_consumer.hpp`. Após `MaxwellTranslator::translate()`, os `DrawCommand` são encaminhados para `RasterizerG610::Draw()` via `processar_draw_comandos()`. `CMakeLists.txt` atualizado. O rasterizador existe mas `Draw()` apenas incrementa contadores — não renderiza.

4. **Conflito de headers resolvido**: `draw_consumer.cpp` é o único TU que inclui `rasterizer_g610.hpp`. `nvdrv.cpp` chama via `extern` `processar_draw_comandos()`.

5. **SubmitGPFIFO funciona**: 1 submit com 624 métodos, max id=3627, 376 ids únicos, 8864 bytes. draw_begin(1414)/draw_end(1413) NÃO presentes no push buffer ativo. Tradutor produz 0 DrawCommand.

6. **SetPreallocatedBuffer**: slots 0 e 1 (binder transação 14), endereço 0xE5D39000, 328 MB cada.

7. **IocCreate/IocAlloc/MapBufferEx**: todos com sucesso.

8. **Sem faults**: fault=0, RECUSA=0.

### O que falta (bloqueios atuais)

**M1 — Guest progride: ALCANÇADO.** O guest não está mais deadlockado. Faz 1015+ SVCs, com ciclos de wait/signal/unlock funcionando corretamente (ex. cv=0x10a5096e4).

**M2 — GPU recebe trabalho de desenho: BLOQUEADO.** O guest submete apenas 1 push buffer (8864 bytes, 624 métodos, apenas métodos de estado — sem draw_begin/draw_end). Não submete push buffers subsequentes com comandos de desenho. O guest está girando em código nativo (pc=0xffffaaa01d6c, todas as threads no mesmo PC) sem fazer mais chamadas a SubmitGPFIFO após 300s de execução. Não é possível determinar sem análise do código ARM64 do guest por que ele não submete mais push buffers. Possíveis causas: esperando GPU completação, DequeueBuffer, vsync, ou carregamento de recursos.

**M3 — GPU conclui trabalho: BLOQUEADO.** `RasterizerG610::Draw()` apenas incrementa contadores (`++contadores_.draws`), não executa trabalho de GPU real. Não há backend Vulkan/PanVK conectado.

**M4 — Caminho de display: PARCIAL.** OpenLayer chamado (VI serviço), Connect (10), Query (9), SetPreallocatedBuffer (14, slots 0 e 1) feitos. Mas DequeueBuffer (3) e QueueBuffer (7) nunca são chamados. O guest nunca chegou a criar um buffer para desenhar.

**M5 — Imagem real: BLOQUEADO.** Consequência dos bloqueios acima.

### Comandos executados

- Build no Orange Pi: `cmake --build build -j$(nproc)`
- Execução (120s): `timeout 120 ./build/g610_boot /home/emuos/piDroid/pokemon/exefs/` com `XDG_RUNTIME_DIR=/run/emuos WAYLAND_DISPLAY=wayland-1 G610_TRAP_LIMIT=0 IA_STOP_SAMPLES=0 IA_THREAD_TRACE=1`
- Execução (300s): mesmo comando com `timeout 300`
- Logs salvos na board: `/home/emuos/eden_fix*.log`, copiados para `/mnt/Dados/piDroid/`

### Alterações de código nesta sessão

1. **src/svc_dispatcher.cpp**: REMOVIDA a chamada a `DespertarUmWaiterDeCadaFilaDeMutexECvs()` em `svc_signal_process_wide_key` (linhas ~3347-3348). A função mantém sua definição mas não é mais chamada.

2. **src/services/nvdrv.cpp**: 
   - Movido `std::uint64_t rs_endereco = 0;` de dentro do bloco `{` (linha 2228) para escopo da função (antes da linha 2217) para que fique acessível após o syncpoint avançar.
   - Adicionada chamada a `SvcDispatcher::AcordarFutexNoEndereco(rs_endereco)` após o syncpoint avançar (linhas ~2689-2696), quando `rs_endereco != 0`.

### INVESTIGAÇÃO CONCLUÍDA — PC 0xffffaaa01d6c (25/10/2026)

#### O que é 0xffffaaa01d6c

**NÃO é código do guest.** É um endereço HOST, no offset `0x1d6c` de uma base ASLR-randomizada (`0xffffa0090000` ou `0xffffaaa00000` nos runs medidos). Fora do intervalo do guest `[0x60000000, 0x310000000)` (~1.5 GB a ~20 GB).

É o PC de uma thread de guest que está **dormindo dentro de `g_arbiter.wake.wait()`**, chamado a partir de `svc_wait_process_wide_key_atomic` (handler do SVC `WaitProcessWideKeyAtomic`, 0x1C).

**Estado dos registos no momento do bloqueio** (eden_fix_new.log):
- `pc=0xffffaaa01d6c`, `x30=0xffffaaa01d4c` (=pc-0x20), `lr1=0xffffaaa049f0` — mesma biblioteca host `0xffffaaa0xxxx`
- `lr2=0xaaaae405f878`, `lr3=0xaaaae4061ea4`, `lr4=0xaaaae406db44` — frames de outra biblioteca host `0xaaaae4xxxxxx`
- `x0=0xaaaae42a9e4c` — argumento na mesma segunda biblioteca
- `sp=x29=0x30fffbac0` — stack válida do guest (handler de sinal usa stack do guest)
- `x1=x19=0x189`, `x2=0x0`

Todas as amostras (`[sample 1]`..`[sample N]`) mostram os **mesmos** valores — a thread está completamente parada.

#### Causa-raiz do bloqueio

**SVC #423** é o último serviço chamado:
```
WaitProcessWideKeyAtomic(x0=0x10a509bf8 [mutex], x1=0x10a509c04 [cv_key],
                         x2=0x81ff [tag], x3=0xffffffffffffffff [timeout infinito])
```
- Thread regista-se em `cv_fila[0x10a509c04]` e dorme em `g_arbiter.wake`
- **CV `0x10a509c04` NUNCA é sinalizada** em `eden_fix_new.log`
- Sinais vão para CV `0x8437e20c` (6×, `acordados=0`, `pendente` acumula) e CV `0x10a5096e4` (1×)
- Nenhum `SignalProcessWideKey(0x10a509c04, ...)` existe no log
- Thread bloqueia para sempre → zero SVCs após #423 → nenhum SubmitGPFIFO #2

**GPU conclusão está noutro endereço:** `rs_endereco = 0xE2D3F000` (confirmado no log: `report semaphore escrito em 0xE2D3F000 = 1`). `AcordarFutexNoEndereco(0xE2D3F000)` acorda waiters em `cv_fila[0xE2D3F000]`, não em `cv_fila[0x10a509c04]`. São objetos de sincronização distintos e não conectados.

**No `eden_fix_cv_signal.log`** a CV `0x10a509c04` É sinalizada no SVC #411 (`SignalProcessWideKey(0x10a509c04, -1)`, antes da espera em #423). O sinal pendente é consumido e o guest avança. Mas eventualmente trava noutras CVs; a amostra de PC continua a ser `0xffffaaa01d6c` (uma thread bloqueada, outras fazem progresso).

### CORREÇÃO IMPLEMENTADA — CV 0x10a509c04 sinalizada após conclusão da GPU (25/10/2026)

#### Causa-raiz identificada

**`RegistarTrabalhoGpu()` (que chama `ProcessPendingIpcPendente()`) só era chamado do handler `FenceAction` do canal GPU** (nvdrv.cpp:2332). Mas o push buffer do SubmitGPFIFO #0 **não contém método `FenceAction`** (IDs 28/29 não aparecem nos 624 métodos). Portanto, `ProcessPendingIpcPendente()` NUNCA era chamado após este submit.

O syncpoint ainda avança (nvdrv.cpp:2683: `syncpoints_.Incrementar()`) e `AcordarFutexNoEndereco(rs_endereco)` é chamado (linha 2696), mas isso só acorda waiters em `cv_fila[0xE2D3F000]`, não em `cv_fila[0x10a509c04]`.

CV `0x10a509c04` é o mecanismo que o guest usa para sincronização após conclusão da GPU. Sem `ProcessPendingIpcPendente()` após o submit, os IPCs pendentes não são processados e CV `0x10a509c04` nunca é sinalizada.

#### Correção

**src/services/nvdrv.cpp**: Adicionada chamada a `SvcDispatcher::RegistarTrabalhoGpu()` após `AcordarFutexNoEndereco(rs_endereco)` (linha ~2696), dentro do bloco `if (canal->fence_id >= 0)`, depois do syncpoint avançar. Isto garante que IPCs pendentes sejam processados após cada conclusão de submissão GPU, independentemente da presença de `FenceAction` no push buffer.

#### Resultados (Orange Pi, 300s execução)

- **CV 0x10a509c04 É sinalizada** no SVC #409 (`SignalProcessWideKey(0x10a509c04, -1)`, x30=0x805e2eb4)
- **Thread acorda**: `[thrd] ACORDEI token=0x1 cv=0x10a509c04 acordei=0`
- **CV 0x10a5096e4 é sinalizada repetidamente** com `acordados=1` (ex. SVC #1019, #1026, #1034, #1505)
- **Nova CV aparece**: 0x10a531fd4 (mutex 0x10a531fc8) sinalizada em SVC #1509
- **SVCs: 423 → 1512** (antes: travado no #423; depois: continua até #1512 em 300s)
- **SubmitGPFIFO #0 continua a ser o único submit** — SubmitGPFIFO #2 ainda não apareceu após 300s
- **Nenhum DequeueBuffer/QueueBuffer** ainda

#### Estado atual

O bloqueio na CV 0x10a509c04 foi resolvido. O guest avança através de ciclos de CV/mutex (0x10a509c04 → 0x10a5096e4 → 0x10a531fd4 → ...) mas ainda não chegou a SubmitGPFIFO #2.

#### Análise do novo bloqueio (após correção da CV 0x10a509c04)

Após o SVC #409 sinalizar CV 0x10a509c04, a thread render (handle 0x20003, tag=0x20003) acorda, faz `ArbitrateUnlock` no mutex 0x10a509bf8, e volta a esperar na CV 0x10a5096e4. A partir daí entra num ciclo infinito:

```
Wait(CV 0x10a5096e4) → Signal → ArbitrateUnlock → Wait(CV secundária) → Signal → IPC → ArbitrateUnlock → back to Wait(CV 0x10a5096e4)
```

Entre cada par de sinais, a thread apenas faz `ArbitrateUnlock` e chamadas IPC (SendSyncRequest para handles 0xe81e7 e 0xf81e5). Nunca chama `DequeueBuffer`, `SubmitGPFIFO #2`, `CtrlEventWait` ou qualquer operação de GPU/display.

**Evidências confirmadas (run de 120s, /tmp/eden_repro_120.log):**

- CV 0x10a5096e4 é sinalizada 204 vezes com `acordados=1`
- Todas as CVs secundárias que a thread espera são eventualmente sinalizadas (não há CV "esquecida")
- `CtrlEventRegister`/`CtrlEventWait` (SVC 0x18) NUNCA chamado
- `WaitSynchronization` (SVC 0x18) NUNCA chamado
- `DequeueBuffer` (binder transação 3) NUNCA chamado
- `QueueBuffer` (binder transação 7) NUNCA chamado
- `GetNativeHandle` (binder transação 2, IHOSBinderDriver) É chamado → cria `g_evento_da_fila` (KEvent)
- `SinalizarEventoDaFila()` sinaliza `g_evento_da_fila` após `SetPreallocatedBuffer` (slots 0 e 1)
- Mas a thread render espera em CVs, não no `KEvent` `g_evento_da_fila`
- Threads 0x8000 (CV 0x13f20febc) e 0x10001 (CV 0x13f31040c) NUNCA são sinalizadas

**Causa provável:** a thread render está num ciclo de espera por CVs mas nunca prossegue para `DequeueBuffer` + `SubmitGPFIFO #2`. Pode estar à espera de um evento de display/vsync que nunca chega (não há mecanismo de vsync no emulador), ou pode haver uma condição no código nativo do guest (entre SVCs) que nunca se torna verdadeira.

#### Próximos passos

1. Determinar se falta um mecanismo de vsync/evento de display que deveria despertar a thread render para chamar `DequeueBuffer` + `SubmitGPFIFO #2`
2. Verificar se threads 0x8000/0x10001 (nunca sinalizadas) são responsáveis por submeter push buffers ou chamar `DequeueBuffer`
3. Investigar o código nativo do guest entre SVCs para entender que condição a thread render verifica antes de prosseguir
4. Se identificar implementação em falta, fazer correção mínima e testar no Orange Pi

### Git

- Branch: `wip/passo-4-thread`
- Local commit: `fe5b26e` (à frente da board `0272e45`)
- Arquivos alterados: `src/svc_dispatcher.cpp`, `src/services/nvdrv.cpp`, `src/gpu/draw_consumer.cpp`, `include/g610/gpu/draw_consumer.hpp`, `CMakeLists.txt`, `include/g610/svc_dispatcher.hpp`
- Arquivos novos não rastreados: `src/gpu/rasterizer_g610.cpp`, `include/g610/gpu/rasterizer_g610.hpp`, `vendor/` (vários)

---

## NOVA SESSÃO — estado confirmado (25/10/2026)

### Sincronização entre máquinas (NÃO estão sincronizadas)

- Local (`/mnt/Dados/piDroid/EdenMaliG610`): branch `wip/passo-4-thread`, commit `fe5b26e`, com alterações não commitadas (destaque: `src/svc_dispatcher.cpp` +1270/-429 linhas, `src/services/nvdrv.cpp`, `src/services/vi.cpp`, `src/eden_bridge.cpp`, `src/native_engine.cpp`, `CMakeLists.txt`, docs).
- Orange Pi (`/home/emuos/EdenMaliG610`): commit `0272e45`, com alterações não commitadas (incluindo deleções da árvore `include/ia/` e `src/ia_*`). Não há processo Wayland em execução no momento da verificação, mas sway está rodando (socket `/run/emuos/wayland-1`).

### Build do código local no Orange Pi

- Copiei a árvore-fonte local para `/home/emuos/EdenMaliG610_src/` (árvore separada, sem tocar na working tree do Orange Pi).
- Sincronizei `vendor/third_party/SPIRV-Tools` (incluindo `build_module.h`, ausente na cópia) a partir da árvore do Orange Pi.
- `cmake --build build -j$(nproc) --target g610_boot` **compilou com sucesso** para AArch64 nativo no Orange Pi.

### Reprodução do bloqueio atual

- O binário local construído no Orange Pi **não chega ao spin loop em `0x805e7060`**.
- Todas as threads de guest ficam **estacionadas no arbiter** (`g_arbiter.wake.wait()`, dentro de `svc_wait_process_wide_key_atomic`) ou em `__clock_nanosleep`. Nenhuma thread executa código de guest.
- `attach_eden(1,1)` **retornou sucesso** ("IPC do Eden : ligado"), logo `eden_` não é nulo e `RegistarTrabalhoGpu()`/`ProcessPendingIpcPendente()` é executável.
- O guest faz ~339 SVCs e 1 `SubmitGPFIFO` (push buffer de 624 métodos, sem draw_begin/draw_end, 0 DrawCommand) e depois trava: todas as threads estacionam numa CV que nunca é sinalizada.
- Conclusão: o bloqueio atual desta build é o **deadlock no arbiter (CV nunca sinalizada)**, anterior ao spin loop de `0x805e7060`. O spin loop do guest só seria observável após desestacionar a thread render.

### Análise estática do spin loop em `0x805e7060` (disassembly de `/tmp/main_seg0_decompressed.bin`)

- A função contém o loop a partir de `0x805e6ee0`; `x0` (argumento) = `x20` = base da estrutura (`callee-saved`, estável dentro da função).
- `x19 = x20 + 0x360` (sub-objeto; passado para stub `0x817e93c0` no prólogo e `0x817e93d0` no epílogo).
- Loop (`0x805e703c`–`0x805e706c`):
  - `ldr x8,[x20,#0x330]; ldr x9,[x20,#0x338]; cmp; b.ne 0x805e6f18` — se campos de fence/sync diferem, sai do loop.
  - `ldarb w8,[x20,#0x2f8]; tbnz w8,#0,0x805e701c` — se bit 0 de `x20+0x2f8` estiver set, sai do loop.
  - `bl 0x817e9470(x0=x20+0x380, x1=x19)` — stub resolvido via GOT (base `0x82494000`); alvo não resolvido estaticamente.
  - `ldr x8,[x20,#0x300]; ldr x9,[x20,#0x308]; cmp; b.eq 0x805e703c` — se `x20+0x300 == x20+0x308` **permanece no spin**; se diferem, sai para `0x805e6f18` (caminho de renderização).
- Sem chamada direta `bl` à entrada `0x805e6ee0` (chamada indireta/virtual via `blr x8`).
- `x20+0x388` é cabeça de lista encadeada (itens a cada 0x30, next em `+0x0`) percorrida internamente pela função.
- Não há instruções `str [reg, #0x308]` diretas (o offset é materializado em registrador separado, ex. `add x22,x20,#0x308`), portanto o escritor legítimo de `x20+0x308` não é identificável por varredura estática simples; requer watchpoint ao vivo em `0x805e7060` (inalcançável enquanto o arbiter estiver deadlockado).

### Bloqueio atual e próximos passos

- Blocker imediato: deadlock no arbiter (render thread estacionada numa CV nunca sinalizada). Não se pode observar `x20`, `x20+0x300/0x308` nem definir watchpoint em `x20+0x308` sem desestacionar a thread render.
- Hipótese (não comprovada): `x20+0x300` (consumidor) vs `x20+0x308` (produtor) é um par de contadores de fila/frame; `x20+0x308` deveria ser atualizado pelo mecanismo que enfileira o próximo quadro/buffer (display/VI `QueueBuffer` ou caminho de apresentação), não de forma arbitrária.
- Não foi possível confirmar `SubmitGPFIFO #2`, `DequeueBuffer`, `QueueBuffer` nem pixels reais nesta sessão.

## Investigação do bloqueio atual (sessão autônoma) — resultado definitivo

### Estado dos repositórios
- Local (`/mnt/Dados/piDroid/EdenMaliG610`): branch `wip/passo-4-thread`, commit `fe5b26e`, alterações não commitadas em `src/svc_dispatcher.cpp` (+1270/-429) e `src/services/nvdrv.cpp`. O fix `RegistarTrabalhoGpu()` + `AcordarFutexNoEndereco(rs_endereco)` está nas **alterações não commitadas** de nvdrv.cpp (não em nenhum commit).
- Orange Pi (`/home/emuos/EdenMaliG610`): branch `wip/passo-4-thread`, commit `0272e45`, alterações não commitadas. `nvdrv.cpp` e `svc_dispatcher.cpp` contêm o mesmo fix (`RegistarTrabalhoGpu` na linha 2703, `AcordarFutexNoEndereco` na 2696). Nenhum processo g610 em execução.
- Os históricos divergem (sem ancestral comum direto entre 0272e45 e fe5b26e nos ramos inspecionados).

### Binário executado na reprodução
- `/home/emuos/EdenMaliG610/build/g610_boot` (ELF aarch64, Release, construído nativamente no Orange Pi a partir da árvore local do Orange Pi com o fix aplicado).
- Inicialização: `XDG_RUNTIME_DIR=/run/emuos WAYLAND_DISPLAY=wayland-1 G610_TRAP_LIMIT=0 IA_STOP_SAMPLES=0 IA_THREAD_TRACE=1 G610_SVC_SIGNAL_TRACE=1 IA_IPC_TRACE=1 timeout 45 ./build/g610_boot /home/emuos/piDroid/pokemon/exefs/` (timeout 124s, log 68 MB com sinalização, sem travar o processo antes do timeout).

### Sequência de espera/sinalização registada (eden_repro_signal.log)
- 1 único `SubmitGPFIFO #0` (push buffer de 624 métodos, sem draw_begin/draw_end). Não há `SubmitGPFIFO #2`.
- Thread render (`tag=0x20003`, `token=0xaaaab2ecb200`) estaciona repetidamente em `cv=0x10a5096e4`, `mutex=0x10a5096d8`, `timeout=-1`.
- `cv=0x10a5096e4` é sinalizada pelo **código guest** (submitter) em `x30=0x805e6eb8` (fim da função `0x805e6970`), `count=1`, ~37 vezes.
- TODOS os 185 sinais têm `acordados=0`: o waiter é movido para a fila do mutex (`fila[addr]`) e acordado apenas quando o dono do mutex chama `ArbitrateUnlock` — não há lost-wakeup no arbiter.
- `cv=0x10a509c04` (deadlock da sessão anterior) **agora é sinalizada 1 vez** (fix `RegistarTrabalhoGpu` funciona); thread `token=0x1` acorda.
- Threads `0x8000` (cv=0x13f20febc), `0x10001` (cv=0x13f31040c), `0x18002` (cv=0xe2028194) estacionadas em CVs que recebem sinais em endereços DIFERENTES (0x13f20fe0c, 0x13f310098/0x13f31035c, 0xe20280e4) — não despertam nesta fase.
- No final da execução, apenas 1 thread faz progresso (svc=197); as demais permanecem em `g_arbiter.wake.wait()`.

### Verificação da implementação do arbiter (src/svc_dispatcher.cpp)
- `WaitProcessWideKeyAtomic` regista `desperta[token]=Aguarda` e `cv_fila[cv_key]` sob o gate, escreve `HasWaiterFlag` e `next_value` no guest SOBRE o gate, e só então bloqueia em `g_arbiter.wake.wait`. Sem janela de lost-wakeup contra `SignalProcessWideKey` (ambos seguram o gate).
- `SignalProcessWideKey`: se o mutex está livre (`prev==0`), CAS(0→tag) acorda o dono (`DespertaSucesso`); se o mutex está ocupado, move o waiter para a fila do mutex sem acordar (acordado posteriormente pelo `ArbitrateUnlock` do dono). Sem perda de despertar.
- `g_cv_pendente` trata sinal-antes-de-espera (sinal fica registado e consumido pelo wait seguinte). Sempre acedido sob o gate.
- `AcordarFutexNoEndereco` acorda um waiter de `cv_fila[addr]` e um de `arb_fila[addr]`, com `notify_all` só se acordou alguém.
- **Conclusão: não há defecto de sincronização (lost-wakeup) no arbiter.** O waiter pode ficar bloqueado apenas se o dono do mutex nunca o liberar — o que não acontece aqui (a render thread desperta e re-estaciona ciclicamente).

### Diferença causal em relação à execução anterior (0x805e7060)
- O loop guest em `0x805e7060` provém de **análise estática** do `main_elf` (disassembly de `/tmp/main_seg0_decompressed.bin`); **não foi observado dinamicamente** em nenhum log disponível (nenhuma amostra mostra PC guest em `0x805e7060`; as threads amostradas estão sempre no arbiter host ou em código do módulo sdk/subsdk).
- Portanto não é possível estabelecer uma divergência causal entre duas execuções dinâmicas: a execução anterior que alegadamente alcançou `0x805e7060` não tem log correspondente disponível.

### Hipótese (demonstrada pelos logs)
- A render thread **não está deadlockada permanentemente**: desperta em `cv=0x10a5096e4` (sinalizada pelo submitter guest), faz `ArbitrateUnlock` + `SendSyncRequest` a handles VI (`0xe81e7`, `0xf81e5`), e re-estaciona em `cv=0x10a5096e4`. Nunca chama `DequeueBuffer`, `QueueBuffer` nem `SubmitGPFIFO #2`.
- A razão pela qual não avança para `SubmitGPFIFO #2` nem para `0x805e7060`: o caminho de display está incompleto (M4: `DequeueBuffer` e `QueueBuffer` nunca chamados); o produtor legítimo de `x20+0x308` (contador de quadro/buffer, ver análise estática do spin loop) nunca é atualizado; o evento de vsync (`ThreadDoVsync`) sinaliza um `KEvent` host (`g_evento_vsync`), **não** a CV guest `cv=0x10a5096e4`, pelo menos o guest não o observa nessa CV.
- Não há operação de sincronização host ausente que se possa corrigir minimalmente: o arbiter funciona, `cv=0x10a5096e4` é sinalizada, a render thread desperta. O bloqueio é de funcionalidade em falta (caminho de display/vsync não ligado à CV guest), não de semáforo perdido.

### Conclusão
- Não há correção mínima de sincronização a aplicar (não se deve introduzir wake-all, `Success` artificial, fences inventadas ou avanço forçado de threads).
- O trabalho seguinte requer implementar o caminho de display (`DequeueBuffer`/`QueueBuffer`, ligação do vsync à CV guest, produtor de `x20+0x308`) para que a render thread saia do ciclo em `cv=0x10a5096e4`, atinja `0x805e7060`, emita `SubmitGPFIFO #2` e, por fim, produza pixels.
- Evidência: `eden_repro_current.log` e `eden_repro_signal.log` (cópia em `/mnt/Dados/piDroid/`).

## Implementação e teste da correção de vsync → CV guest

### Correção aplicada
Em `src/services/vi.cpp`, `ThreadDoVsync` passa a chamar `SvcDispatcher::AcordarFutexNoEndereco(0x10a5096d8)` após sinalizar `g_evento_vsync`. O endereço `0x10a5096d8` é o mutex da CV `cv=0x10a5096e4` (o waiter é movido para a fila do mutex quando o mutex está ocupado, por isso o wake deve ser no endereço do mutex, não na CV). Incluído `#include "g610/svc_dispatcher.hpp"` em `vi.cpp`.

### Resultado do teste no Orange Pi
- Build bem-sucedido (`make g610_boot`).
- Execução de 30s com `eden_fix_vsync.log` (cópia em `/mnt/Dados/piDroid/`).
- `cv=0x10a5096e4` sinalizada 419 vezes (vs 174 antes) — o vsync agora acorda a render thread.
- `SubmitGPFIFO #0`: 1 submissão (inalterado).
- `DequeueBuffer` (binder txn 3): **0 chamadas** (inalterado).
- `QueueBuffer` (binder txn 7): **0 chamadas** (inalterado).
- `SubmitGPFIFO #2`: não observado.

### Análise do resultado
A sinalização de `cv=0x10a5096e4` pelo vsync acorda a render thread, mas ela **não chama `DequeueBuffer`**. Após despertar, a thread render (PC `0x83abc1e8`, sdk/subsdk) executa leituras de armazenamento (handle `0xe81e7`) e consultas VI (handle `0xf81e5`), e re-estaciona em `cv=0x10a5096e4`. O código guest no caminho pós-despertar não inclui `DequeueBuffer`.

### Conclusão da correção de vsync
A conexão vsync → CV guest é necessária mas **não suficiente**. A render thread precisa também encontrar trabalho na fila (`[x20+0x300] != [x20+0x308]`), o que requer que `[x20+0x308]` (produtor) seja avançado. Sem isso, a render thread desperta, não encontra trabalho, e re-estaciona.

### Próximo bloqueio concreto
Encontrar o endereço guest `x20` (base do contexto de render) para que o host possa avançar `[x20+0x308]` quando o vsync disparar. Alternativamente, determinar se o código guest em `0x83abc1e8` (sdk/subsdk) contém um caminho para `DequeueBuffer` que não está sendo alcançado por falta de alguma condição (ex. resposta de query VI incorreta).

## Investigação aprofundada do fluxo guest (sessão autônoma) — desassembly do `main`

### Método
- O binário `exefs_pokemon/main` é um NSO0 comprimido (LZ4). Segmento de texto descomprimido para `/tmp/main_text.bin` (25.084.672 bytes).
- Base de carregamento do guest: `0x80000000` (texto começa na location 0 do NSO).
- Desassembly com `capstone` (Python) de `main_text.bin` a VA `0x80000000`.

### Função do render thread (guest) — `0x805e6ee0` a `0x805e6ed4`
Prologue: `x20 = x0` (base do contexto de render), `x19 = x0 + 0x360`. Fluxo:
1. `bl 0x817e93c0(x0=x19)` — stub de setup (GOT[0xba8]).
2. `cmp [x20+0x300], [x20+0x308]`; se iguais → salta para o spin loop em `0x805e7034`.
3. Senão: verifica flag `[x20+0x2f8]` bit 0; se set → `0x805e701c` (epílogo, retorna sem trabalho).
4. Senão: verifica `[x20+0x300]` vs `[x20+0x308]`; se iguais → `0x805e7074` (processamento de fences); se diferentes → `0x805e6f30` (percorre lista ligada de trabalho em `[x20+0x380]`, chama método virtual offset 0x10 de cada item).

### Spin loop — `0x805e703c` a `0x805e7070` (confirmação da análise estática em `0x805e7060`)
```
0x805e703c: ldr  x8, [x20, #0x330]   ; fence A
0x805e7040: ldr  x9, [x20, #0x338]   ; fence B
0x805e7044: cmp  x8, x9
0x805e7048: b.ne 0x805e6f18           ; fences diferentes → sai do spin
0x805e704c: ldrb w8, [x22]            ; x22 = &x20+0x2f8 (flag)
0x805e7050: tbnz w8, #0, 0x805e6f18   ; flag bit0 set → sai do spin
0x805e7054: mov  x0, x21               ; x0 = x20+0x380 (lista de trabalho)
0x805e7058: mov  x1, x19               ; x1 = x20+0x360
0x805e705c: bl   0x817e9470            ; STUB WAIT (GOT[0xc00]) — espera na CV
0x805e7060: ldr  x8, [x20, #0x300]   ; consumidor
0x805e7064: ldr  x9, [x20, #0x308]   ; produtor
0x805e7068: cmp  x8, x9
0x805e706c: b.eq 0x805e703c           ; iguais → volta ao spin (espera novamente)
0x805e7070: b   0x805e6f18            ; diferentes → sai para trabalho
```

### Stub resolvidos via GOT (base `0x82494000`)
- `0x817e9470` (GOT[0xc00]): **função de espera**. Chamada no spin loop com `x0=x20+0x380, x1=x20+0x360`. É a que bloqueia na CV `cv=0x10a5096e4`. Nunca aparece como `x30` em `SignalProcessWideKey` para `cv=0x10a5096e4` (não sinaliza, apenas espera).
- `0x817e97b0` (GOT[0xda0]): **função de sinalização**. Chamada em `0x805e6eb4` com `x0=x20+0x380` → sinaliza `cv=0x10a5096e4` (x30=0x805e6eb8, confere com os 174 sinais no log). Também chamada em `0x805e7138` e outros locais, mas com x0 diferente → sinaliza CVs diferentes (não aparecem no log de `cv=0x10a5096e4`).
- `0x817e93c0` (GOT[0xba8]), `0x817e93d0` (GOT[0xbb0]): stubs de setup/callback.

### Completion handler — `0x805d6d70` (chamado por 6 locais)
```
0x805d6d80: bl   0x805d1080            ; stlrb [x0+0x2f8] = 1 (flag)
0x805d6d84: add  x8, x19, #0x37c       ; estado em [x0+0x37c]
0x805d6d88: ldar w9, [x8]
0x805d6d8c: cmp  w9, #2
0x805d6d90: b.eq 0x805d6dac            ; estado já é 2 → retorna (one-time)
...
0x805d6dc4: bl   0x817e93c0(x0=x19+0x330)
0x805d6dcc: bl   0x805d73e0            ; → stub 0x817e9660
0x805d6dd0: add  x0, x19, #0x350
0x805d6dd4: bl   0x817e97b0            ; sinaliza CV associada a x19+0x350
```
- Transição one-time: estado `[x0+0x37c]` de 0 para 2 (CAS).
- Sinaliza a CV associada a `x0+0x350` (não necessariamente `cv=0x10a5096e4`; depende de x0).
- Define flag `[x0+0x2f8] = 1`.

### Render thread após despertar de `cv=0x10a5096e4`
No log, após `ACORDEI cv=0x10a5096e4`:
1. Faz `SendSyncRequest` a handle `0xe81e7` (IStorage/leitura de ficheiros — resposta "SFCO"/"SFCI" não confirmada como VI).
2. Faz `SendSyncRequest` a handle `0xf81e5` (VI session, comandos com x24=0x8/0x20/0x28 — queries de display).
3. Re-estaciona em `cv=0x10a5096e4`.
**Nunca chama `DequeueBuffer` (binder txn 3) nem `QueueBuffer` (binder txn 7).**

### Escrita a `x20+0x308`
- Nenhuma instrução `str [x20/x19, #0x308]` imediata encontrada em todo o segmento de texto (25 MB).
- Escrita possível via offset de registrador (`add x22,x20,#0x308; str x?, [x22]`) encontrada em `0x805e6ff8`, mas `x20` é sobreescrito antes (`ldr x20, [x8,#0x300]` em `0x805e6f74`) — trata-se de percurso de lista ligada, não de incremento de contador produtor.
- **Conclusão: `x20+0x308` (produtor) nunca é avançado por código guest encontrado.**

### VI / vsync
- `ThreadDoVsync` arranca automaticamente em `OpenLayer` (60 Hz), sinaliza `g_evento_vsync` (`KEvent` host).
- `GetDisplayVsyncEvent` (comando VI 5202) **nunca é chamado pelo guest** (não aparece nos logs).
- O guest nunca recebe handle do vsync; a thread render espera em `cv=0x10a5096e4` (arbiter host), **não** em `g_evento_vsync`.

### Diferença causal identificada (primeira divergência provada)
Após `SubmitGPFIFO #0` (concluído, syncpoint avança para 1, sem draw commands):
1. O guest conecta a `vi:m` e `vi:s` (GetService).
2. A thread render desperta em `cv=0x10a5096e4` (sinalizada pelo submitter guest), faz queries VI/storage, e re-estaciona.
3. `[x20+0x308]` nunca é avançado → `[x20+0x300] == [x20+0x308]` → spin loop nunca encontra trabalho → `DequeueBuffer` nunca chamado → `SubmitGPFIFO #2` nunca emitido.
4. `g_evento_vsync` é sinalizado pelo vsync host mas **não está conectado** a `cv=0x10a5096e4` nem a `x20+0x308`.

### Conclusão da investigação do fluxo guest
O bloqueio não é de sincronização (arbiter funciona, CV é sinalizada, render thread desperta) nem de perdido-wakeup. É de **funcionalidade em falta**: o produtor de `x20+0x308` (que deveria ser o caminho de display/vsync a enfileirar o primeiro quadro) nunca é acionado, porque `g_evento_vsync` não está ligado à CV guest nem ao contador produtor. A render thread fica no ciclo `espera CV → sem trabalho → re-espera` indefinidamente.

### Próximo bloqueio concreto
Implementar a ligação do vsync ao caminho de apresentação do guest de forma que:
- O vsync dispare o primeiro `DequeueBuffer` (ou avance `x20+0x308` de forma correta),
- Sem escrever arbitrariamente em `x20+0x308` nem conectar `g_evento_vsync` à CV guest sem prova do contrato guest,
- Sem sinais artificiais nem `Success` para ocultar falhas.

Requer determinar o endereço guest de `x20` (base do contexto de render) ou encontrar um mecanismo host que avance `x20+0x308` sem conhecer `x20` diretamente.
