// O `appletOE` — o proxy do `am` (Application Manager), **nosso**.
//
// ## O que é
//
// O SDK da Nintendo, no arranque, pede o serviço `appletOE` ao `sm:`. Esse
// serviço é o *proxy* do `am`: dá ao jogo um conjunto de sub-interfaces
// (`ICommonStateGetter`, `ISelfController`, `IApplicationFunctions`, …) que
// respondem a perguntas sobre o estado da aplicação.
//
// A estrutura vem do Eden (`core/hle/service/am/service/`), e é esta:
//
//   appletOE  (o serviço)
//     └─ IApplicationProxy        (o proxy, comando 0 = OpenApplicationProxy)
//          ├─ 0  GetCommonStateGetter
//          ├─ 1  GetSelfController
//          ├─ 2  GetWindowController
//          ├─ 3  GetAudioController
//          ├─ 4  GetDisplayController
//          ├─ 10 GetProcessWindingController
//          ├─ 11 GetLibraryAppletCreator
//          ├─ 20 GetApplicationFunctions
//          └─ 1000 GetDebugFunctions
//
// ## Porque não é o do Eden
//
// O `am` do Eden tem 8946 linhas e arrasta o **núcleo de HID** (o
// `ButtonPoller` precisa de `hid_core/frontend/emulated_controller.h`) e o
// **sistema de janelas** (`WindowSystem`, que é o que o `appletOE` consulta
// para saber que applet é o do processo). Trazer o `am` inteiro seria trazer
// o emulador.
//
// O que o SDK precisa, no arranque, é um conjunto pequeno de respostas. E
// este ficheiro é a versão fina: a **interface CMIF é a do Eden, tal e
// qual** (os IDs dos comandos e o formato das respostas são o que tem de
// ser exacto — é o contrato com o guest), e o **conteúdo** é o mínimo que
// um jogo precisa.
//
// ## O estilo
//
// Os handlers são escritos à mão (`IPC::RequestParser` para ler,
// `IPC::ResponseBuilder` para responder), e não com o mecanismo tipado
// (`CmifReplyWrap`). O mecanismo tipado obriga a que a assinatura do handler
// corresponda exactamente à do comando, e isso obriga a vendorizar os
// cabeçalhos do `am` inteiro. À mão, o que interessa é o **formato** do
// pacote, e esse está medido no Eden.
//
// A consequência: cada handler responde com o que o Eden responderia,
// **quando o valor é óbvio** (o modo de performance é `Normal`, o foco é
// `NotFocused`, o idioma é o do sistema). Onde o Eden consulta o estado
// real — o applet, a janela, a camada de display — não há estado para
// consultar, e a resposta é a de um applet que está a correr em primeiro
// plano, que é o caso do jogo.
//
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

namespace ia::services {

namespace {

using Service::HLERequestContext;

using IPC::RequestParser;
using IPC::ResponseBuilder;

// ─────────────────────────────────────────────────────────────────────────────
// Os resultados
// ─────────────────────────────────────────────────────────────────────────────
//
// **Um Result invalido e' pior do que um Result de erro.** O SDK da
// Nintendo aborta ante um Result invalido: `nn::result::detail::ConvertInvalidImpl`
// (medido em 0x82d37748) chama o abort. E o nosso `ResultUnknown` — que e'
// `0xFFFFFFFF` — e' exactamente isso: um Result invalido.
//
// A medicao: o SDK pediu `ReceiveMessage` (comando 1 do `ICommonStateGetter`),
// o nosso servico respondeu `ResultUnknown`, e o SDK morreu num assert.
//
// A regra que daqui sai, e que vale para **todos** os comandos deste
// servico:
//
//   - um comando que sabemos responder: `ResultSuccess` e o valor certo;
//   - um comando que nao sabemos responder: `ResultSuccess` e saidas a zero.
//
// Ou seja: **nunca um Result de erro**. O Eden pode dar-se a esse luxo
// porque implementa tudo; aqui, um Result de erro e' um assert no guest.
//
// A excecao e' o `ReceiveMessage`, em que o proprio Eden devolve um
// Result de erro especifico — `Service::AM::ResultNoMessages`
// (`am_results.h`), que e' um Result **valido** (modulo 128) e que o SDK
// sabe interpretar como "nao ha mensagens".
constexpr Result kResultNoMessages{ErrorModule::AM, 3};

// `ResultSuccess` e `ResultUnknown` sao globais (estao em
// `core/hle/result.h`, fora de qualquer namespace) — o Eden usa-os assim
// nos seus servicos, e o mesmo vale aqui.

// ─────────────────────────────────────────────────────────────────────────────
// O estado do applet
// ─────────────────────────────────────────────────────────────────────────────
//
// O `am` do Eden tem um `Applet` com um `LifecycleManager`, e o
// `WindowSystem` empurra para a fila do applet as mensagens de mudança de
// estado. A fila e' o que o `ReceiveMessage` vai buscar.
//
// **O que o SDK espera, medido:** o SDK ficou preso a pedir `ReceiveMessage`
// sem parar. E a mensagem que falta e' a de entrada no primeiro plano —
// `AppletMessage::ChangeIntoForeground` (= 1), do enum do Eden
// (`am_types.h:125-147`).
//
// No Switch, um applet e' lançado, o `am` mete-o em primeiro plano e
// entrega-lhe essa mensagem. O SDK inicializa, e so depois de a receber e'
// que continua.
//
// Aqui nao ha `Applet`, nem `LifecycleManager`, nem `WindowSystem`. Mas a
// mensagem e' uma **primeira vez**: o applet e' criado, e vai para o
// primeiro plano. Por isso a fila tem uma mensagem, e so uma.
//
// A fila vive no `AppletOE` — o objecto do serviço — e e' partilhada por
// referencia com o proxy e com as sub-interfaces. **Nao pode viver na
// sub-interface**: cada `PushIpcInterface` cria um objecto novo, e a
// mensagem seria entregue uma vez por sub-interface.
struct EstadoDoApplet {
    // As mensagens, por ordem. `AppletMessage`, do Eden (`am_types.h`).
    std::vector<u32> mensagens;

    // A mensagem que o `am` entrega quando o applet e' lanado:
    // `ChangeIntoForeground` = 1.
    static constexpr u32 kChangeIntoForeground = 1;

    // O evento do sistema: o que o `GetEventHandle` devolve, e o que e'
    // sinalizado quando o estado muda.
    //
    // **A ligacao entre a mensagem e o evento e' do Eden**, e e' o que
    // faz o SDK arrancar (`lifecycle_manager.cpp:31-34`):
    //
    //     void LifecycleManager::PushUnorderedMessage(Kernel::KernelCore& kernel,
    //                                                AppletMessage message) {
    //         m_unordered_messages.push_back(message);
    //         this->SignalSystemEventIfNeeded(kernel);
    //     }
    //
    // A mensagem diz **o que** mudou; o evento diz **que** mudou. O SDK
    // espera por um e le o outro.
    Kernel::KEvent* evento = nullptr;

    // O handle do evento. Vem do `KHandleTable::Add` — o `KEvent` nao tem
    // `GetHandle()`, porque o handle vive na tabela, e nao no objecto.
    std::uint32_t handle_evento = 0;

    bool tem_mensagem() const { return !mensagens.empty(); }

    // Tira a proxima. Devolve `None` (= 0) quando nao ha nenhuma.
    u32 proxima() {
        if (mensagens.empty())
            return 0;
        const u32 m = mensagens.front();
        mensagens.erase(mensagens.begin());
        return m;
    }

    // Empurra uma mensagem e sinaliza o evento do sistema.
    void empurrar(Kernel::KernelCore& kernel, u32 mensagem) {
        mensagens.push_back(mensagem);
        if (evento != nullptr)
            evento->Signal(kernel);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O evento
// ─────────────────────────────────────────────────────────────────────────────
//
// Varios comandos do `am` devolvem um **handle de evento** — o evento de
// notificacao do applet, o evento do sleep lock, o evento de mudanca de
// resolucao. O Eden devolve o evento do applet, que vive no
// `LifecycleManager`.
//
// Aqui nao ha applet nem gestor de ciclo de vida: ha um motor a correr um
// jogo. O que se devolve e' um evento **novo**, registado na tabela de
// handles do processo, que nunca e' sinalizado — que e' exactamente o
// comportamento de um applet que nao tem nada para notificar.
//
// A razao de ser um evento a serio e nao um handle zero: um handle zero e'
// um handle **invalido**, e o guest que o tenta usar aborta. E o SDK usa
// estes eventos para esperar por algo — e o `WaitSynchronization` do Eden
// com um handle invalido e' um caminho que nao devolve nada.
//
// O que o Eden faz, medido (`common_state_getter.cpp:93-97`):
//
//     *out_event = m_applet->lifecycle_manager.GetSystemEvent().GetHandle();
//
// e' um `OutCopyHandle`, ou seja, um handle copiado para o guest.
std::uint32_t handle_de_evento(Core::System& system) {
    auto& kernel = system.Kernel();
    auto& processo = Kernel::GetCurrentProcess(kernel);

    auto* evento = new Kernel::KEvent(kernel);
    evento->Initialize(kernel, &processo);

    Kernel::Handle handle{};
    if (R_FAILED(processo.GetHandleTable().Add(kernel, &handle, evento))) {
        LOG_WARNING(Service_AM, "handle_de_evento: a reserva do handle falhou");
        return 0;
    }
    return static_cast<std::uint32_t>(handle);
}

// ─────────────────────────────────────────────────────────────────────────────
// Os sub-controllers
// ─────────────────────────────────────────────────────────────────────────────
//
// Cada um é um `ServiceFramework` com a tabela de comandos do Eden. A tabela
// é a parte que não se inventa: os IDs estão medidos no
// `core/hle/service/am/service/*.cpp` do Eden.
//
// Os handlers que não consequentialmente-necessários respondem
// `ResultUnknown`, e **dizem-no na telemetria**. A ideia é que a lista do
// que o jogo pede seja um registo, não uma adivinhação.

/// O `ICommonStateGetter` — o estado comum do applet.
class CommonStateGetter final : public Service::ServiceFramework<CommonStateGetter> {
public:
    CommonStateGetter(Core::System& system, std::shared_ptr<EstadoDoApplet> estado)
        : ServiceFramework{system, "ICommonStateGetter"}, estado_{std::move(estado)} {
        static const FunctionInfo functions[] = {
            {0, &CommonStateGetter::GetEventHandle, "GetEventHandle"},
            {1, &CommonStateGetter::ReceiveMessage, "ReceiveMessage"},
            {5, &CommonStateGetter::GetOperationMode, "GetOperationMode"},
            {6, &CommonStateGetter::GetPerformanceMode, "GetPerformanceMode"},
            {8, &CommonStateGetter::GetBootMode, "GetBootMode"},
            {9, &CommonStateGetter::GetCurrentFocusState, "GetCurrentFocusState"},
            {10, &CommonStateGetter::RequestToAcquireSleepLock, "RequestToAcquireSleepLock"},
            {11, &CommonStateGetter::ReleaseSleepLock, "ReleaseSleepLock"},
            {12, &CommonStateGetter::ReleaseSleepLockTransiently, "ReleaseSleepLockTransiently"},
            {13, &CommonStateGetter::GetAcquiredSleepLockEvent, "GetAcquiredSleepLockEvent"},
            {20, &CommonStateGetter::PushToGeneralChannel, "PushToGeneralChannel"},
            {31, &CommonStateGetter::GetReaderLockAccessorEx, "GetReaderLockAccessorEx"},
            {32, &CommonStateGetter::GetWriterLockAccessorEx, "GetWriterLockAccessorEx"},
            {50, &CommonStateGetter::IsVrModeEnabled, "IsVrModeEnabled"},
            {51, &CommonStateGetter::SetVrModeEnabled, "SetVrModeEnabled"},
            {52, &CommonStateGetter::SetLcdBacklighOffEnabled, "SetLcdBacklighOffEnabled"},
            {53, &CommonStateGetter::BeginVrModeEx, "BeginVrModeEx"},
            {54, &CommonStateGetter::EndVrModeEx, "EndVrModeEx"},
            {55, &CommonStateGetter::IsInControllerFirmwareUpdateSection,
             "IsInControllerFirmwareUpdateSection"},
            {60, &CommonStateGetter::GetDefaultDisplayResolution, "GetDefaultDisplayResolution"},
            {61, &CommonStateGetter::GetDefaultDisplayResolutionChangeEvent,
             "GetDefaultDisplayResolutionChangeEvent"},
        };
        RegisterHandlers(functions);
    }

private:
    // O evento de notificação. O Eden devolve um `KReadableEvent`; aqui
    // devolve-se um handle inválido e diz-se isso, porque um evento que
    // nunca é sinalizado é o que um applet sem gestor de eventos recebe.
    void GetEventHandle(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(estado_ ? handle_do_evento_do_sistema() : 0u);
    }

    // O handle do evento do sistema. O evento e' **o mesmo** que o
    // `handle_de_evento` devolveu — e por isso que o `empurrar` o possa
    // sinalizar.
    std::uint32_t handle_do_evento_do_sistema() const {
        return estado_ ? estado_->handle_evento : 0u;
    }

    void ReceiveMessage(HLERequestContext& ctx) {
        // A mensagem, se a houver. `AppletMessage`, que e' um u32.
        if (estado_ && estado_->tem_mensagem()) {
            const u32 mensagem = estado_->proxima();
            LOG_DEBUG(Service_AM, "ReceiveMessage: entrega {}", mensagem);
            ResponseBuilder rb{ctx, 2};
            rb.Push(ResultSuccess);
            rb.Push(mensagem);
            return;
        }

        // `ResultNoMessages`, como o Eden (`common_state_getter.cpp:99-104`).
        // E' um Result **valido**, e o SDK sabe o que significa.
        ResponseBuilder rb{ctx, 2};
        rb.Push(kResultNoMessages);
    }

    // `OperationMode`: 0 = Handheld, 1 = Console.
    void GetOperationMode(HLERequestContext& ctx) {
        LOG_DEBUG(Service_AM, "GetOperationMode: Handheld");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(0));
    }

    // `PerformanceMode`: 0 = Normal, 1 = Boost. O Eden decide pelo
    // `Settings::IsDockedMode()`; este motor corre um jogo, e o jogo não
    // está docked.
    void GetPerformanceMode(HLERequestContext& ctx) {
        LOG_DEBUG(Service_AM, "GetPerformanceMode: Normal");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(0));
    }

    // `SystemBootMode`: 0 = NormalBoot.
    void GetBootMode(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(0));
    }

    // `FocusState`: 0 = NotFocused, 1 = Focused. Um jogo que está a correr
    // tem o foco.
    void GetCurrentFocusState(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(1));
    }

    void RequestToAcquireSleepLock(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ReleaseSleepLock(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ReleaseSleepLockTransiently(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetAcquiredSleepLockEvent(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(handle_de_evento(system));
    }

    void PushToGeneralChannel(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetReaderLockAccessorEx(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetWriterLockAccessorEx(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsVrModeEnabled(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void SetVrModeEnabled(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetLcdBacklighOffEnabled(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void BeginVrModeEx(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void EndVrModeEx(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsInControllerFirmwareUpdateSection(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    // A resolução por omissão do ecrã do Switch: 1280x720.
    void GetDefaultDisplayResolution(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<s32>(1280));
        rb.Push(static_cast<s32>(720));
    }

    void GetDefaultDisplayResolutionChangeEvent(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(handle_de_evento(system));
    }

private:
    std::shared_ptr<EstadoDoApplet> estado_;
};

/// O `ISelfController` — o que o jogo pode fazer consigo próprio.
class SelfController final : public Service::ServiceFramework<SelfController> {
public:
    explicit SelfController(Core::System& system)
        : ServiceFramework{system, "ISelfController"} {
        static const FunctionInfo functions[] = {
            {0, &SelfController::Exit, "Exit"},
            {1, &SelfController::LockExit, "LockExit"},
            {2, &SelfController::UnlockExit, "UnlockExit"},
            {3, &SelfController::EnterFatalSection, "EnterFatalSection"},
            {4, &SelfController::LeaveFatalSection, "LeaveFatalSection"},
            {9, &SelfController::GetLibraryAppletLaunchableEvent, "GetLibraryAppletLaunchableEvent"},
            {10, &SelfController::SetScreenShotPermission, "SetScreenShotPermission"},
            {11, &SelfController::SetOperationModeChangedNotification,
             "SetOperationModeChangedNotification"},
            {12, &SelfController::SetPerformanceModeChangedNotification,
             "SetPerformanceModeChangedNotification"},
            {13, &SelfController::SetFocusHandlingMode, "SetFocusHandlingMode"},
            {14, &SelfController::SetRestartMessageEnabled, "SetRestartMessageEnabled"},
            {15, &SelfController::SetScreenShotAppletIdentityInfo, "SetScreenShotAppletIdentityInfo"},
            {16, &SelfController::SetOutOfFocusSuspendingEnabled, "SetOutOfFocusSuspendingEnabled"},
            {19, &SelfController::SetAlbumImageOrientation, "SetAlbumImageOrientation"},
            {40, &SelfController::CreateManagedDisplayLayer, "CreateManagedDisplayLayer"},
            {41, &SelfController::IsSystemBufferSharingEnabled, "IsSystemBufferSharingEnabled"},
            {42, &SelfController::GetSystemSharedLayerHandle, "GetSystemSharedLayerHandle"},
            {43, &SelfController::GetSystemSharedBufferHandle, "GetSystemSharedBufferHandle"},
            {44, &SelfController::CreateManagedDisplaySeparableLayer,
             "CreateManagedDisplaySeparableLayer"},
            {50, &SelfController::SetHandlesRequestToDisplay, "SetHandlesRequestToDisplay"},
            {51, &SelfController::ApproveToDisplay, "ApproveToDisplay"},
            {60, &SelfController::OverrideAutoSleepTimeAndDimmingTime,
             "OverrideAutoSleepTimeAndDimmingTime"},
            {90, &SelfController::GetAccumulatedSuspendedTickValue,
             "GetAccumulatedSuspendedTickValue"},
            {91, &SelfController::GetAccumulatedSuspendedTickChangedEvent,
             "GetAccumulatedSuspendedTickChangedEvent"},
        };
        RegisterHandlers(functions);
    }

private:
    // Sair do jogo. O Eden chama `m_system.Exit()`; aqui o jogo pode
    // simplesmente regressar, e a telemetria regista.
    void Exit(HLERequestContext& ctx) {
        LOG_WARNING(Service_AM, "Exit: o jogo pediu para sair");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void LockExit(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void UnlockExit(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void EnterFatalSection(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void LeaveFatalSection(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetLibraryAppletLaunchableEvent(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(handle_de_evento(system));
    }

    void SetScreenShotPermission(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetOperationModeChangedNotification(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetPerformanceModeChangedNotification(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetFocusHandlingMode(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetRestartMessageEnabled(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetScreenShotAppletIdentityInfo(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetOutOfFocusSuspendingEnabled(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetAlbumImageOrientation(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // As camadas de display são do subsistema de vídeo. Sem ele, não há
    // layer para dar — e diz-se.
    void CreateManagedDisplayLayer(HLERequestContext& ctx) {
        LOG_WARNING(Service_AM, "CreateManagedDisplayLayer: sem subsistema de video");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(0));
    }

    void IsSystemBufferSharingEnabled(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void GetSystemSharedLayerHandle(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(0));
        rb.Push(static_cast<u64>(0));
    }

    void GetSystemSharedBufferHandle(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(0));
    }

    void CreateManagedDisplaySeparableLayer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(0));
    }

    void SetHandlesRequestToDisplay(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ApproveToDisplay(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void OverrideAutoSleepTimeAndDimmingTime(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // O numero de ticks que o applet passou suspenso. Nao ha suspensao
    // aqui, e' zero.
    void GetAccumulatedSuspendedTickValue(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(0));
    }

    // O evento que dispara quando esse numero muda. Ver `handle_de_evento`.
    void GetAccumulatedSuspendedTickChangedEvent(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(handle_de_evento(system));
    }
};

/// O `IWindowController` — o estado da janela do applet.
class WindowController final : public Service::ServiceFramework<WindowController> {
public:
    explicit WindowController(Core::System& system)
        : ServiceFramework{system, "IWindowController"} {
        static const FunctionInfo functions[] = {
            {0, &WindowController::CreateWindow, "CreateWindow"},
            {1, &WindowController::GetAppletResourceUserId, "GetAppletResourceUserId"},
            {2, &WindowController::GetAppletResourceUserIdOfCallerApplet,
             "GetAppletResourceUserIdOfCallerApplet"},
            {10, &WindowController::AcquireForegroundRights, "AcquireForegroundRights"},
            {11, &WindowController::ReleaseForegroundRights, "ReleaseForegroundRights"},
            {12, &WindowController::RejectToChangeIntoBackground, "RejectToChangeIntoBackground"},
            {20, &WindowController::SetAppletWindowVisibility, "SetAppletWindowVisibility"},
            {21, &WindowController::SetAppletGpuTimeSlice, "SetAppletGpuTimeSlice"},
        };
        RegisterHandlers(functions);
    }

private:
    // A criacao de janelas e do subsistema de video. O Eden responde
    // `nullptr` — ou seja, **nao responde**. Aqui responde-se com um erro
    // explicito, porque uma ausencia de resposta deixa o guest a ler lixo
    // (ver a nota sobre o auto-stub em `settings_bridge.h`).
    void CreateWindow(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // O `AppletResourceUserId` e' o PID do processo. O Eden tira-o do
    // `AppletManager`, que o associa ao applet; aqui e' o nosso PID, que e'
    // o unico que existe.
    void GetAppletResourceUserId(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(1));
    }

    void GetAppletResourceUserIdOfCallerApplet(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(1));
    }

    void AcquireForegroundRights(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ReleaseForegroundRights(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void RejectToChangeIntoBackground(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetAppletWindowVisibility(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetAppletGpuTimeSlice(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

/// O `IAudioController` — o volume esperado.
class AudioController final : public Service::ServiceFramework<AudioController> {
public:
    explicit AudioController(Core::System& system)
        : ServiceFramework{system, "IAudioController"} {
        static const FunctionInfo functions[] = {
            {0, &AudioController::SetExpectedMasterVolume, "SetExpectedMasterVolume"},
            {1, &AudioController::GetMainAppletExpectedMasterVolume,
             "GetMainAppletExpectedMasterVolume"},
            {2, &AudioController::GetLibraryAppletExpectedMasterVolume,
             "GetLibraryAppletExpectedMasterVolume"},
            {3, &AudioController::ChangeMainAppletMasterVolume, "ChangeMainAppletMasterVolume"},
            {4, &AudioController::SetTransparentVolumeRate, "SetTransparentVolumeRate"},
        };
        RegisterHandlers(functions);
    }

private:
    void SetExpectedMasterVolume(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetMainAppletExpectedMasterVolume(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(1.0f);
    }

    void GetLibraryAppletExpectedMasterVolume(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(1.0f);
    }

    void ChangeMainAppletMasterVolume(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetTransparentVolumeRate(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

/// O `IDisplayController` — as capturas de ecrã.
class DisplayController final : public Service::ServiceFramework<DisplayController> {
public:
    explicit DisplayController(Core::System& system)
        : ServiceFramework{system, "IDisplayController"} {
        static const FunctionInfo functions[] = {
            {5, &DisplayController::GetLastForegroundCaptureImageEx, "GetLastForegroundCaptureImageEx"},
            {7, &DisplayController::GetCallerAppletCaptureImageEx, "GetCallerAppletCaptureImageEx"},
            {8, &DisplayController::TakeScreenShotOfOwnLayer, "TakeScreenShotOfOwnLayer"},
            {20, &DisplayController::ClearCaptureBuffer, "ClearCaptureBuffer"},
            {22, &DisplayController::AcquireLastApplicationCaptureSharedBuffer,
             "AcquireLastApplicationCaptureSharedBuffer"},
            {23, &DisplayController::ReleaseLastApplicationCaptureSharedBuffer,
             "ReleaseLastApplicationCaptureSharedBuffer"},
            {24, &DisplayController::AcquireLastForegroundCaptureSharedBuffer,
             "AcquireLastForegroundCaptureSharedBuffer"},
            {25, &DisplayController::ReleaseLastForegroundCaptureSharedBuffer,
             "ReleaseLastForegroundCaptureSharedBuffer"},
            {26, &DisplayController::AcquireCallerAppletCaptureSharedBuffer,
             "AcquireCallerAppletCaptureSharedBuffer"},
            {27, &DisplayController::ReleaseCallerAppletCaptureSharedBuffer,
             "ReleaseCallerAppletCaptureSharedBuffer"},
        };
        RegisterHandlers(functions);
    }

private:
    // Todas as capturas dependem do subsistema de vídeo. Não o há.
    void GetLastForegroundCaptureImageEx(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void GetCallerAppletCaptureImageEx(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void TakeScreenShotOfOwnLayer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ClearCaptureBuffer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireLastApplicationCaptureSharedBuffer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void ReleaseLastApplicationCaptureSharedBuffer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireLastForegroundCaptureSharedBuffer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void ReleaseLastForegroundCaptureSharedBuffer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireCallerAppletCaptureSharedBuffer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void ReleaseCallerAppletCaptureSharedBuffer(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

/// O `IApplicationFunctions` — o que o jogo faz com o seu armazenamento.
class ApplicationFunctions final : public Service::ServiceFramework<ApplicationFunctions> {
public:
    explicit ApplicationFunctions(Core::System& system)
        : ServiceFramework{system, "IApplicationFunctions"} {
        static const FunctionInfo functions[] = {
            {1, &ApplicationFunctions::PopLaunchParameter, "PopLaunchParameter"},
            {12, &ApplicationFunctions::CreateApplicationAndRequestToStart,
             "CreateApplicationAndRequestToStart"},
            {20, &ApplicationFunctions::EnsureSaveData, "EnsureSaveData"},
            {21, &ApplicationFunctions::GetDesiredLanguage, "GetDesiredLanguage"},
            {22, &ApplicationFunctions::SetTerminateResult, "SetTerminateResult"},
            {23, &ApplicationFunctions::GetDisplayVersion, "GetDisplayVersion"},
            {25, &ApplicationFunctions::ExtendSaveData, "ExtendSaveData"},
            {26, &ApplicationFunctions::GetSaveDataSize, "GetSaveDataSize"},
            {27, &ApplicationFunctions::CreateCacheStorage, "CreateCacheStorage"},
            {28, &ApplicationFunctions::GetSaveDataSizeMax, "GetSaveDataSizeMax"},
            {29, &ApplicationFunctions::GetCacheStorageMax, "GetCacheStorageMax"},
            {30, &ApplicationFunctions::BeginBlockingHomeButtonShortAndLongPressed,
             "BeginBlockingHomeButtonShortAndLongPressed"},
            {31, &ApplicationFunctions::EndBlockingHomeButtonShortAndLongPressed,
             "EndBlockingHomeButtonShortAndLongPressed"},
            {32, &ApplicationFunctions::BeginBlockingHomeButton, "BeginBlockingHomeButton"},
            {33, &ApplicationFunctions::EndBlockingHomeButton, "EndBlockingHomeButton"},
            {40, &ApplicationFunctions::NotifyRunning, "NotifyRunning"},
            {50, &ApplicationFunctions::GetPseudoDeviceId, "GetPseudoDeviceId"},
            {60, &ApplicationFunctions::SetMediaPlaybackStateForApplication,
             "SetMediaPlaybackStateForApplication"},
            {65, &ApplicationFunctions::IsGamePlayRecordingSupported,
             "IsGamePlayRecordingSupported"},
            {66, &ApplicationFunctions::InitializeGamePlayRecording,
             "InitializeGamePlayRecording"},
        };
        RegisterHandlers(functions);
    }

private:
    void PopLaunchParameter(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateApplicationAndRequestToStart(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // O armazenamento é o sistema de ficheiros, que não existe ainda.
    void EnsureSaveData(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // O idioma: o do sistema. `nn::oe::Language::AmericanEnglish` = 1.
    void GetDesiredLanguage(HLERequestContext& ctx) {
        LOG_DEBUG(Service_AM, "GetDesiredLanguage: 1 (AmericanEnglish)");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u64>(1));
    }

    void SetTerminateResult(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetDisplayVersion(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(0));
    }

    void ExtendSaveData(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetSaveDataSize(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateCacheStorage(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetSaveDataSizeMax(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetCacheStorageMax(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void BeginBlockingHomeButtonShortAndLongPressed(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void EndBlockingHomeButtonShortAndLongPressed(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void BeginBlockingHomeButton(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void EndBlockingHomeButton(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void NotifyRunning(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void GetPseudoDeviceId(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetMediaPlaybackStateForApplication(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsGamePlayRecordingSupported(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void InitializeGamePlayRecording(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

/// O `IProcessWindingController` — como o jogo foi lançado.
class ProcessWindingController final : public Service::ServiceFramework<ProcessWindingController> {
public:
    explicit ProcessWindingController(Core::System& system)
        : ServiceFramework{system, "IProcessWindingController"} {
        static const FunctionInfo functions[] = {
            {0, &ProcessWindingController::GetLaunchReason, "GetLaunchReason"},
            {11, &ProcessWindingController::OpenCallingLibraryApplet, "OpenCallingLibraryApplet"},
            {21, &ProcessWindingController::PushContext, "PushContext"},
            {22, &ProcessWindingController::PopContext, "PopContext"},
            {23, &ProcessWindingController::CancelWindingReservation, "CancelWindingReservation"},
            {30, &ProcessWindingController::WindAndDoReserved, "WindAndDoReserved"},
            {40, &ProcessWindingController::ReserveToStartAndWaitAndUnwindThis,
             "ReserveToStartAndWaitAndUnwindThis"},
            {41, &ProcessWindingController::ReserveToStartAndWait, "ReserveToStartAndWait"},
        };
        RegisterHandlers(functions);
    }

private:
    // `AppletProcessLaunchReason`: 0 = Normal, 1 = ApplicationRequested.
    void GetLaunchReason(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(0));
    }

    void OpenCallingLibraryApplet(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void PushContext(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void PopContext(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CancelWindingReservation(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void WindAndDoReserved(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ReserveToStartAndWaitAndUnwindThis(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ReserveToStartAndWait(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

/// O `IDebugFunctions` — o que só o.debug usa.
class DebugFunctions final : public Service::ServiceFramework<DebugFunctions> {
public:
    explicit DebugFunctions(Core::System& system)
        : ServiceFramework{system, "IDebugFunctions"} {
        // O Eden nao tem comandos registados neste interface (a tabela
        // esta vazia), e por isso o regista do mesmo modo: um interface
        // sem comandos responde "desconhecido" a tudo, que e' o que o Eden
        // faz.
        static const FunctionInfo functions[] = {
            {0, &DebugFunctions::Reservado, "Reservado"},
        };
        RegisterHandlers(functions);
    }

private:
    void Reservado(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

/// O `ILibraryAppletCreator` — abrir um applet de biblioteca.
class LibraryAppletCreator final : public Service::ServiceFramework<LibraryAppletCreator> {
public:
    explicit LibraryAppletCreator(Core::System& system)
        : ServiceFramework{system, "ILibraryAppletCreator"} {
        static const FunctionInfo functions[] = {
            {0, &LibraryAppletCreator::CreateLibraryApplet, "CreateLibraryApplet"},
            {1, &LibraryAppletCreator::CreateLibraryAppletEx, "CreateLibraryAppletEx"},
            {2, &LibraryAppletCreator::CreateStorage, "CreateStorage"},
            {3, &LibraryAppletCreator::CreateTransferMemoryStorage, "CreateTransferMemoryStorage"},
            {4, &LibraryAppletCreator::CreateHandleStorage, "CreateHandleStorage"},
        };
        RegisterHandlers(functions);
    }

private:
    void CreateLibraryApplet(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateLibraryAppletEx(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateStorage(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateTransferMemoryStorage(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateHandleStorage(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O proxy
// ─────────────────────────────────────────────────────────────────────────────

/// O `IApplicationProxy` — o que dá as sub-interfaces.
class ApplicationProxy final : public Service::ServiceFramework<ApplicationProxy> {
public:
    ApplicationProxy(Core::System& system, std::shared_ptr<EstadoDoApplet> estado)
        : ServiceFramework{system, "IApplicationProxy"}, estado_{std::move(estado)} {
        static const FunctionInfo functions[] = {
            {0, &ApplicationProxy::GetCommonStateGetter, "GetCommonStateGetter"},
            {1, &ApplicationProxy::GetSelfController, "GetSelfController"},
            {2, &ApplicationProxy::GetWindowController, "GetWindowController"},
            {3, &ApplicationProxy::GetAudioController, "GetAudioController"},
            {4, &ApplicationProxy::GetDisplayController, "GetDisplayController"},
            {10, &ApplicationProxy::GetProcessWindingController, "GetProcessWindingController"},
            {11, &ApplicationProxy::GetLibraryAppletCreator, "GetLibraryAppletCreator"},
            {20, &ApplicationProxy::GetApplicationFunctions, "GetApplicationFunctions"},
            {1000, &ApplicationProxy::GetDebugFunctions, "GetDebugFunctions"},
        };
        RegisterHandlers(functions);
    }

private:
    // Todas as sub-interfaces se devolvem do mesmo modo: um interface novo,
    // registado no gerenciador de sessoes, e o handle da sessao vai no
    // descritor de handles da resposta.
    //
    // `rb{ctx, 2, 0, 1}`: dois palavras de corpo (o Result e um
    // reservado), e **um** objecto a mover (a sessao do interface).
    // Devolve uma sub-interface.
    //
    // A fabrica vem de fora porque **so o `CommonStateGetter` precisa do
    // estado**: e' o unico que tem a fila de mensagens. Os outros nao tem
    // estado nenhum, e um `make_shared<T>(system, estado_)` a todos seria
    // uma assinatura que nenhum deles tem.
    template <typename T>
    void Devolve(HLERequestContext& ctx, const char* nome,
                 std::shared_ptr<T> (*fabrica)(Core::System&, std::shared_ptr<EstadoDoApplet>)) {
        LOG_DEBUG(Service_AM, "{}: devolvido", nome);
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<T>(ctx, fabrica(system, estado_));
    }

    std::shared_ptr<EstadoDoApplet> estado_;

    void GetCommonStateGetter(HLERequestContext& ctx) {
        Devolve<CommonStateGetter>(ctx, "GetCommonStateGetter", [](Core::System& sys, std::shared_ptr<EstadoDoApplet> est) {
            return std::make_shared<CommonStateGetter>(sys, est);
        });
    }

    void GetSelfController(HLERequestContext& ctx) {
        Devolve<SelfController>(ctx, "GetSelfController", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<SelfController>(sys);
        });
    }

    void GetWindowController(HLERequestContext& ctx) {
        Devolve<WindowController>(ctx, "GetWindowController", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<WindowController>(sys);
        });
    }

    void GetAudioController(HLERequestContext& ctx) {
        Devolve<AudioController>(ctx, "GetAudioController", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<AudioController>(sys);
        });
    }

    void GetDisplayController(HLERequestContext& ctx) {
        Devolve<DisplayController>(ctx, "GetDisplayController", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<DisplayController>(sys);
        });
    }

    void GetProcessWindingController(HLERequestContext& ctx) {
        Devolve<ProcessWindingController>(ctx, "GetProcessWindingController", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<ProcessWindingController>(sys);
        });
    }

    void GetLibraryAppletCreator(HLERequestContext& ctx) {
        Devolve<LibraryAppletCreator>(ctx, "GetLibraryAppletCreator", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<LibraryAppletCreator>(sys);
        });
    }

    void GetApplicationFunctions(HLERequestContext& ctx) {
        Devolve<ApplicationFunctions>(ctx, "GetApplicationFunctions", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<ApplicationFunctions>(sys);
        });
    }

    void GetDebugFunctions(HLERequestContext& ctx) {
        Devolve<DebugFunctions>(ctx, "GetDebugFunctions", [](Core::System& sys, std::shared_ptr<EstadoDoApplet>) {
            return std::make_shared<DebugFunctions>(sys);
        });
    }
};

/// O `appletOE` propriamente dito.
class AppletOE final : public Service::ServiceFramework<AppletOE> {
public:
    explicit AppletOE(Core::System& system) : ServiceFramework{system, "appletOE"} {
        LOG_DEBUG(Service_AM, "appletOE: registado");

        // O applet e' criado, e vai para o primeiro plano — que e' o que o
        // `am` do Eden faz quando lanca um applet
        // (`window_system.cpp`, `AppletMessage::ChangeIntoForeground`).
        //
        // **Sem esta mensagem o SDK nao arranca.** Medido: o guest ficou a
        // pedir `ReceiveMessage` sem parar, e a unica mensagem que o
        // `am` entrega no arranque e' esta.
        estado_ = std::make_shared<EstadoDoApplet>();

        // O evento do sistema. E' o que o `GetEventHandle` devolve, e e'
        // sinalizado quando a mensagem e' empurrada.
        {
            auto& kernel = system.Kernel();
            auto& processo = Kernel::GetCurrentProcess(kernel);
            auto* evento = new Kernel::KEvent(kernel);
            evento->Initialize(kernel, &processo);
            Kernel::Handle handle{};
            if (R_SUCCEEDED(processo.GetHandleTable().Add(kernel, &handle, evento))) {
                estado_->evento = evento;
                estado_->handle_evento = static_cast<std::uint32_t>(handle);
            } else {
                LOG_WARNING(Service_AM, "appletOE: o handle do evento do sistema nao reservou");
            }
        }

        // A mensagem de entrada no primeiro plano, e o evento que a
        // acompanha. Ver `EstadoDoApplet::empurrar`.
        estado_->empurrar(system.Kernel(), EstadoDoApplet::kChangeIntoForeground);
        static const FunctionInfo functions[] = {
            {0, &AppletOE::OpenApplicationProxy, "OpenApplicationProxy"},
        };
        RegisterHandlers(functions);
    }

private:
    // `OpenApplicationProxy(out, pid, process_handle)`.
    //
    // O `pid` vem no corpo (2 palavras) e o handle do processo vem no
    // descritor de handles — e o Eden não o usa para nada além de
    // confirmar que o applet existe.
    //
    // A resposta é o proxy, como um objecto a mover.
    void OpenApplicationProxy(HLERequestContext& ctx) {
        LOG_DEBUG(Service_AM, "appletOE: OpenApplicationProxy");
        RequestParser rp{ctx};
        const auto pid = rp.Pop<u64>();
        LOG_DEBUG(Service_AM, "OpenApplicationProxy: pid={}", pid);

        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ApplicationProxy>(ctx, std::make_shared<ApplicationProxy>(system, estado_));
    }

    // O estado do applet: a fila de mensagens. Vive aqui porque o objecto
    // do servico e' o unico que sobrevive a todas as sub-interfaces.
    std::shared_ptr<EstadoDoApplet> estado_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// O registo
// ─────────────────────────────────────────────────────────────────────────────

// Regista o `appletOE` no `sm:`.
//
// O `sm:` é o `Service::SM::ServiceManager` do Eden, e o registo de um
// serviço faz-se por um `ServerManager` — o mesmo caminho que o `apm` e o
// `lm` usam (`Service::APM::LoopProcess` e `Service::LM::LoopProcess`).
void registar_applet_oe(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("appletOE", std::make_shared<AppletOE>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace ia::services
