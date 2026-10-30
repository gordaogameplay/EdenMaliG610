# IPC e Inicialização do SDK — Estado

## Resumo

Com patches desligados (`G610_PATCH_DISABLE=all`), o jogo:
1. Booteia via rtld, carrega 5 módulos
2. Faz 282 SVCs (era 363 antes da correção IPC)
3. Cria threads via `CreateThread`/`StartThread` (implementadas, não são stubs)
4. Faz chamadas IPC (`SendSyncRequest`) a vários serviços
5. **Deadlock** em `WaitProcessWideKey` — CVs nunca sinalizadas

Com patches ligados (estado original):
- Loop em `0x83ab15e4` com 0 SVCs (patches mascaram o aborto real)

---

## Alterações Feitas

### 1. `src/eden_bridge.cpp` — `eden_send_sync_request`

**Antes:** `return processado || resultado == Kernel::ResultNotFound;`
**Depois:** `return processado;` + leitura do resultado CMIF em `tls_buffer + 0x28` (`kOffBody`), devolvendo `false` se não-zero.

Isso impede que o guest prossiga com um resultado de erro do servidor (ex. `0x2` do handle `0x681f4`), forçando `ResultInvalidHandle` em vez de fabricar sucesso.

### 2. `src/svc_dispatcher.cpp` — `svc_break`

**Alteração:** Quando `pc == 0x83b0bba4 && gpr[30] == 0x8390d1e8` (AbortImpl chama svcBreak), define `ctx.pc = 0x8390d1ec`. O motor usa `c.pc + 4` para definir o PC após o SVC, fazendo o guest retomar em `0x8390d1f0` (função após AbortImpl).

**Nota:** Solução temporária. A causa-raiz (por que o guest chama AbortImpl) deve ser corrigida na Fase 1.

### 3. `src/native_engine.cpp` — `handle_guest_signal`

**Alteração:** `G610_MCTX_PC(*uc) = pc + 4` → `G610_MCTX_PC(*uc) = c.pc + 4`, permitindo que `svc_break` altere o ponto de continuação.

---

## Descobertas Críticas

### Patches e RTLD

O patch `vtable[4]` (escreve `0x83aec290` em `0x84370168`) é **sobrescrito** pelo RTLD do guest, que processa relocações RELATIVE em `.data` (a vtable está em `.data`). A relocação lê o valor atual como addend e reescreve com `base + addend`, destruindo o patch.

O patch `cbnz_rt` (escreve `mov w0, wzr` em `0x83aec244` no `.text`) **sobrevive** ao RTLD, pois `.text` não tem relocações RELATIVE.

### Abortos Encontrados

| Condição | Local | SVCs | Notas |
|----------|-------|------|-------|
| Patches ligados | `0x83ab15e4` | 0 | Loop mascarado (`ret` → `x30=0x83ab15e4` → `ret` → ...) |
| Patches desligados | `0x8390d1e8` | 282 | `AbortImpl` → `svcBreak` → `b self` |

### Fluxo de Execução (patches desligados)

1. rtld carrega módulos, aplica relocações
2. `nn::init::Start` inicia SDK
3. Vários `SendSyncRequest` e `SignalProcessWideKey`
4. `CreateThread`/`StartThread` criam threads guest (host threads reais)
5. Serviço `handle=0x681f4` retorna erro `0x2` (detetado e tratado pelo fix IPC)
6. `WaitProcessWideKey` em 3 CVs nunca sinalizadas → deadlock

### CVs com Waiters Mas Sem Sinal

```
[arb] cv 0x10a5096e4 (1): {0x20003,addr=0x10a5096d8,waiter}
[arb] cv 0xe2028194 (1): {0x18002,addr=0xe2028188,waiter}
[arb] cv 0x10a509a8c (1): {0x28004,addr=0x10a509a80,waiter}
```

Sinais enviados: `0x8437e20c`, `0x8434ed4c` — **nenhum corresponde aos CVs com waiters**.

### Handle `0x681f4`

Retorna erro `0x2` no `SendSyncRequest`. Detetado pelo fix IPC, o guest recebe `ResultInvalidHandle` e continua (faz mais SVCs).

---

## Próximos Passos (Fase 1)

1. **Fix `SignalProcessWideKey`/`WaitProcessWideKey`** — keys não batem: signal usa `x[0]` como chave, wait usa `x[1]` como chave. Investigar se a chamada usa o registo correto.
2. **Fix `svcBreak` abort** — o `svc_break` PC fix pode não funcionar devido a `sigreturn` restaurar PC de `uc_mcontext.pc` em vez de `gregs[31]`. Investigar.
3. **Implementar serviços ausentes** — quem deveria sinalizar as CVs `0x10a5096e4`, `0xe2028194`, `0x10a509a8c`?
4. **Filesystem/fsp-srv** — leitura real de arquivos do RomFS
5. **Integrar video_core upstream** — motor Maxwell do Eden
6. **NVDRV/GPU** — pipeline de comandos Maxwell → Vulkan
7. **Apresentação HDMI** — conectar framebuffer ao Wayland

---

## Arquivos Modificados

- `src/eden_bridge.cpp` — `eden_send_sync_request` (correção de falso sucesso + leitura de resultado CMIF)
- `src/svc_dispatcher.cpp` — `svc_break` (desvio temporário do loop de AbortImpl)
- `src/native_engine.cpp` — `handle_guest_signal` (usa `c.pc + 4` para continuação)

---

## Backups

Criados em `backups_20261030/` (cópia de `src/`, `include/`, `CMakeLists.txt` antes das alterações).
