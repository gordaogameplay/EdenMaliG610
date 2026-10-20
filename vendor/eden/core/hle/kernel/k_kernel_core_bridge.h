// O KernelCore mínimo.
//
// ## O que substitui
//
// `core/hle/kernel/kernel.h` e `kernel.cpp` do Eden.
//
// ## Porquê
//
// Medido: a camada de IPC chama **três** métodos no `KernelCore`, e só três
// (ver `grep -rhoE "kernel\.[A-Z][A-Za-z_]*\("` sobre `service/`, a tabela de
// handles, e os objectos de porta/sessão):
//
//     Kernel().RunOnHostCoreThread(...)     -- a espera adiada do ServerManager
//     System()                               -- para chegar à memória
//     UnregisterInUseObject(...)
//
// O `KernelCore` do Eden tem implementações que usam `Core::Timing`,
// `system.GetCpuManager().CurrentCore()`, `KDynamicPageManager` e
// `KDynamicPageManager` — a infra de emulação. Uma delas chega:
//
//     preemption_event = Core::Timing::CreateEvent("PreemptionCallback", ...);
//     system.CoreTiming().ScheduleLoopingEvent(time_interval, time_interval, ...);
//     return u32(system.GetCpuManager().CurrentCore());
//
// Nenhuma das três coisas que o IPC pede tem a ver com isso. Por isso o
// `KernelCore` daqui tem três métodos com o nome certo, e não o resto.
//
// ## O que se perde, e porque não dói
//
// **Não há pré-emption com intervalo.** No Eden, o `KernelCore` agendar um
// evento periódico de pré-emption, porque há oito núcleos a correr código
// emulado que se pode alternar. Aqui o guest corre **nativo, numa thread**,
// e essa thread não é alternada por nós. A pré-emption que o Eden simula é
// a pré-emption do emulador de CPU, não do sistema.
//
// **Não há `CurrentCore()`.** Não há emulação de CPU, logo não há núcleo
// virtual a reportar. E o nosso motor decide ocore real uma vez, no
// arranque, e é isso.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <mutex>
#include <vector>

#include "vendor/eden/core/hle/kernel/svc_common.h"

namespace Core {
class System;
} // namespace Core

namespace Kernel {

class KAutoObject;
class KernelCore;

// Um "core" do ponto de vista do Eden. Nao ha emulacao de CPU, por isso um
// unico core virtual e' suficiente — e `CurrentCore()` devolve sempre 0.
//
// Chama-se `HostCore` e nao `Core` por causa de um sombreamento que mordeu:
// dentro de `namespace Kernel`, escrever `Core::System` resolvia para
// `Kernel::Core::System` — esta classe — e nao para `::Core::System`, que e'
// onde vive o `System` do Eden. O compilador dizia "expected ')'" numa linha
// onde nao havia nada de errado, que e' a forma mais cara de um bug de nome.
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
    // `RegisterWithKernel` (k_auto_object.cpp) regista o objecto na lista de
    // "em uso" para o `UnregisterInUseObject` poder fechar. O Eden guarda
    // isso num spin lock; aqui e' uma lista com um mutex, que e' o mesmo
    // objecto com o preco certo.
    void RegisterInUseObject(KAutoObject* self);
    void UnregisterInUseObject(KAutoObject* self);

    // ── o caminho de espera adiada ───────────────────────────────────────
    //
    // `ServerManager` usa isto quando um servico ainda nao esta registado: o
    // pedido fica pendurado e o cliente espera por um evento. No Eden e'
    // correr a funcao no "host core thread". Aqui nao ha host core thread —
    // ha uma thread. E' a unica thread de execucao do guest, e por isso
    // devolver `false` e' o honesto: nao ha onde correr, e fingir que corria
    // deixaria o pedido pendurado para sempre.
    //
    // E' a UNICA adaptacao aqui que muda comportamento em vez de o
    // remover. Fica nomeada e comentada, e se algum dia o jogo precisar de
    // servicos adiados, e' este o sitio.
    bool RunOnHostCoreThread(const std::function<void()>& func);

    // ── o que `KAutoObject` e oeden pedem e que nao muda nada ───────────
    bool IsShuttingDown() const { return false; }
    bool IsShuttingDownOrPaused() const { return false; }
    void Shutdown(bool) {}

    // Numeros de objecto. No Eden sao um contador atomic com regras de
    // reutilizacao; aqui e' um contador. A distincao so aparece quando dois
    // objectos recebem o mesmo numero, e isso nao acontece.
    u32 CreateNewObjectID() { return ++object_id_; }

private:
    ::Core::System& system_;
    HostCore core_{*this};
    u32 object_id_ = 0;
    std::vector<KAutoObject*> in_use_;
    std::mutex in_use_mutex_;
};

} // namespace Kernel