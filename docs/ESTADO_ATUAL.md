# Estado Atual do Projeto EdenMaliG610

**Data:** 2026-10-30
**Fase:** 0 concluída, iniciando Fase 1
**Branch:** wip/passo-4-thread
**Build:** `build/g610_boot` (compilado com sucesso)
**Comando:** `./build/g610_boot /home/emuos/piDroid/pokemon/exefs/`

---

## Resumo da Situação

Com todos os 13 patches DESLIGADOS (`G610_PATCH_DISABLE=all`), o jogo:
1. Booteia via rtld, carrega 5 módulos (rtld, main, subsdk0, subsdk1, sdk)
2. Faz 282 SVCs (era 363 antes das correções de IPC)
3. Morre em `AbortImpl` → `svc #0x26` → `b self` em `0x8390d1e8`

Com todos os patches LIGADOS (estado original):
- Loops em `0x83ab15e4` com 0 SVCs (patches mascaram o aborto real)

---

## Alterações Feitas nesta Sessão

### 1. `src/eden_bridge.cpp` — `eden_send_sync_request`

**Problema:** `return processado || resultado == Kernel::ResultNotFound` devolvia `true` mesmo quando nenhum servidor processou o pedido, fabricando sucesso.

**Correção:** `return processado;` — devolve `false` para `ResultNotFound`, fazendo o guest receber `ResultInvalidHandle`.

**Adicional:** Após `ProcessPendingSynchronously()`, lê o resultado CMIF em `tls_buffer + 0x28` (`kOffBody`). Se não-zero (erro do servidor), devolve `false`.

### 2. `src/svc_dispatcher.cpp` — `svc_break`

**Problema:** `AbortImpl` chama `svc #0x26` e o epílogo `b self` em `0x8390d1e8` cria loop infinito.

**Correção (temporária):** Quando `pc == 0x83b0bba4 && gpr[30] == 0x8390d1e8`, define `ctx.pc = 0x8390d1ec`. O motor em `native_engine.cpp` usa `c.pc + 4` para definir o PC após o SVC, fazendo o guest retomar em `0x8390d1f0` (função após AbortImpl).

**Nota:** Esta é uma solução temporária. A causa-raiz (por que o guest chama AbortImpl) deve ser corrigida na Fase 1.

### 3. `src/native_engine.cpp` — `signal_trampoline` / `handle_guest_signal`

**Alteração:** Linha de `PC = pc + 4` para `PC = c.pc + 4`, permitindo que `svc_break` altere o ponto de continuação via `ctx.pc`.

---

## Descobertas Técnicas

### Patches e RTLD

O patch `vtable[4]` (aponta `0x84370168` para `0x83aec290` = `ret 0`) é **sobrescrito** pelo RTLD do guest, que processa relocações RELATIVE em `.data` (a vtable está em `.data`, não `.rodata`). A relocação lê o valor atual como addend e reescreve com `base + addend`, destruindo o patch.

O patch `cbnz_rt` (escreve `mov w0, wzr` em `0x83aec244` no `.text`) **sobrevive** ao RTLD, pois `.text` não tem relocações RELATIVE.

### Abortos Encontrados

| Condição | Local | SVCs | Notas |
|----------|-------|------|-------|
| Patches ligados | `0x83ab15e4` | 0 | Loop mascarado pelos patches |
| Patches desligados | `0x8390d1e8` | 282 | Abort real: `AbortImpl` → `svcBreak` |

### Fluxo de Execução (patches desligados)

1. rtld carrega módulos, aplica relocações
2. `nn::init::Start` inicia SDK
3. Vários `SendSyncRequest` e `SignalProcessWideKey`
4. Threads criadas via `CreateThread`/`StartThread`
5. Serviço `handle=0x681f4` retorna erro `0x2` (detetado e tratado)
6. Serviço `handle=0x481f8` envolvido no aborto final
7. Thread para em `0x8390d1e8` (epílogo de `AbortImpl`)

---

## Próximos Passos (Fase 1)

1. **Fix mutex/sincronização** — investigar `0x83ab1500` (verificação de posse de `InternalCriticalSection`)
2. **Implementar threads reais** — `CreateThread`/`StartThread` são stubs
3. **Fix serviço IPC** que retorna erro `0x2` (handle `0x681f4`)
4. **Filesystem/fsp-srv** — leitura real de arquivos do RomFS
5. **Integração video_core upstream** — motor Maxwell do Eden
6. **NVDRV/GPU** — pipeline de comandos Maxwell → Vulkan
7. **Apresentação HDMI** — conectar framebuffer ao Wayland

---

## Arquivos Modificados

- `src/eden_bridge.cpp` — `eden_send_sync_request` (correção de falso sucesso)
- `src/svc_dispatcher.cpp` — `svc_break` (desvio temporário do loop de AbortImpl)
- `src/native_engine.cpp` — `handle_guest_signal` (usa `c.pc + 4` para continuação)

---

## Backups

Criados em `backups_20261030/` (cópia de `src/`, `include/`, `CMakeLists.txt` antes das alterações).
