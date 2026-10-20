// Substitui `core/core.h` nos ficheiros que só precisam de chegar ao
// KernelCore. Ver vendor/eden/README.md.
//
// `Core::System` é a raiz do Eden e arrasta o VFS, o NCA e o resto do
// emulador. A camada de IPC não usa nada disso — usa `system.Kernel()`, e é
// isso que fica declarado aqui.
//
// `KernelCore` é o nosso (ver k_kernel_core_bridge.h); o `System` é o dono
// que o nosso motor implementa, e a classe é abstracta precisamente para
// que ninguém a instancie sem querer.
//
// Esta é a ponte em miniatura, e o que mostra é a dimensão do problema:
// **duas linhas de interface** separam a camada de sistema do emulador. Não é
// o `Core::System` que é grande — é tudo o que ele arrasta atrás de si.
#pragma once

#include <memory>
#include <utility>

namespace Kernel {
class KernelCore;
} // namespace Kernel

namespace Service {
class ServerManager;
namespace SM {
class ServiceManager;
} // namespace SM
} // namespace Service

namespace Core {

// O que o `System` tem de oferecer, e medido.
//
//     Kernel()          -- o `ServiceManager` e o `HLERequestContext` sobem
//                          daqui, e com eles quase todo o IPC
//     ServiceManager()  -- `sm.cpp:309` e `sm.cpp:316`; o registo de servicos
//                          do Eden, de onde saem as portas nomeadas
//
// Dezanove métodos no `Core::System` do Eden; a camada de IPC pede dois.
// Os outros dezassete sao memoria, NCA, VFS, audio, video, e as coisas que
// emitem SVCs — que e' o emulador.
class System {
public:
    virtual ~System() = default;
    virtual Kernel::KernelCore& Kernel() = 0;
    virtual Service::SM::ServiceManager& ServiceManager() = 0;

    // Arranca o servidor de servicos.
    //
    // No Eden isto lanca as threads queperingem as portas e esperam por
    // pedidos (`ServerManager::LoopProcess`). Aqui NAO: os pedidos sao
    // atendidos a sincrono, de dentro do `SendSyncRequest` do guest, que e'
    // como o motor nativo corre — nao ha loop de eventos para os servicos
    // viverem dentro.
    //
    // E' a MESMA adaptacao que `RunOnHostCoreThread` devolve `false`, e pelo
    // mesmo motivo: nao ha thread onde correr, e fingir que ha deixaria o
    // pedido pendurado para sempre.
    virtual void RunServer(std::unique_ptr<Service::ServerManager>&& server_manager) = 0;
};

} // namespace Core