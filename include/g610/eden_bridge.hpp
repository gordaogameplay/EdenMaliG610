// Onde o motor nativo se liga ao sistema do Eden.
//
// Ver `vendor/eden/core/hle/kernel/k_process_bridge.h` para o `KProcess`,
// o `KThread` e o `Memory` mínimos, e `k_kernel_core_bridge.h` para o
// `KernelCore`.
//
// ## O que este ficheiro é
//
// A **cadeia de montagem**. Tudo o resto é a ponte; isto é a ordem em que as
// peças se montam umas dentro das outras:
//
//     EdenSystem  ── Kernel() ─────────► KernelCore
//         │                              │  (precisa de Core::System&)
//         ├── ServiceManager() ──────► SM::ServiceManager
//         │                              │  (regista o "sm:")
//         ├── KHandleTable             (inicializada por nós)
//         ├── KResourceLimit
//         ├── KProcess ── GetHandleTable(), GetProcessId(), GetResourceLimit()
//         └── KThread  ── GetOwnerProcess()
//
// ## O ciclo, e como se resolve
//
// `KernelCore` recebe `Core::System&` no construtor, e o `System` *contém* o
// `KernelCore`. Isto é um ciclo de construção, não um ciclo de tipos.
//
// A solução é alocação em duas fases: o `EdenSystem` é completo — e por isso
// já é um `Core::System` — no momento em que o `KernelCore` é construído, e o
// `KernelCore` guarda a referência. É por isso que o `KernelCore` vive atrás
// de um `unique_ptr` e não como membro por valor: por valor, o construtor do
// membro correria antes de a base estar pronta.
//
// ## O que NÃO existe aqui
//
// Page tables, escalonamento, interfaces de CPU, temporizadores, VFS, NCA.
// Isto não emula nada. O guest corre nativo, e este sistema existe só para
// atender o IPC que o guest lhe faz — que, medido, são vinte e tal objectos
// de porta e sessão e quatro comandos do `sm:`.
#pragma once

#include <cstdint>
#include <memory>

#include "g610/guest_memory.hpp"
#include "vendor/eden/core/system_ref.h"

namespace Kernel {
class KernelCore;
class KObjectNameGlobalData;
class KHandleTable;
class KResourceLimit;
class KProcess;
class KThread;
} // namespace Kernel

namespace Service {
class ServerManager;
namespace SM {
class ServiceManager;
} // namespace SM
} // namespace Service

namespace g610 {

// O sistema. Ver a nota da cadeia acima.
class EdenSystem final : public Core::System {
public:
    EdenSystem(GuestMemory& memoria, std::uint64_t pid, std::uint64_t tid);
    ~EdenSystem() override;

    // ── o que o `Core::System` promete ────────────────────────────────────
    //
    // **Nada.** Os três métodos (`Kernel()`, `ServiceManager()`,
    // `RunServer()`) vivem no `Core::System` de `system_ref.h` e não são
    // virtuais, exactamente como no Eden (`core.h:300`, `:331`, `:401`).
    //
    // Isto não é estilo. No Eden são métodos normais, logo têm de existir
    // como símbolos; declará-los virtuais aqui muda o que o compilador
    // emite, e o linker vai atrás de um símbolo que ninguém escreveu. Ver a
    // nota longa em `system_ref.h`.
    //
    // O que esta classe faz é o trabalho de montagem, que é dela e só dela:
    // construir as peças pela ordem certa e ligá-las. `ligar()` está no
    // `Core::System` e é chamado no fim do construtor, quando o `KernelCore`
    // e o `ServiceManager` já existem — o que resolve o ciclo sem
    // `virtual` e sem Chronologia de membros.

    // O handle da thread principal, na tabela de handles do Eden.
    //
    // É o que o Eden passa no x1 do entry point (`k_process.cpp:1006-1010`) e
    // é o que o SDK da Nintendo lê para preencher
    // `nn::os::detail::g_OsBootParamter`. Sem ele, o SDK recebe zero, e zero é
    // o valor de um mutex livre — por isso o `SdkMutexType::TryLock` achava que
    // o mutex já era seu.
    std::uint32_t main_thread_handle() const noexcept { return main_thread_handle_; }

    // O endereço de TLS. Vai para o `KThread` para que
    // `KThread::GetTlsAddress` responda, que é o que a camada de IPC usa
    // para saber onde está o `MessageBuffer` do guest.
    void set_tls(std::uint64_t endereco) noexcept;
    std::uint64_t tls() const noexcept { return tls_; }

private:
    GuestMemory& memoria_;
    std::uint64_t pid_;
    std::uint64_t tid_;
    std::uint64_t tls_ = 0;
    std::uint32_t main_thread_handle_ = 0;

    // Ver a nota do ciclo: `unique_ptr`, e não membro por valor.
    std::unique_ptr<Kernel::KernelCore> kernel_;
    std::unique_ptr<Service::SM::ServiceManager> sm_;
    std::unique_ptr<Core::Reporter> reporter_;

    // Vivem aqui, não dentro do `KProcess`.
    //
    // No Eden o `KProcess` é dono de ambos (`k_process.cpp:1178`). Aqui o
    // `KProcess` é da ponte, e a ponte só guarda referências — por isso quem
    // os constrói e quem os guarda tem de ser alguém de fora, e esse alguém
    // é esta classe.
    //
    // `unique_ptr` com o tipo só declarado à frente: dá para ter o membro no
    // cabeçalho e o tipo completo só no `.cpp`. O `~EdenSystem` está
    // **declarado aqui e definido no `.cpp`** — se estivesse definido aqui, o
    // compilador teria de eliminar o destructor do `unique_ptr` aqui dentro, e
    // isso exige o tipo completo. É a mesma razão pela qual o `KernelCore`
    // é um `unique_ptr`: a construção tem de ser controlável, não implícita.
    std::unique_ptr<Kernel::KHandleTable> handles_;
    std::unique_ptr<Kernel::KResourceLimit> limites_;
    std::unique_ptr<Kernel::KProcess> processo_;
    std::unique_ptr<Kernel::KThread> thread_;
};

// ── os dois SVCs que passam a ser do Eden ──────────────────────────────────
//
// Devolvem `false` se o sistema não estiver montado — que é o caso dos testes
// que não ligam memória ao despachante. Nesse caso o despachante cai no
// caminho antigo, e o jogo continua a dar o mesmo erro de sempre em vez de
// crashar.

// `ConnectToNamedPort(nome_ptr, out_handle)`.
bool eden_connect_to_named_port(Kernel::KernelCore& kernel, GuestMemory& memoria,
                                std::uint64_t nome_ptr, std::uint32_t* out_handle);

// `SendSyncRequest(session_handle)` e `SendSyncRequestLight`.
//
// O `SendSyncRequest` **ignora x1 e x2**: é o que o Eden faz, e é o que o
// guest espera. O kernel usa o `MessageBuffer` do TLS da thread, que o
// `KClientSession::SendSyncRequest` vai buscar sozinho.
bool eden_send_sync_request(Kernel::KernelCore& kernel, std::uint32_t session_handle);

} // namespace g610