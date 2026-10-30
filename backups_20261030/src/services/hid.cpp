// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// O `hid` — o servico de entrada, **nosso**.
//
// ## O que e'
//
// O `hid` (o `IHidServer` do Eden) e' o servico que da ao jogo o estado dos
// comandos. A forma e' sempre a mesma: o jogo cria um `IAppletResource`
// (comando 0), recebe uma **memoria partilhada** com o estado dos comandos
// (`IAppletResource::GetSharedMemoryHandle`) e configura os tipos de comando
// que aceita (`SetSupportedNpadStyleSet`, `ActivateNpad`, ...).
//
// ## Porque este ficheiro existe
//
// **Medido** (`/tmp/boot_font.log`): depois das fontes, o SDK pede
// `GetService("hid")` e a casa nao o tinha. O `sm:` defere o pedido, o guest
// recebe uma resposta de 0 palavras e aborta no leitor CMIF:
//
//     Service: SM: GetService: nome lido = "hid"
//     Service: SM: Server is not registered! service=hid
//     Service: Audio: SetTerminateResult: modulo=10 desc=212
//     [svc] #0x26 Break
//
// O `hid_core/` do Eden (ResourceManager, EmulatedController, AppletResource)
// **nao esta vendorizado** — so' a camada de servico. Por isso a casa
// implementa o servico **fino**: cria o `IAppletResource`, devolve um handle
// de um `KEvent` como "memoria partilhada" (o kernel vendorizado nao tem
// `KSharedMemory`) e responde aos comandos de configuracao com sucesso. O
// estado dos comandos fica a zeros; o `src/input.cpp` ainda nao escreve nele.
//
// **Medido**: com esta versao o jogo passa do `hid` e continua o arranque.
// O que fica por fazer e' o **conteudo** da memoria partilhada (os botoes).
#include "g610/services/hid.hpp"

#include <cstdint>
#include <memory>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"
#include "vendor/eden/core/hle/service/cmif_serialization.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

namespace g610::services {

namespace {

using Service::HLERequestContext;
using Service::Out;
using Service::OutCopyHandle;
using Service::OutInterface;

// O kernel vendorizado nao tem `hid_core/hid_types.h`; estes tipos tem o
// **mesmo tamanho e alinhamento** que os do Eden, que e' o que o CMIF
// serializa. O conteudo nao e' usado.
struct LedPattern {
    std::uint8_t led1{};
    std::uint8_t led2{};
    std::uint8_t led3{};
    std::uint8_t led4{};
};

struct VibrationDeviceInfo {
    std::uint32_t type{};
    std::uint32_t position{};
};

struct VibrationValue {
    float amplitude_low{};
    float frequency_low{};
    float amplitude_high{};
    float frequency_high{};
};

struct SixAxisSensorFusionParameters {
    float parameter1{};
    float parameter2{};
};

// ─────────────────────────────────────────────────────────────────────────────
// IAppletResource — o objecto do comando 0 do `hid`
// ─────────────────────────────────────────────────────────────────────────────

class AppletResource final : public Service::ServiceFramework<AppletResource> {
public:
    AppletResource(Core::System& system, Kernel::KEvent* shm)
        : ServiceFramework{system, "IAppletResource"}, shm_{shm} {
        // A tabela e' a do Eden (`applet_resource.cpp:18-23`).
        static const FunctionInfo functions[] = {
            {0, C<&AppletResource::GetSharedMemoryHandle>, "GetSharedMemoryHandle"},
        };
        RegisterHandlers(functions);
    }

private:
    // **Medido**: o comando 0 e' o unico que o jogo chama neste objecto.
    // O Eden devolve `OutCopyHandle<KSharedMemory>`; a casa devolve um
    // handle de copia para um `KEvent` — a mesma forma que o `pl:u` usa
    // para a fonte (`src/services/pl_u.cpp`). O `svcMapSharedMemory` do
    // guest aceita-o e o conteudo lido e' zeros (a memoria do processo).
    Result GetSharedMemoryHandle(OutCopyHandle<Kernel::KReadableEvent> out_handle) {
        LOG_DEBUG(Service_HID, "IAppletResource::GetSharedMemoryHandle");
        *out_handle = &shm_->GetReadableEvent();
        R_SUCCEED();
    }

    Kernel::KEvent* shm_{};
};

// ─────────────────────────────────────────────────────────────────────────────
// IActiveVibrationDeviceList — o objecto do comando 203 do `hid`
// ─────────────────────────────────────────────────────────────────────────────

class ActiveVibrationDeviceList final
    : public Service::ServiceFramework<ActiveVibrationDeviceList> {
public:
    explicit ActiveVibrationDeviceList(Core::System& system)
        : ServiceFramework{system, "IActiveVibrationDeviceList"} {
        static const FunctionInfo functions[] = {
            {0, C<&ActiveVibrationDeviceList::ActivateVibrationDevice>,
             "ActivateVibrationDevice"},
        };
        RegisterHandlers(functions);
    }

private:
    Result ActivateVibrationDevice() {
        LOG_DEBUG(Service_HID, "IActiveVibrationDeviceList::ActivateVibrationDevice");
        R_SUCCEED();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O servico
// ─────────────────────────────────────────────────────────────────────────────

class Hid final : public Service::ServiceFramework<Hid> {
public:
    explicit Hid(Core::System& system) : ServiceFramework{system, "hid"} {
        // A tabela segue a do Eden (`hid_server.cpp:49-145`). Os comandos
        // que o Eden implementa com estado sao aqui **finos**: respondem
        // sucesso e deixam o estado a zeros. Os comandos que o Eden
        // devolve um handle nulo (`AcquireXpadIdEventHandle`,
        // `GetJoyXpadLifoHandle`) devolvem o mesmo.
        static const FunctionInfo functions[] = {
            {0, C<&Hid::CreateAppletResource>, "CreateAppletResource"},
            {1, C<&Hid::SemEstado>, "ActivateDebugPad"},
            {11, C<&Hid::SemEstado>, "ActivateTouchScreen"},
            {21, C<&Hid::SemEstado>, "ActivateMouse"},
            {26, C<&Hid::SemEstado>, "ActivateDebugMouse"},
            {31, C<&Hid::SemEstado>, "ActivateKeyboard"},
            {32, C<&Hid::SemEstado>, "SendKeyboardLockKeyEvent"},
            {40, C<&Hid::AcquireXpadIdEventHandle>, "AcquireXpadIdEventHandle"},
            {41, C<&Hid::SemEstado>, "ReleaseXpadIdEventHandle"},
            {51, C<&Hid::SemEstado>, "ActivateXpad"},
            {55, C<&Hid::GetXpadIds>, "GetXpadIds"},
            {56, C<&Hid::SemEstado>, "ActivateJoyXpad"},
            {58, C<&Hid::GetJoyXpadLifoHandle>, "GetJoyXpadLifoHandle"},
            {59, C<&Hid::GetJoyXpadIds>, "GetJoyXpadIds"},
            {60, C<&Hid::SemEstado>, "ActivateSixAxisSensor"},
            {61, C<&Hid::SemEstado>, "DeactivateSixAxisSensor"},
            {62, C<&Hid::GetSixAxisSensorLifoHandle>, "GetSixAxisSensorLifoHandle"},
            {63, C<&Hid::SemEstado>, "ActivateJoySixAxisSensor"},
            {64, C<&Hid::SemEstado>, "DeactivateJoySixAxisSensor"},
            {65, C<&Hid::GetJoySixAxisSensorLifoHandle>, "GetJoySixAxisSensorLifoHandle"},
            {66, C<&Hid::SemEstado>, "StartSixAxisSensor"},
            {67, C<&Hid::SemEstado>, "StopSixAxisSensor"},
            {68, C<&Hid::IsSixAxisSensorFusionEnabled>, "IsSixAxisSensorFusionEnabled"},
            {69, C<&Hid::SemEstado>, "EnableSixAxisSensorFusion"},
            {70, C<&Hid::SemEstado>, "SetSixAxisSensorFusionParameters"},
            {71, C<&Hid::GetSixAxisSensorFusionParameters>,
             "GetSixAxisSensorFusionParameters"},
            {72, C<&Hid::SemEstado>, "ResetSixAxisSensorFusionParameters"},
            {79, C<&Hid::SemEstado>, "SetGyroscopeZeroDriftMode"},
            {80, C<&Hid::GetGyroscopeZeroDriftMode>, "GetGyroscopeZeroDriftMode"},
            {81, C<&Hid::SemEstado>, "ResetGyroscopeZeroDriftMode"},
            {82, C<&Hid::IsSixAxisSensorAtRest>, "IsSixAxisSensorAtRest"},
            {83, C<&Hid::IsFirmwareUpdateAvailableForSixAxisSensor>,
             "IsFirmwareUpdateAvailableForSixAxisSensor"},
            {84, C<&Hid::SemEstado>, "EnableSixAxisSensorUnalteredPassthrough"},
            {85, C<&Hid::IsSixAxisSensorUnalteredPassthroughEnabled>,
             "IsSixAxisSensorUnalteredPassthroughEnabled"},
            {87, C<&Hid::SemEstado>, "LoadSixAxisSensorCalibrationParameter"},
            {88, C<&Hid::SemEstado>, "GetSixAxisSensorIcInformation"},
            {89, C<&Hid::SemEstado>, "ResetIsSixAxisSensorDeviceNewlyAssigned"},
            {91, C<&Hid::SemEstado>, "ActivateGesture"},
            {92, C<&Hid::SemEstado>, "SetGestureOutputRanges"},
            {100, C<&Hid::SemEstado>, "SetSupportedNpadStyleSet"},
            {101, C<&Hid::GetSupportedNpadStyleSet>, "GetSupportedNpadStyleSet"},
            {102, C<&Hid::SemEstado>, "SetSupportedNpadIdType"},
            {103, C<&Hid::SemEstado>, "ActivateNpad"},
            {104, C<&Hid::SemEstado>, "DeactivateNpad"},
            {106, C<&Hid::AcquireNpadStyleSetUpdateEventHandle>,
             "AcquireNpadStyleSetUpdateEventHandle"},
            {107, C<&Hid::SemEstado>, "DisconnectNpad"},
            {108, C<&Hid::GetPlayerLedPattern>, "GetPlayerLedPattern"},
            {109, C<&Hid::SemEstado>, "ActivateNpadWithRevision"},
            {120, C<&Hid::SemEstado>, "SetNpadJoyHoldType"},
            {121, C<&Hid::GetNpadJoyHoldType>, "GetNpadJoyHoldType"},
            {122, C<&Hid::SemEstado>, "SetNpadJoyAssignmentModeSingleByDefault"},
            {123, C<&Hid::SemEstado>, "SetNpadJoyAssignmentModeSingle"},
            {124, C<&Hid::SemEstado>, "SetNpadJoyAssignmentModeDual"},
            {125, C<&Hid::SemEstado>, "MergeSingleJoyAsDualJoy"},
            {126, C<&Hid::SemEstado>, "StartLrAssignmentMode"},
            {127, C<&Hid::SemEstado>, "StopLrAssignmentMode"},
            {128, C<&Hid::SemEstado>, "SetNpadHandheldActivationMode"},
            {130, C<&Hid::SemEstado>, "SwapNpadAssignment"},
            {132, C<&Hid::SemEstado>, "EnableUnintendedHomeButtonInputProtection"},
            {133, C<&Hid::SemEstado>, "SetNpadJoyAssignmentModeSingleWithDestination"},
            {134, C<&Hid::SemEstado>, "SetNpadAnalogStickUseCenterClamp"},
            {135, C<&Hid::SemEstado>, "SetNpadCaptureButtonAssignment"},
            {136, C<&Hid::SemEstado>, "ClearNpadCaptureButtonAssignment"},
            {200, C<&Hid::GetVibrationDeviceInfo>, "GetVibrationDeviceInfo"},
            {201, C<&Hid::SemEstado>, "SendVibrationValue"},
            {202, C<&Hid::GetActualVibrationValue>, "GetActualVibrationValue"},
            {203, C<&Hid::CreateActiveVibrationDeviceList>,
             "CreateActiveVibrationDeviceList"},
            {204, C<&Hid::SemEstado>, "PermitVibration"},
            {205, C<&Hid::IsVibrationPermitted>, "IsVibrationPermitted"},
            {206, C<&Hid::SemEstado>, "SendVibrationValues"},
            {207, C<&Hid::SemEstado>, "SendVibrationGcErmCommand"},
            {211, C<&Hid::IsVibrationDeviceMounted>, "IsVibrationDeviceMounted"},
            {212, C<&Hid::SemEstado>, "SendVibrationValueInBool"},
            {300, C<&Hid::SemEstado>, "ActivateConsoleSixAxisSensor"},
            {301, C<&Hid::SemEstado>, "StartConsoleSixAxisSensor"},
            {302, C<&Hid::SemEstado>, "StopConsoleSixAxisSensor"},
            {303, C<&Hid::SemEstado>, "ActivateSevenSixAxisSensor"},
            {304, C<&Hid::SemEstado>, "StartSevenSixAxisSensor"},
            {305, C<&Hid::SemEstado>, "StopSevenSixAxisSensor"},
            {306, C<&Hid::SemEstado>, "InitializeSevenSixAxisSensor"},
            {307, C<&Hid::SemEstado>, "FinalizeSevenSixAxisSensor"},
            {310, C<&Hid::SemEstado>, "ResetSevenSixAxisSensorTimestamp"},
        };
        RegisterHandlers(functions);

        // O objecto por tras do `GetSharedMemoryHandle`. O kernel
        // vendorizado nao tem `KSharedMemory`; o guest recebe um handle
        // de copia de um `KEvent` e o `svcMapSharedMemory` aceita-o. O
        // conteudo e' zero — o guest le os botoes como "nenhum".
        auto& kernel = system.Kernel();
        auto& processo = Kernel::GetCurrentProcess(kernel);
        shm_ = new Kernel::KEvent(kernel);
        shm_->Initialize(kernel, &processo);

        // O evento do `AcquireNpadStyleSetUpdateEventHandle`. O Eden
        // sinaliza-o quando o conjunto de estilos muda; aqui ele nasce
        // sinalizado para um primeiro `WaitSynchronization` nao prender.
        style_event_ = new Kernel::KEvent(kernel);
        style_event_->Initialize(kernel, &processo);
        style_event_->Signal(kernel);
    }

    ~Hid() override {
        auto& kernel = system.Kernel();
        if (shm_ != nullptr) {
            shm_->Close(kernel);
            delete shm_;
        }
        if (style_event_ != nullptr) {
            style_event_->Close(kernel);
            delete style_event_;
        }
    }

private:
    // Comandos sem estado: o Eden devolve sucesso e a casa tambem. A
    // assinatura sem argumentos faz o CMIF escrever **so'** o Result —
    // que e' a resposta destes comandos.
    Result SemEstado() {
        R_SUCCEED();
    }

    Result CreateAppletResource(OutInterface<AppletResource> out_interface) {
        LOG_DEBUG(Service_HID, "CreateAppletResource");
        *out_interface = std::make_shared<AppletResource>(system, shm_);
        R_SUCCEED();
    }

    Result AcquireXpadIdEventHandle(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_HID, "AcquireXpadIdEventHandle (handle nulo, como o Eden)");
        *out_event = nullptr;
        R_SUCCEED();
    }

    Result GetJoyXpadLifoHandle(OutCopyHandle<Kernel::KReadableEvent> out_handle) {
        LOG_DEBUG(Service_HID, "GetJoyXpadLifoHandle (handle nulo, como o Eden)");
        *out_handle = nullptr;
        R_SUCCEED();
    }

    Result GetSixAxisSensorLifoHandle(OutCopyHandle<Kernel::KReadableEvent> out_handle) {
        LOG_DEBUG(Service_HID, "GetSixAxisSensorLifoHandle (handle nulo, como o Eden)");
        *out_handle = nullptr;
        R_SUCCEED();
    }

    Result GetJoySixAxisSensorLifoHandle(OutCopyHandle<Kernel::KReadableEvent> out_handle) {
        LOG_DEBUG(Service_HID, "GetJoySixAxisSensorLifoHandle (handle nulo, como o Eden)");
        *out_handle = nullptr;
        R_SUCCEED();
    }

    Result GetXpadIds(Out<std::uint64_t> out_count) {
        LOG_DEBUG(Service_HID, "GetXpadIds");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetJoyXpadIds(Out<std::int64_t> out_count) {
        LOG_DEBUG(Service_HID, "GetJoyXpadIds");
        *out_count = 0;
        R_SUCCEED();
    }

    Result IsSixAxisSensorFusionEnabled(Out<bool> out_enabled) {
        LOG_DEBUG(Service_HID, "IsSixAxisSensorFusionEnabled");
        *out_enabled = false;
        R_SUCCEED();
    }

    Result GetSixAxisSensorFusionParameters(
        Out<SixAxisSensorFusionParameters> out_parameters) {
        LOG_DEBUG(Service_HID, "GetSixAxisSensorFusionParameters");
        *out_parameters = {};
        R_SUCCEED();
    }

    Result GetGyroscopeZeroDriftMode(Out<std::uint32_t> out_mode) {
        LOG_DEBUG(Service_HID, "GetGyroscopeZeroDriftMode");
        *out_mode = 0;
        R_SUCCEED();
    }

    Result IsSixAxisSensorAtRest(Out<bool> out_at_rest) {
        LOG_DEBUG(Service_HID, "IsSixAxisSensorAtRest");
        *out_at_rest = true;
        R_SUCCEED();
    }

    Result IsFirmwareUpdateAvailableForSixAxisSensor(Out<bool> out_available) {
        LOG_DEBUG(Service_HID, "IsFirmwareUpdateAvailableForSixAxisSensor");
        *out_available = false;
        R_SUCCEED();
    }

    Result IsSixAxisSensorUnalteredPassthroughEnabled(Out<bool> out_enabled) {
        LOG_DEBUG(Service_HID, "IsSixAxisSensorUnalteredPassthroughEnabled");
        *out_enabled = false;
        R_SUCCEED();
    }

    // `0x2` e' o `NpadStyleTag::Handheld` (o Eden usa o mesmo bit).
    Result GetSupportedNpadStyleSet(Out<std::uint32_t> out_style_set) {
        LOG_DEBUG(Service_HID, "GetSupportedNpadStyleSet -> Handheld");
        *out_style_set = 0x2;
        R_SUCCEED();
    }

    Result AcquireNpadStyleSetUpdateEventHandle(
        OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_HID, "AcquireNpadStyleSetUpdateEventHandle");
        style_event_->Signal(system.Kernel());
        *out_event = &style_event_->GetReadableEvent();
        R_SUCCEED();
    }

    Result GetPlayerLedPattern(Out<LedPattern> out_pattern) {
        LOG_DEBUG(Service_HID, "GetPlayerLedPattern");
        *out_pattern = LedPattern{1, 0, 0, 0};
        R_SUCCEED();
    }

    Result GetNpadJoyHoldType(Out<std::uint64_t> out_hold_type) {
        LOG_DEBUG(Service_HID, "GetNpadJoyHoldType -> Vertical");
        *out_hold_type = 0;
        R_SUCCEED();
    }

    Result GetVibrationDeviceInfo(Out<VibrationDeviceInfo> out_info) {
        LOG_DEBUG(Service_HID, "GetVibrationDeviceInfo");
        *out_info = {};
        R_SUCCEED();
    }

    Result GetActualVibrationValue(Out<VibrationValue> out_value) {
        LOG_DEBUG(Service_HID, "GetActualVibrationValue");
        *out_value = {};
        R_SUCCEED();
    }

    Result CreateActiveVibrationDeviceList(
        OutInterface<ActiveVibrationDeviceList> out_interface) {
        LOG_DEBUG(Service_HID, "CreateActiveVibrationDeviceList");
        *out_interface = std::make_shared<ActiveVibrationDeviceList>(system);
        R_SUCCEED();
    }

    Result IsVibrationPermitted(Out<bool> out_permitted) {
        LOG_DEBUG(Service_HID, "IsVibrationPermitted");
        *out_permitted = true;
        R_SUCCEED();
    }

    Result IsVibrationDeviceMounted(Out<bool> out_mounted) {
        LOG_DEBUG(Service_HID, "IsVibrationDeviceMounted");
        *out_mounted = false;
        R_SUCCEED();
    }

    Kernel::KEvent* shm_{};
    Kernel::KEvent* style_event_{};
};

} // namespace

void registar_hid(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("hid", std::make_shared<Hid>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
