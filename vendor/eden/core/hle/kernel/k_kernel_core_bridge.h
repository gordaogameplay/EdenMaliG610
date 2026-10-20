// O KernelCore mínimo. Ver `k_process_bridge.h` para a ponte do processo.
//
// ## O que substitui
//
// `core/hle/kernel/kernel.h` e `kernel.cpp` do Eden.
//
// ## Porquê
//
// Medido: a camada de IPC chama **três** métodos no `KernelCore`, e só três
// (ver `grep -rhoE "kernel\.[A-Z][A-Za-z_]*\("` sobre `service/`, a tabela de
// handles e os objectos de porta/sessão):
//
//     Kernel().RunOnHostCoreThread(...)     -- a espera adiada do ServerManager
//     System()                               -- para chegar à memória
//     UnregisterInUseObject(...)
//
// O `KernelCore` do Eden tem implementações que usam `Core::Timing`,
// `system.GetCpuManager().CurrentCore()` e `KDynamicPageManager` — a infra
// de emulação. Nenhuma das três coisas que o IPC pede tem a ver com isso:
//
//     preemption_event = Core::Timing::CreateEvent("PreemptionCallback", ...);
//     system.CoreTiming().ScheduleLoopingEvent(time_interval, ...);
//     return u32(system.GetCpuManager().CurrentCore());
//
// ## A ordem dos includes, que é a parte que custou tempo
//
// Este ficheiro **não** inclui `k_handle_table.h`, `k_resource_limit.h` nem
// `k_synchronization_object.h`. Os tipos `KHandleTable` e `KResourceLimit` são
// usados só por referência, e `KSynchronizationObject` fica para o ficheiro
// de cima, que é quem precisa dele.
//
// A regra que disto sai: **o ficheiro que define o tipo-base vem primeiro, e
// quem precisa dele sobe na cadeia.** Sem isso, `ponte -> x.h -> ponte`, e com
// `#pragma once` um dos dois fica com o corpo vazio e o compilador diz que o
// tipo não existe — sem nenhuma pista de que a causa é um ciclo. Aconteceu
// duas vezes nesta costura.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <memory>
#include <mutex>
#include <vector>

#include "vendor/eden/core/hle/kernel/k_auto_object_container.h"
#include "vendor/eden/core/hle/kernel/k_slab_heap.h"
#include "vendor/eden/core/hle/kernel/svc_common.h"

namespace Core {
class System;
} // namespace Core

namespace Kernel {

class KAutoObject;
class KernelCore;

// Um "core" do ponto de vista do Eden. Não há emulação de CPU, por isso um
// único core virtual chega — e `GetID()` devolve sempre 0.
//
// Chama-se `HostCore` e não `Core` por causa de um sombreamento que mordeu:
// dentro de `namespace Kernel`, escrever `Core::System` resolvia para
// `Kernel::Core::System` — esta classe — e não para `::Core::System`. O
// compilador apontava uma linha onde não havia nada de errado, que é a forma
// mais cara de um bug de nome.
class HostCore {
public:
    explicit HostCore(KernelCore& kernel) : kernel_(kernel) {}
    u32 GetID() const { return 0; }

private:
    KernelCore& kernel_;
};

class KernelCore {
public:
    explicit KernelCore(::Core::System& system);
    ~KernelCore();

    HostCore& GetCore(u32 index) {
        (void)index;
        return core_;
    }
    const HostCore& GetCore(u32 index) const {
        (void)index;
        return core_;
    }

    ::Core::System& System() { return system_; }
    const ::Core::System& System() const { return system_; }

    // ── o que o `KAutoObject` usa ────────────────────────────────────────
    //
    // `RegisterWithKernel` (k_auto_object.cpp) mete o objecto numa lista de
    // "em uso" para o `UnregisterInUseObject` poder fechá-lo. O Eden usa um
    // spin lock; aqui um mutex, que é o mesmo objecto pelo preço certo.
    void RegisterInUseObject(KAutoObject* self);
    void UnregisterInUseObject(KAutoObject* self);

    // ── o caminho de espera adiada ───────────────────────────────────────
    //
    // `ServerManager` usa isto quando um serviço ainda não está registado: o
    // pedido fica pendurado e o cliente espera por um evento.
    //
    // **Isto é a única adaptação que MUDA comportamento em vez de o
    // remover.** No Eden corre a função no "host core thread". Aqui não há
    // host core thread — há uma thread, e é a do guest. Devolver `false` é o
    // honesto: prometer que corre seria deixar o pedido pendurado para
    // sempre. E o jogo vai notar, no dia em que precisar de um serviço
    // adiado, que este é o sítio a mexer.
    // No Eden devolve um `std::jthread` que corre a funcao numa thread
    // auxiliar do host (kernel.h:217). Aqui devolve o mesmo tipo, vazio:
    // nao ha thread onde correr.
    //
    // Ver a nota de `Core::System::RunServer` — e' a mesma adaptacao, pelo
    // mesmo motivo. Um `jthread` destructor faz `join`, e um vazio faz
    // `join` de nada.
    std::jthread RunOnHostCoreThread(std::string&& thread_name,
                                     std::function<void()> func) {
        (void)thread_name;
        (void)func;
        return std::jthread{};
    }

    // ── o que o Eden pede e aqui não muda nada ───────────────────────────
    bool IsShuttingDown() const { return false; }
    bool IsShuttingDownOrPaused() const { return false; }
    void Shutdown(bool) {}

    // No Eden há um contador atómico com regras de reutilização. Aqui é um
    // contador. A distinção só aparece quando dois objectos recebem o mesmo
    // número — e isso não acontece.
    u32 CreateNewObjectID() { return ++object_id_; }

    // ── a lista de objectos com pai ──────────────────────────────────────
    //
    // `KAutoObjectWithList::Destroy` chama `kernel.ObjectListContainer().
    // Unregister(this)`. No Eden o contentor existe para o `GetOwnedCount`,
    // que o gestor de memória usa para contar quantos objectos um processo
    // possui. Aqui é uma lista e mais nada, e é o suficiente para o
    // `Destroy` não rebentar.
    KAutoObjectWithListContainer& ObjectListContainer() { return object_list_; }
    const KAutoObjectWithListContainer& ObjectListContainer() const {
        return object_list_;
    }

    // ── os slabs ─────────────────────────────────────────────────────────
    //
    // A MEDIDA que libertou isto: `KSlabHeap<T>` é `KSlabHeapBase<false>` —
    // **sem expansão dinâmica** — e inicializa-se com um bloco de memória e
    // um tamanho (`k_slab_heap.h:177-186`):
    //
    //     void Initialize(void* memory, size_t memory_size) {
    //         BaseHeap::Initialize(sizeof(T), memory, memory_size);
    //     }
    //
    // Espera-se que isto precisasse de páginas, e portanto de uma page
    // table, e portanto do emulador. Não precisa: a página era o que o
    // Eden usava para *expandir* o slab, e a expansão dinâmica é
    // precisamente a variante que não está em uso. Um bloco é uma lista
    // livre.
    //
    // Um slab por tipo, com 64 objectos. Vinte e tal tipos, 64 objectos cada:
    // alguns KiB. A memória é-resources livreada de propósito — vive tanto
    // quanto o `KernelCore`, e libertá-la exigiria um destrutor por tipo que
    // não compensa um `free` que nunca acontece.
    template <typename T>
    KSlabHeap<T>& SlabHeap() {
        struct Holder {
            Holder() {
                constexpr std::size_t kObjetos = 64;
                // `operator new` com alinhamento, e não um vector<char>:
                // um vector de char não garante alinhamento para T, e um
                // T torto e' um crash que só aparece com alguns tipos.
                bloco.reset(static_cast<char*>(
                    ::operator new(kObjetos * sizeof(T), std::align_val_t{alignof(T)})));
                heap.Initialize(bloco.get(), kObjetos * sizeof(T));
            }
            std::unique_ptr<char[]> bloco;
            KSlabHeap<T> heap;
        };
        static Holder holder;
        return holder.heap;
    }

private:
    ::Core::System& system_;
    HostCore core_{*this};
    u32 object_id_ = 0;
    std::vector<KAutoObject*> in_use_;
    std::mutex in_use_mutex_;
    KAutoObjectWithListContainer object_list_{*this};
};

// Desliga a troca de contexto enquanto se segura um lock (k_thread.h:981-993).
//
//     explicit KScopedDisableDispatch(KernelCore& kernel) : m_kernel{kernel} {
//         if (m_kernel.IsShuttingDown()) { return; }
//         GetCurrentThread(kernel).DisableDispatch(kernel);
//     }
//
// No Eden, para a tabela de handles não ser mexida a meio de uma troca de
// contexto. Aqui **não há troca de contexto**: o guest corre noutra thread, de
// forma nativa e síncrona. É um no-op com o nome certo.
//
// Vive aqui, e não na ponte do processo, **porque não precisa de `KThread`** —
// e essa é a razão de não haver ciclo entre os dois.
class KScopedDisableDispatch {
public:
    explicit KScopedDisableDispatch(KernelCore&) {}
};

} // namespace Kernel