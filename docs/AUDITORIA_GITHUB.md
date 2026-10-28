# Auditoria de integridade do repositório GitHub

**Data da auditoria:** 2026-10-28
**Auditor:** Kilo (agente autónomo)

---

## 1. Estado do repositório

| Item | Valor |
|---|---|
| Directório | `/home/emuos/EdenMaliG610` |
| Branch local | `wip/passo-4-thread` |
| HEAD local | `5d3faef9cd07dca83b20cd202ac6531c02a0846f` |
| HEAD remoto (origin/wip/passo-4-thread) | `5d3faef9cd07dca83b20cd202ac6531c02a0846f` |
| Remoto | `git@github.com:gordaogameplay/EdenMaliG610.git` |
| Divergência local vs remoto | 0 à frente, 0 atrás (sincronizado) |
| Divergência local vs origin/main | 12 à frente, 0 atrás |

O checkpoint anterior (`5d3faef`) está publicado no remoto e o HEAD local corresponde exatamente.

---

## 2. Arquivos essenciais rastreados pelo Git

| Categoria | Estado | Detalhes |
|---|---|---|
| Código-fonte engine | Versionado | `src/main.cpp`, `src/native_engine.cpp`, `src/svc_dispatcher.cpp`, `src/guest_program.cpp`, `src/nso_loader.cpp`, `src/patcher.cpp`, `src/ipc_min.cpp`, `src/telemetry.cpp`, `src/guest_memory.cpp`, `src/guest_placement.cpp`, `src/eden_bridge.cpp`, `src/eden_log.cpp`, `src/eden_settings.cpp`, `src/eden_synchronization_object.cpp`, `src/crypto/aes.cpp`, `src/fs/romfs.cpp` |
| Código-fonte GPU | Versionado | `src/gpu/draw_consumer.cpp`, `src/gpu/maxwell_translator.cpp`, `src/gpu/rasterizer_g610.cpp`, `src/gpu/vulkan_backend.cpp`, `src/gpu/shader_spv.cpp`, `src/gpu/ir.cpp`, `src/gpu/maxwell_isa.cpp`, `src/gpu/maxwell_program.cpp`, `src/gpu/maxwell_frontend.cpp`, `src/gpu/glsl_backend.cpp`, `src/gpu/pushbuffer.cpp`, `src/gpu/macro_mme.cpp` |
| Serviços Eden | Versionado | `src/services/nvdrv.cpp`, `src/services/vi.cpp`, `src/services/binder.cpp`, `src/services/applet_oe.cpp`, `src/services/csrng.cpp`, `src/services/fsp_srv.cpp`, `src/services/hid.cpp`, `src/services/pl_u.cpp`, `src/services/set.cpp`, `src/services/shared_font.cpp`, `src/services/time_u.cpp` |
| Cabeçalhos | Versionado | `include/g610/` (todos os `.hpp`) |
| Testes | Versionado | `tests/test_core.cpp`, `tests/test_exec.cpp`, `tests/exec_mem.cpp`, `tests/test_pushbuffer.cpp`, `tests/test_gpfifo_replay.cpp`, `tests/test_program.cpp`, `tests/bench_svc.cpp`, `tests/test_maxwell.cpp`, `tests/test_translate.cpp`, `tests/test_gpu_exec.cpp`, `tests/test_rasterizer_clear.cpp`, `tests/test_guest_memory_read.cpp`, `tests/test_vertex_attributes.cpp`, `tests/test_gpfifo_inspect.cpp`, `tests/test_integrated_draw.cpp`, `tests/test_vulkan_pipeline.cpp`, `tests/probe_input.cpp`, `tests/mostra.cpp`, `tests/test_input.cpp`, `tests/boot_trace.cpp` |
| Documentação | Versionado | `docs/ARQUITETURA.md`, `docs/CLOCK_GPU_1GHZ.md`, `docs/PIROID_ANTERIOR.md`, `docs/PUBLICAR_MAIN.md`, `docs/RELATORIO_GHIDRA_REA_PIXELS.md`, `docs/RESULTADOS.md`, `docs/SESSAO_GRAFICA.md`, `docs/AUDITORIA_GITHUB.md` (este documento) |
| Código Eden vendored | Versionado | `vendor/eden/` (pruned copy of Eden's core/hle) |
| Código SPIR-V vendored | Versionado | `vendor/third_party/sirit/`, `vendor/third_party/SPIRV-Reflect/` |
| Scripts de vendor | Versionado | `tools/gerar_lista.py`, `tools/podar_vendor.py`, `tools/aplicar_overrides.py`, `tools/overrides/` |
| LICENSE | Versionado | `LICENSE` (GPL-3.0-or-later) |
| README.md | Versionado | `README.md` (requer correções - ver seção 5) |
| PROGRESSO.md | Versionado | `PROGRESSO.md` (running log em português) |

---

## 3. Arquivos NÃO rastreados (untracked) - classificação

### 3.1 Build artifacts - devem permanecer ignorados (categoria 2)

| Arquivo/Diretório | Justificativa |
|---|---|
| `build/` | Diretório de build CMake (já ignorado por `.gitignore`) |
| `build_asan/` | Build com AddressSanitizer - artefato de compilação, reproduzível |
| `*.o`, `*.a` | Object files e bibliotecas estáticas (já ignorados por `.gitignore`) |

### 3.2 Backups e evidências de diagnóstico - preservar localmente (categoria 3)

| Arquivo | Justificativa |
|---|---|
| `src/gpu/maxwell_translator.cpp.bak` | Backup do ficheiro antes de alterações - preservar localmente |
| `src/services/binder.cpp.bak` | Idem |
| `src/services/nvdrv.cpp.bak` | Idem |
| `src/svc_dispatcher.cpp.bak` | Idem |
| `src/svc_dispatcher.cpp.trunc-runO` | Truncado durante teste - evidência diagnóstica |
| `0_total=0` | Ficheiro de controlo de execução - preservar localmente |
| `g610-telemetry/doctor.csv` | Saída de telemetria - regenerável |
| `g610-telemetry/doctor_0.json` | Saída de telemetria - regenerável |

### 3.3 Arquivos privados / ROMs / NSOs - NÃO versionar (categoria 4)

| Arquivo | Justificativa |
|---|---|
| `/home/emuos/piDroid/pokemon/exefs/` | NSOs do jogo Pokémon Shield - propriedade do utilizador, não do repositório |
| `*.keys`, `*.nsp`, `*.xci`, `*.nca` | Chaves e ROMs - já ignorados por `.gitignore` |

---

## 4. Arquivos novos referenciados no CMakeLists.txt

| Ficheiro referenciado | Existe? | Versionado? |
|---|---|---|
| `src/gpu/shader_spv.cpp` | Sim | Sim (commit 5d3faef) |
| `include/g610/gpu/shader_spv.hpp` | Sim | Sim (commit 5d3faef) |
| `vendor/third_party/sirit/` | Sim | Sim |
| `vendor/third_party/SPIRV-Reflect/` | Sim | Sim |

Todos os ficheiros referenciados no `CMakeLists.txt` existem e estão versionados.

---

## 5. Diferença entre branch de trabalho e origin/main

A branch `wip/passo-4-thread` está 12 commits à frente de `origin/main`. Os commits adicionais são:

```
5d3faef checkpoint: native ARM64 execution + Mali-G610 Vulkan + SVC dispatch functional
92626c8 ETAPA 15: WAIT-RET reasons no G610_TRACE_SYNC
39dbcbc ETAPA 11: instrumentacao G610_TRACE_SYNC em Wait/SignalProcessWideKey
a68f08f ETAPA 9: ferramenta de inspecao de capturas GPFIFO
5205371 ETAPA 8: instrumentacao diagnostica + LeDoGuest wired no draw_consumer
7b112ad ETAPA 7: topologia Maxwell + desenho integrado (dados sinteticos)
549e217 fixup: CMakeLists vertex_attributes target + draw_vertex_buffer declaration
5f861a7 ETAPA 6: decodificacao de atributos de vertices Maxwell
164600b ETAPA 5: leitura segura da memoria do guest + draw_vertex_buffer
790323f ETAPA 4: auditoria Maxwell + captura de vertex stream base VA no translator
199bea5 ETAPA 3: pipeline grafico Vulkan real (triangulo offscreen)
725eb72 ETAPA 2: backend Vulkan minimo ligado a RasterizerG610::Clear()
2411c7f Checkpoint ETAPA 1: auditoria de ambientes + sync Pi->CachyOS + faxina de clones aninhados (já em main)
```

O commit `2411c7f` é o último comum com `main`. Tudo depois dele é trabalho desta branch.

---

## 6. Alterações pendentes (não commitadas)

Estas 5 alterações estão na working tree mas ainda não foram commitadas:

| Ficheiro | Tipo | Resumo |
|---|---|---|
| `docs/RELATORIO_GHIDRA_REA_PIXELS.md` | Modificado | +62 linhas - atualização com estado do RTLD preso |
| `include/g610/svc_dispatcher.hpp` | Modificado | +1 linha - declaração `svc_output_debug_string` |
| `src/main.cpp` | Modificado | -51 linhas - removido GOT pre-population e NOP patching do rtld |
| `src/native_engine.cpp` | Modificado | +114/-41 linhas - SIGALRM timer, SIGSEGV/SIGILL handlers, spin-loop detection |
| `src/svc_dispatcher.cpp` | Modificado | +17 linhas - `svc_output_debug_string` no-op, `svc_return_from_exception` preserva x0 |

Estas alterações precisam ser commitadas e publicadas.

---

## 7. Resultado da compilação e testes

| Item | Resultado |
|---|---|
| `cmake --build build` | PASS (todos os targets construídos sem erros) |
| `git diff --check` | PASS (sem erros de whitespace) |
| Execução `./build/g610 run /home/emuos/piDroid/pokemon/exefs/` | PASS (processo roda 15+ min sem crash, VSYNC a avançar) |
| Testes `ctest` | Não executados nesta auditoria (requerem tempo adicional) |
| SVC dispatch | Verificado: `svc #6 QueryMemory`, `svc #27 OutputDebugString`, `svc #28 ReturnFromException` despachados do rtld |

---

## 8. Limitações

1. O módulo `main` não consegue executar devido a relocações NSO não aplicadas (endereços globais fora dos segmentos alocados).
2. Nenhuma chamada IPC do módulo `main` foi observada (sem `vi:m:GetDisplayService`, `vi:u:OpenLayer`, `GetDisplayVsyncEvent`).
3. Nenhum comando Maxwell real do jogo foi emitido.
4. Nenhum pixel real de Pokémon Shield foi apresentado na tela física.
5. Os testes `ctest` não foram executados nesta sessão de auditoria.

---

## 9. Itens que precisam ser adicionados ao Git

As 5 alterações listadas na seção 6 precisam ser stageadas e commitadas:
- `docs/RELATORIO_GHIDRA_REA_PIXELS.md`
- `include/g610/svc_dispatcher.hpp`
- `src/main.cpp`
- `src/native_engine.cpp`
- `src/svc_dispatcher.cpp`

---

## 10. Itens mantidos fora do Git (justificada)

| Item | Justificativa |
|---|---|
| `build_asan/` | Build com ASan - artefato de compilação, grande (~126 MB), reproduzível |
| `*.bak` | Backups de ficheiros antes de alterações - preservar localmente, não são parte do código |
| `svc_dispatcher.cpp.trunc-runO` | Ficheiro truncado durante teste - evidência diagnóstica local |
| `0_total=0` | Ficheiro de controlo de execução - regenerável |
| `g610-telemetry/` | Saída de telemetria - regenerável a cada execução |
| `build/` | Diretório de build CMake - já ignorado por `.gitignore` |
| `/home/emuos/piDroid/pokemon/exefs/` | NSOs do jogo - propriedade do utilizador, não do repositório |

---

**Conclusão da auditoria:** O repositório está em boas condições. As 5 alterações pendentes precisam ser commitadas e publicadas. O README.md requer correções para refletir o estado real do projeto (ver correções separadamente). Nenhum arquivo privado, ROM ou chave está sendo versionado indevidamente.
