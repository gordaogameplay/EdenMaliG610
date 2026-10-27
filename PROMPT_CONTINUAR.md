# PROMPT DE CONTINUAÇÃO — Pokémon Shield em EdenMaliG610 até "Jogo na tela" (C8)

Documento de handoff. Tudo o que já foi medido está abaixo e **não precisa ser re-medido**; os próximos passos começam onde a investigação parou. Projeto: `/home/emuos/EdenMaliG610` (espelho local `/mnt/Dados/piDroid/EdenMaliG610/`). Alvo: `ssh emuos@192.168.2.51` (sem password). Jogo: `/home/emuos/piDroid/pokemon/exefs/`.

## 0. Comandos canónicos (não alterar)

- Build: `cd /home/emuos/EdenMaliG610 && cmake --build build -j$(nproc)` — **nunca** `make g610_boot`.
- Run: `./build/g610_boot /home/emuos/piDroid/pokemon/exefs/` com `XDG_RUNTIME_DIR=/run/emuos WAYLAND_DISPLAY=wayland-1` (ICD panfrost em `/usr/share/vulkan/icd.d/panfrost_icd.json`).
- Fluxo de código: editar no espelho local, `scp` para a board (cpp **e** hpp se o header mudou), rebuild. Sem git destrutivo, sem commits automáticos.
- Só conta C8 ("JOGO NA TELA" — pixels reais no ecrã físico). Sem fixes artificiais (sem forçar sucesso / fences falsos). Reportar só sucesso final ou blocker provado.

## 1. Objetivo e estado

Rodar Pokémon Shield no Orange Pi 5 Pro (RK3588S / Mali-G610) até pixels reais no ecrã físico. Estado actual: **freeze (deadlock) diagnosticado e contornado no braço cego; baseline cego estável até svc #933**. C8 ainda não atingido. O braço CAS (tentativa de fix "correcto") revelou-se pior que o baseline e foi **revertido por omissão** (opt-in via `IA_CAS=1`).

## 2. Hipóteses mortas (não re-abrir)

- **Gate-leak**: `[gaten]` nunca disparou em nenhuma run (timed_mutex nunca bloqueou >1 s apesar de tráfego pesado). Morta.
- **Dispatch-stall**: `dispatch_take` (native_engine.cpp) dorme 100 µs com teto de 5 s e depois faulta; runL não mostrou linhas de fault. Morta.

## 3. Mecanismo do freeze (provado — runL + runM #1206/#1207)

- As escritas de palavras do mutex/cv do lado do kernel eram **escritas cegas** (`write_guest`), correndo em paralelo com os loops LL/SC nativos do guest → **lost updates** → estado fantasma `0x400081ff` (dono|waiters sem dono real) → worker estaciona em `svc_arbitrate_lock` para sempre no svc #912 enquanto a main dorme na cv C2 — **deadlock**.
- ABI: `svcArbitrateLock(x0=owner_tag, x1=addr, x2=self_tag)`; estaciona iff `word == x0|0x40000000`.
- runL: o PARK da main apareceu **depois** das linhas #911/#912 (interleaving tardio) — a escrita de liberação (0) da main correu contra o LL/SC do guest.
- runM verificou o ciclo saudável L2 de ponta a ponta: park→signal-mexe→unlock-pop→wake nas duas direções; transições de word `0→0x20003→0x40020003→0x81ff`.

## 4. Braço CAS (tentado e revertido)

- Builtins CAS verificados na board (`/tmp/cas_test.cpp` → `CAS_TOOLCHAIN_OK`); precedente em `svc_wait_for_address` tipo==1.
- Aplicado: `cas_word_guest` (via `mem_->host_pointer` + `__atomic_compare_exchange_n`, SEQ_CST) e `escrita_atomica_word` (16 tentativas CAS com re-leitura fresca, fallback cego para liveness). Entrega do sinal reestruturada num único ciclo CAS cobrindo livre (CAS(0→tag), acorda já dono) e ocupado (CAS(observado→observado|0x40000000), push na fila), com re-leitura fresca em perda — nunca do `prev` velho.
- Sítios convertidos: unlock (0x1b), park cv-word=1, park next_value, signal (0x1d), 0x35 (SignalToAddress decrement), gpu-wake (AcordarFutexNoEndereco). Escrita pós-drenagem do cv_key deixada cega (palavra só de writers kernel).
- **Resultado (runCAS0-5): ESTANQUE DETERMINÍSTICO** — toda a run para no svc #402-407 com o mutex `0x10a5096d8` órfão `0x00020000` (depois `0x40020000` depois de a main CASar a flag de waiters). Zero ACORDEI/DLOCK após o cume. O rasto `[mwr]` por SVC mostrou a word a ir `0x00020003→0` (a nossa liberação CAS) e **depois o próprio guest escrever `0x00020000`** (um valor de tag nunca visto em runs saudáveis) — uma corrida do tipo lost-update que o braço CAS amplificou em vez de prevenir. Testemunho: runBLIND (`IA_BLIND=1`, restaurando a semântica plain `write_guest`) chegou a svc #933 com ACORDEI=41 DLOCK=41 e a família sã de palavras (`0x0/0x00020003/0x000081ff/0x400081ff/0x40020003`).

## 5. Estado actual da árvore (revert)

- `cas_word_guest` e `escrita_atomica_word` **defaultam agora para a semântica cega** (o braço `IA_BLIND`); o caminho CAS fica atrás de `IA_CAS=1` para experiência futura. A run por omissão = cego = baseline saudável.
- Assinaturas e call sites inalterados; o header continua a declarar os dois helpers.
- Isto **não é um fix artificial**: repõe a configuração empiricamente mais estável e mantém o braço CAS opt-in.

## 6. Instrumentação presente (toda gated por env, custo zero quando desligada)

- `IA_THREAD_TRACE=1`: LPAUSA (token/addr/tag/word/fila_endereco/cvs_apontando), LACORDEI, ACORDEI, DLOCK, PARK, SIGNAL (cap 64/%1000).
- `IA_IPC_TRACE=1`: linhas `[arb]` unlock/signal, `[ipc]`/`[ipcR]`.
- `IA_WORD_TRACE=1`: `[word] CAS/BLIND/FALLBACK` por chamada + `[mwr] d8/e0/e4` por SVC (em `tests/boot_trace.cpp`).
- `IA_CAS=1`: reactiva a experiência CAS.
- `IA_BLIND=1`: cego explícito (agora redundante — é o default).
- `IA_WATCH=rtld G610_WATCH_ADDR=...`: arma uma página em só-leitura.
- `quadro_entrar` + gate `timed_mutex`: imprime `[gaten]` se alguma espera no gate passar de 1 s.
- `caca_freeze.sh` / `forense.sh` na board (`/tmp/`) e localmente.

## 7. Factos medidos (não re-medir)

- `[gaten]` nunca disparou em **nenhuma** run (gate nunca bloqueou >1 s) — hipótese gate-leak morta.
- Ritmo saudável (sem IPC trace): ~35 svc/s; IPC trace custa ~6×.
- Primeiro ACORDEI aparece sempre no svc #403-409 (cume onde começa a fase de cv-wait).
- runM (240 s, IPC trace): LPAUSA=2 LACORDEI=2 ACORDEI=125 DLOCK=174 gaten=0; reads vagueiam em `0x22F8xxxx`.
- runM1-3 (200 s): LPAUSA 2-4, LACORDEI = LPAUSA-1 (shutdown benigno), ACORDEI 53-93, DLOCK 53-93, gaten=0 — todas limpas (fim=1 parou=5).
- runN1 (700 s): limpa, svc #1282, reads 77, offset `0x22F81A450`, gaten=0.
- runP1-4 (4×300 s paralelo): todas limpas.
- runL (730 s): **o** freeze — svc #912, worker estacionado em `0x400081ff` órfão, main dorme em C2; último svc #912; gaten=0. Frequência ~1 em 13 runs.
- Braço CAS: estanque determinístico no svc #402-407, 0 ACORDEI/DLOCK após o cume, órfão `0x00020000`.
- Braço cego: svc #933, ACORDEI=41 DLOCK=41 em 200 s — saudável.
- Ferramentas: CAS toolchain verificado (`/tmp/cas_test.cpp` → `CAS_TOOLCHAIN_OK`).
- `host_pointer` = mapeamento identidade (endereço guest == host) — coerência confirmada; não é problema de mapeamento.
- Handles estancam em 104 (~2 min); GPFIFO submits=1; ritmo ~3,6 itens/s depois do estanque; 18 chaves `pendente` stale (`0x8437e20c`, `0x8437b55c`, `0x8434ed4c`, …).
- Tags: main=0x81ff; por-lane cv constantes (0x20003, 0x10001, 0x8000, 0x18002, 0x28004); `kArbWaitMask=0x40000000`.
- Mapeamento de traps: #26=ArbitrateLock(0x1a), #27=ArbitrateUnlock(0x1b), #28=WaitProcessWideKeyAtomic(0x1c), #29=SignalProcessWideKey(0x1d), #33=SendSyncRequest(0x21).
- Semântica de evidência: linha `[svc #N]` + sem trace adicional ⇒ thread congelou DENTRO desse handler; end-stats `traps=svc+1` ⇒ a última trap nunca retornou.
- Identidade: `dispatch_owner_itself()` (thread_local `t_dispatch_in_flight`); `kOwnerMain=1`; `read_thread_pointer()` nunca casa com estado do guest.
- Escrita pós-drenagem do cv_key (~linha 3276) deixada cega intencionalmente.

## 8. Onde continuar (próximos passos)

1. **Caça de estabilidade do baseline cego**: `/tmp/caca_freeze.sh 4 700` (ou sequencial) — o freeze original **não** deve reproduzir. Objectivo: confirmar que o ~1/13 do freeze desapareceu no baseline cego (deve — é o código pré-CAS exacto; se ainda ocorrer, a causa do freeze é outra e re-abre-se o diagnóstico).
2. Se estável: **não** remover os prints de diagnóstico (gated por env, custo zero quando desligados) — são o kit forense da próxima parede. Manter também o gate `timed_mutex`.
3. **Métricas de progressão**: handles crescem além de 104, ≥2 submits GPFIFO, reads avançam além de `0x22F8xxxx`. Se o baseline cego ainda estanca em 104 handles / 1 submit, o freeze não era a única parede — re-abrir as 18 chaves `pendente` stale e o estanque de handles.
4. **Caminho C8** (só depois de freeze+estanque resolvidos): CtrlEventRegister/CtrlEventWait, DequeueBuffer(3)/QueueBuffer(7), apresentação binder (superfície Wayland existe; SetPreallocatedBuffer trans=14 já atingido). Compositor em `src/services/binder.cpp`.
5. Re-avaliar as 18 chaves `pendente` stale uma vez o freeze fixo e o estanque de handles re-verificado.
6. **Experiência CAS (opcional, mais tarde)**: reactivar com `IA_CAS=1` só depois de capturar por que o guest escreve `0x00020000` — precisa de uma run de watch que sobreviva até à janela (corrigir primeiro a morte precoce, ou capturar por outra sonda: dump por SVC do struct CriticalSection completo à volta de d8 em `tests/boot_trace.cpp`, gated por intervalo de svc).

## 9. Ficheiros relevantes

- `src/svc_dispatcher.cpp` — helpers CAS + todos os sítios do arbiter + diagnósticos.
- `include/g610/svc_dispatcher.hpp` — declarações `cas_word_guest`/`escrita_atomica_word`.
- `tests/boot_trace.cpp` — print `[svc #N]` + dump `[mwr]` por SVC.
- `src/native_engine.cpp` — `dispatch_take/release/vacate/claim/set/itself`.
- `src/services/nvdrv.cpp` — callers do gpu-wake.
- `src/services/binder.cpp` — compositor para C8.
- `caca_freeze.sh`, `forense.sh` — launcher de caça + extractor forense (também em `/tmp/` na board).
- `PROGRESSO.md` — paredes anteriores.

## 10. Armadilhas / convenções

- Não fazer polling de hunts em background — notificam ao terminar.
- `cv_key` pós-drenagem zero-write (~linha 3276) deixada cega intencionalmente.
- `read_thread_pointer()` nunca casa com estado do guest; usar `dispatch_owner_itself()`.
- `dispatch_owner_claim()` tem teto de 5 s e depois faulta — sem linhas de fault nas runs saudáveis.
- A run saudável pode ter `LPAUSA = LACORDEI + 1` (o caminho de shutdown/paragem retorna antes do print LACORDEI) — benigno.
- `IA_IPC_TRACE` custa ~6× o ritmo — não usar em hunts longos a não ser que se queira forense.
- Sincronizar sempre os **dois** ficheiros quando o header mudar.

---
*Última actualização: revert do braço CAS por omissão; baseline cego estável até svc #933; próximo passo imediato = caça de estabilidade 4×700 s.*
