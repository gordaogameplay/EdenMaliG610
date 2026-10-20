# vendor/eden — código do Eden

Isto é **código do Eden**, copiado. Não é uma reimplementação.

## Licença

Cada ficheiro conserva o seu header SPDX original, que é o que a GPL exige e
o que dá a proveniência de cada linha:

| origem | licença |
|---|---|
| Eden Emulator Project | GPL-3.0-or-later |
| yuzu Emulator Project (via o Eden) | GPL-2.0-or-later |
| Dolphin Emulator Project | GPL-2.0-or-later |
| Tony Wasserka (`bit_field.h`) | BSD-3-Clause |

**Este projecto é GPL.** É uma exigência da GPL-3.0 que a obra derivada seja
distribuída sob GPL-3.0, e é a razão de o texto estar aqui à vista em vez de
num comentário qualquer.

## O que foi tirado, e o que NÃO foi

**Tirado** — a camada de SISTEMA, a parte sistemática que é o que estava a
ser reinventada à mão:

    common/        bit_field, swap, common_types, common_funcs, assert,
                   concepts, logging, scope_exit, thread, scratch_buffer
    core/hle/      ipc.h              (o formato do buffer de comando)
    core/hle/service/  hle_ipc, ipc_helpers, server_manager, service,
                      cmif_types, os/{mutex,multi_wait}, sm/
    core/hle/kernel/   k_handle_table, k_port, k_client_port, k_server_port,
                      k_client_session, k_server_session, k_session, k_event,
                      k_synchronization_object, k_auto_object, k_object_name,
                      k_process, k_thread, k_resource_limit, k_spin_lock,
                      svc_common, svc.h, svc_results

**NÃO tirado** — nada de `core/cpu/`, `core/arm/`, `core/frontend/`, do
scheduler, nem das traduções de instrução. O CPU corre **nativo**; a única
tradução é Maxwell → Valhall, e essa é nossa (`src/gpu/`).

## Os cortes

**`Core::ArmInterface` e `m_arm_interfaces`** — removidos de `k_process.h` e
`k_thread.h`. É o array de interfaces de CPU, uma por núcleo, e é a única
coisa que ligava esta camada à emulação. Nenhum outro ponto do cabeçalho
precisa de ARM: o resto do livro de registo por núcleo usa só
`Core::Hardware::NUM_CPU_CORES`, que é uma constante — **8 no Tegra X1 e 8 no
RK3588**, com a mesma forma (4 fracos + 4 fortes).

**`k_process.h` completo** — o IPC só lhe pede quatro coisas:

    GetOwnerProcess()->GetHandleTable()
    GetOwnerProcess()->GetProcessId()
    GetCurrentProcess(...).GetResourceLimit()
    GetCurrentMemory(...).ReadCString()      // só no sm.cpp

Tudo o resto do `k_process.h` — page tables, reserva de recursos, metadata do
programa — não é usado pela camada de IPC e fica de fora. O que o substitui é
uma ponte fina nossa, porque o processo aqui é o nosso motor nativo, não o
do Eden.

## Include paths

Todos os `#include "common/..."` e `#include "core/..."` foram reescritos para
`"vendor/eden/common/..."` e `"vendor/eden/core/..."`. Nada mais foi tocado
dentro dos ficheiros, para que a correspondência com o original se veja a olho.
