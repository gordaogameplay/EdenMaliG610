# EdenMaliG610 — Prompt de Continuação (agente seguidor)

## Contexto do projeto
- Máquina: Orange Pi 5 Pro (RK3588S, Mali-G610, aarch64, Ubuntu 24.04.5 LTS, kernel 6.18.38)
- Usuário: emuos
- Projeto: /home/emuos/EdenMaliG610
- Branch: wip/passo-4-thread
- HEAD (commitado): 42dd64d "docs: audit repository completeness and correct project status"
- HEAD remoto (origin/wip/passo-4-thread): 42dd64d (sincronizado)
- Remoto: git@github.com:gordaogameplay/EdenMaliG610.git
- Jogo: /home/emuos/piDroid/pokemon/exefs/ (NSO, não ELF) — main + rtld + sdk + subsdk0 + subsdk1

## OBJETIVO FINAL
Obter pixels reais de Pokémon Shield na tela física da Orange Pi 5 Pro, através do caminho:
Pokémon Shield ARM64 nativo → SVC/IPC → vi:m/vi:s/vi:u → OpenLayer → nvdrv → GPFIFO/pushbuffer → Maxwell → Vulkan → Mali-G610 → framebuffer → tela física
**Não declarar sucesso sem pixels reais do jogo na tela.**

## ESTADO ATUAL (28/10/2026)

### O que PASSOU: experimento "BOOT NATURAL" (SC1–SC5 ✓)

O guest arranca **pela cadeia fria do próprio rtld do jogo** e chega ao SDK sem
nenhum forçamento de PC (zero [patch]/[illeg]/SIGILL/nop-fórcio). Critérios:

- **SC1** entrada do rtld 0x84461000 com x0=0; primeira svc = QueryMemory
- **SC2** as 15 relocações RELATIVE do rtld aplicadas com os valores esperados
- **SC3** PLT do rtld resolvido para o SDK real (+0x3170=0x83a23c50 nn::init::Start)
- **SC4** main[0x24B5CA0]=0x8246a8b0 relocalizado pelo próprio rtld (nenhum motor escreveu .data)
- **SC5** zero forçamentos de PC na corrida completa

Relatório completo: `g610-telemetry/rlogs/exp_boot_natural_20261028.md`.
Histórico detalhado: `PROGRESSO.md` (secção 28/10).

### Fronteira atual: ArbitrateLock (svc #0x1A)

O SDK fica em spin de mutex em `0x83abc780` chamando svc 0x1A com
`x0=0x1 x1=0x843ea4d0 x2=0x81ff` (pós-run palavra=0x00000001). O disassembly do
SDK provou o protocolo userspace: CAS 0→own tag; em contenção set bit30
(0x40000000) e a chamada ao svc com **x0 = tag do dono**, x1=endereço, x2=prória
tag; após acordar espera ver A SUA tag na palavra. Assinatura verificada em libnx
+ SwitchBrew: `ArbitrateLock(W0=Handle<Thread>, X1=addr, W2=tag)`,
`ArbitrateUnlock(X0=addr)`. A leitura de argumentos no dispatcher está CORRETA; o
caminho de park existe (Eden-faithful) e o unlock já faz a transferência correta
(escreve a tag do acordado). O que falta saber com evidência: a palavra que o
engine lê em cada chamada (bit30 masca?) — determina a correção. Próximo passo:
run diagnóstico com `IA_WORD_TRACE=1 IA_THREAD_TRACE=1` e decidir de evidência.

### Working tree (alterações NÃO commitadas — nenhuma por commit ainda)

| Arquivo | Alteração | Estado |
|---------|-----------|--------|
| src/guest_program.cpp | sem header NSO em memória; entry_point devolve rtld primeiro (cadeia fria) | aplicado, não commitado |
| src/g610_call_guest.S | x0=xzr no trampolim (era kTlsBase 0x88000000) | aplicado, não commitado |
| src/svc_dispatcher.cpp | svc_return_from_exception sem forçar PC; descanso limpos para boot natural | aplicado, não commitado |
| src/native_engine.cpp | flag natural_boot() (G610_ARTIFACTS_BOOT=1 restaura modo antigo); gates sobre relocs heurísticas/NOPs/band-aids; sampler SIGALRM | aplicado, não commitado |
| tests/boot_trace.cpp | escrutínio pós-boot (PLT/GOT/main globais) + --dumpaddr | aplicado, não commitado |
| src/nso_loader.cpp | dynstr/dynsym offsets relativos ao módulo (sessões anteriores) | aplicado, não commitado |
| CMakeLists.txt / docs/RELATORIO_GHIDRA_REA_PIXELS.md / include/g610/svc_dispatcher.hpp / src/services/vi.cpp | suporte das mudanças acima | aplicado, não commitado |

Não-rastreados mantidos por decisão do usuário: `0_total=0`, `build_asan/`,
`g610-telemetry/rlogs/`, `*.bak`, `src/svc_dispatcher.cpp.trunc-runO`,
`rtld_natural/`, `PROMPT_CONTINUACAO.md` (este arquivo).

### Paredes secundárias
- **SIGSEGV rc=139 no teardown**: acontece em TODOS os runs (incluindo pré-experimento e naive). Problema separado, para depois.
- **Sampler SIGALRM não dispara durante a fase guest** (<0.2 s); o escrutínio pós-boot cobre a observação.

## PRÓXIMOS PASSOS (em ordem)

### 1. Build (se ainda não existir)
```bash
cmake --build build -j$(nproc) --target g610_boot
```

### 2. Run de referência (boot natural)
```bash
./build/g610_boot /home/emuos/piDroid/pokemon/exefs
```
Esperado: parede no ArbitrateLock (svc 0x1A x0=0x1 x1=0x843ea4d0), terminação
por trace 4 s descrita como "limite de traps atingido (4000)" (label antigo —
na verdade é o force-stop de 4 s).

### 3. Investigação ArbitrateLock — COM EVIDÊNCIA
```bash
IA_WORD_TRACE=1 IA_THREAD_TRACE=1 timeout 8 ./build/g610_boot /home/emuos/piDroid/pokemon/exefs 2>&1 | tee /tmp/opencode/arb_dignostico2.log
```
Ler: no ramo de park (`[thrd] LPAUSA`), do `word=` impresso. Depois:
- Se word==0x40000001 e chegou LPAUSA: paragem funciona; o wake nunca vem (dono
  0x1 não existe) → implementar por evidência a cadeia de wake (não hardcode).
- Se palavra visível é 1 (sem bit30) nos dois pontos: ver se o CAS do SDK
  realmente concretiza bit30 (re-run IV_WORD_TRACE? a vizinhança de memória
  ajuda) e por que o engine nunca entra no ramo park indicate condição.
Referências: PROGRESSO.md secção 28/10; sdk_text_now.bin (janela 0x83abc5c0-0x83abc900).

### 4. Depois do ArbitrateLock
É a porta para Spawn/threads SDK — novos SVCs (CreateThread etc.). Depois o
caminho histórico: vi:m → OpenLayer → Dequeue/QueueBuffer → SubmitGPFIFO #2 →
maxwell_translator → Vulkan/Mali-G610 → pixels. Checkpoints do histórico do
projeto (vi:GetDisplayService real, OpenLayer, vsync, DequeueBuffer/QueueBuffer,
SubmitGPFIFO #2) continuam as caixas de passagens seguintes.

## COMANDOS ÚTEIS
```bash
# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Rodar o modo artefacto antigo (para comparação)
G610_ARTIFACTS_BOOT=1 IA_SVC_TRACE=1 ./build/g610_boot /home/emuos/piDroid/pokemon/exefs/

# Diagnóstico ArbitrareLock
IA_WORD_TRACE=1 IA_THREAD_TRACE=1 IA_SVC_TRACE=1 timeout 8 ./build/g610_boot /home/emuos/piDroid/pokemon/exefs/ 2>&1 | tee /tmp/opencode/arb_diag.log

# Disassembly do SDK (descomprimir .text com python3+lz4; base 0x8378d000)
#   texto: /tmp/opencode/sdk_text_now.bin = NSO sdk .text descomprimido
objdump -D -b binary -m aarch64 --adjust-vma=0x8378d000 /tmp/opencode/sdk_text_now.bin --start-address=0x83abc770 --stop-address=0x83abc900

# Git
git status --short
git diff --stat
git log --oneline -5
```

## ARQUIVOS PRINCIPAIS
- src/native_engine.cpp — motor de execução nativa, signal handlers, natural_boot()
- src/svc_dispatcher.cpp — implementação de Implementação dos SVCs (ArbitrateLock l.2164, unlock l.2340)
- src/g610_call_guest.S — trampolim de entrada guest (ABI: x0=0)
- src/guest_program.cpp — carga módulos, cadeia fria rtld→main
- src/nso_loader.cpp — carregador NSO (LZ4, segmentação com gaps virtuais)
- tests/boot_trace.cpp — escrutínio pós-boot, --dumpaddr
- vendor/eden/ — referência de kernel (k_condition_variable.cpp, svc_lock.cpp)
- src/gpu/maxwell_translator.cpp — tradução Maxwell (incompleto)
- src/services/vi.cpp, src/services/nvdrv.cpp — serviços de vídeo/GPU

## ALERTA: artefactos STALE
- `/tmp/opencode/rtld_text.dis` e `/tmp/opencode/rea_rtld/` vêm de dumps ANTIGAS
  (era artefacto). Endereços como 0x26e8/0x26fc NÃO batem com a imagem atual.
  Verdade atual (rtld): svc/BRK em 0x16e8(#6)/0x16fc(#0x26)/0x1704(#0x27)/
  0x170c(#0x28); MOD0 probe ~0x2018; imagem natural do rtld em
  `/tmp/opencode/rtld_now.bin`. Para endereços novos usar `--dumpaddr`.

## REGRAS
- NÃO fazer commit/push sem autorização
- NÃO usar git reset --hard ou git clean
- NÃO apagar .bak, build_asan/, ou arquivos não rastreados
- NÃO alterar kernel/drivers GPU
- NÃO declarar sucesso sem pixels reais do jogo na tela
- NÃO confiar em testes sintéticos (triângulo vermelho) como prova de renderização
- Trabalhar apenas no Orange Pi 5 Pro

## CHECKPOINTS OBRIGATÓRIOS
- CHECKPOINT: IPC REAL — quando o guest fizer a primeira chamada IPC real (vi:m:GetDisplayService)
- CHECKPOINT FINAL: JOGO NA TELA — quando houver pixels reais de Pokémon Shield na tela física

## O QUE NÃO FAZER
- Não adicionar NOPs aleatórios para "fazer o guest avançar"
- Não retornar Success para SVCs desconhecidos sem entender o que fazem
- Não pular o RTLD sem entender por que ele está preso
- Não confiar que "o processo está vivo" significa que o jogo está rodando corretamente
- Não adicionar stubs artificiais: decisões devem vir de evidência em logs/disassembly
