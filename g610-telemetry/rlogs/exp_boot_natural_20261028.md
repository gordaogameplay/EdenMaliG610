# EXPERIMENTO "BOOT NATURAL" — 28/10/2026
## Jogo: Pokémon Shield (exefs real) · alvo: pixels reais via vi/nvdrv/GPU

## Mudanças aplicadas (1 patch coerente, sem commit)
1. `src/guest_program.cpp`: (a) **sem** header NSO em memória (causa raiz do
   laço eterno em `FUN_84461440` — probe MOD0 quebra); (b) `entry_point()`
   devolve o **rtld primeiro** (cadeia fria), fallback main sem +0x100.
2. `src/g610_call_guest.S`: **x0 = xzr** (era `movz x0,#0x8800, lsl #16` —
   kTlsBase). Evidência: corrida 1 — `cmp x0,#0` do entry falha → ramo
   quente → fail-arm `CallSupervisor(0x28, 0xf801)` + spin `b .`@0x844610f8,
   svc_calls=1 (cadeia fria nunca arrancou).
3. `src/svc_dispatcher.cpp`: `svc_return_from_exception` sem forçar PC
   (0x84461100 removido; mantém ret=0).
4. `src/native_engine.cpp`: flag `natural_boot()` (env `G610_ARTIFACTS_BOOT=1`
   restaura o modo antigo); gates sobre relocações heurísticas + NOPs do crt0;
   band-aids gated off (SIGSEGV stub-GOT/skip low-addr/skip espaço guest;
   SIGILL auto-nop + pc+4; SIGALRM spin-advance + patch `b #0`; o force pós-
   ReturnFromException; o early-return pc==0x844610fc). Sampler no SIGALRM
   (só observação: deltas dos slots PLT/GOT do rtld + main 0x24B5CA0).
   SITES DE SAÍDA MANTIDOS (não são forcing de boot): trap-limit/kBrkForceStop/
   kBrkGuestReturned/paragem de threads/ExitProcess → `recovery_pc`.
5. `tests/boot_trace.cpp`: escrutínio pós-boot (slots PLT/GOT lidos de memória)
   + `--dumpaddr <vaddr>,<bytes>` (instrumento de diagnóstico).

## Corrida de referência: boot_natural_run3.log (run1/run2 anteriores)
- [boot] natural=1 · entry 0x84461000 com x0=0; sp=0x310000000; tpidr ok.
- svc_calls=45, traps=47(without host faults), faults=0, **zero** linhas
  [patch]/[illeg]/SIGILL/host-fault/**0x84461100** (SC5 ✓).
- Service mix: 25× QueryMemory (varrimento completo por módulo, 0x0→0x310001000)
  · GetInfo(0xb,0xc,0xd,0xe,0xf,0x2,0x3) · GetThreadPriority/GetThreadId
  · **SendSyncRequest (x0=0x101fe, canal sm:)** · 5× ArbitrateLock spin.

## SC (critérios)
- **SC1 ✓** entrada do rtld com x0=0 — cadeia fria executou (não há fail-arm;
  nenhum trap 0x28 no arranque; a primeira svc é QueryMemory).
- **SC2 ✓** 15 RELATIVE do rtld aplicados **com os valores esperados**:
  +0x3180=0x844642b0(+0x32b0) · +0x3188=0x84461710(+0x710) ·
  +0x3190=0x844642a0(+0x32a0) · +0x3198=0x844642a8(+0x32a8) ·
  +0x31a0=0x84462624(+0x1624) · +0x31a8=0x84465000(+0x4000) ·
  +0x31b8=0x84464008(+0x3008=&dynamic) · +0x3218/22/260/268=0x84464280(+0x3280)
  · +0x3270=0x8446376c(+0x276c) · +0x3278=0x8446393c(+0x293c).
  PLT: +0x3170 = **0x83a23c50** (nn::init::Start RESOLVIDA — sdk);
  +0x3178 = 0x844629c0 (lazy/default). nós da lista: +0x3280/88=self 0x84464280.
- **SC3 ✓** o fluxo passou o processamento de PLT da rtld e ENTOU no SDK:
  GetInfo chain @0x83b0bbc0 (svc table BRK 0x29), SendSyncRequest #43, e
  chegou ao caminho do mutex @0x83abc780 (TAG 0x81ff — thread principal).
- **SC4 ✓** main[0x24B5CA0] = **0x8246a8b0** (= 0x80000000+0x246a8b0; imagem=0).
  Reolocalizado NATURALMENTE pelo rtld — nenhuma escrita de motor n ela
  (o patch_all não toca no .data).
- **SC5 ✓** zero forcamentos de PC na corrida 3: as mesma flags limps no log
  ([patch], [illeg], SIGILL, [host-fault], 0x84461100): 0 ocorrências.

## Paredes remanescentes (não são SC do boot natural)
1. **ArbitrateLock spin** (svc #0x1a) — SDK mutex @0x843ea4d0 (sdk data):
   o guest CAS-loops; svc chamado com **x0=1**, x1=0x843ea4d0 (addr),
   x2=0x81ff (tag); x4=0x30ffffeb8. A implementação actual lê
   (handle=x0, addr=x1, tag=x2) e no fast-path (word != handle|0x40000000)
   retorna Success imediato — o SDK re-park sem sucesso — limit 4000/4s.
   → Próximo passo decidido: fixar a semântica real de `svc 0x1a` no sdk
   21+:DumpEval: `_ZN2nn2os6detail...` — a chamada feita em 0x83abc7dc
   (bl 0x83b0bb2c → svc 0x1a). NOTA: sem esta mudança NÃO há aviso novo de
   thread nenhuma (rlog: 0 [thrd]).
2. **SIGSEGV rc=139 no teardown** — acontece **também** nos runs antigos;
   não é introduzido pelo boot natural (run1/2/3 e naive idêntico).
3. Sampler SIGALRM não fuela em fase guest curta (<0.2s) — deltas disponíveis
   só se a fase guest durar mais que um tick; o escrutínio pós-boot cobre.

## Riscos de cada tecla mudada (auditoria)
- `G610_ARTIFACTS_BOOT=1` restaura EXACTAMENTE o comportamento antigo (gates).
- `espaço guest` skip/`:0x84461100`oinfinito etc. — TODOS sob `!natural_boot()`.
- IaaS: IA_TRACE/IA_IPC_TRACE/IA_WORD_TRACE/IA_STOP_SAMPLES intactos.

## Como reproduzir
- `cmake --build build -j8 --target g610_boot`
- `./build/g610_boot /home/emuos/piDroid/pokemon/exefs`
  (opcional: `--dumpaddr 0x83abc7a0,96` · `--raw naive.bin`)
- Os logs: /tmp/opencode/boot_natural_run{1,2,3}.log, boot_dump_probe.log,
  mutex_word.log, wrapper.log, rtld_now.bin (--raw 0x4k da imagem natural).
