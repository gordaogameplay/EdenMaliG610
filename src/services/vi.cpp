// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O `vi` — o serviço de **vídeo** (o display) — **nosso**, fino.
//
// ## O que é
//
// O `vi` é o serviço que dá ao jogo a **tela**: as camadas (*layers*),
// os *stacks*, a resolução, o vsync. O SDK pede-o ao `sm:` no arranque
// (o nome que o jogo pediu foi **"vi:m"** — o serviço do *manager*).
//
// São três serviços, e o Eden regista-os com três nomes
// (`core/hle/service/vi/vi.cpp`, `LoopProcess`):
//
//   - "vi:m" — o **manager** (o lado do sistema);
//   - "vi:s" — o **sistema**;
//   - "vi:u" — a **aplicação**.
//
// Cada um devolve, com um `GetDisplayService`, o seu display — e cada
// display tem a **sua** tabela de comandos.
//
// ## Porque este ficheiro existe
//
// **Medido** (`boot_p7d.log`): o guest pede `GetService("vi:m")` e, como
// o serviço não está registado, o `sm:` **defere** o pedido — e o
// deferimento, no motor nativo, responde com **0 palavras**, o que mata o
// guest (o reader CMIF aborta com `modulo=10 desc=212`).
//
// ## A forma é do Eden, à letra
//
// As tabelas de comandos são as do upstream
// (`core/hle/service/vi/manager_root_service.cpp`,
// `system_root_service.cpp`, `application_root_service.cpp`, e os
// `*_display_service.cpp` correspondentes), com os **IDs** e os **nomes**.
//
// ## O conteúdo é o mínimo
//
// O display do Eden (`container.cpp`, `nvnflinger/`) gere as camadas
// reais e o *compositor*. Aqui o que há é a **interface**: os comandos
// existem, respondem com a forma certa, e os **identificadores** que o
// guest precisa (o id do display, o id da camada) são reais.
//
// **A tela em si é a da casa** — o caminho de render (Wayland/SDL2 com o
// panvk) — e não o do Eden. Ligar os dois é o passo seguinte.
//
// SPDX-FileCopyrightText: 2018 Yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

namespace g610::services {

namespace {

using Service::HLERequestContext;

using IPC::RequestParser;
using IPC::ResponseBuilder;

// ─────────────────────────────────────────────────────────────────────────────
// O estado do display.
//
// Os **identificadores** que o guest recebe (o id do display, o id da
// camada) são reais e estáveis — o guest usa-os nos comandos seguintes,
// e um id que mudasse a cada chamada seria um id inútil.
// ─────────────────────────────────────────────────────────────────────────────
struct EstadoDoDisplay {
    std::uint64_t proximo_display = 1;
    std::uint64_t proxima_camada = 1;
    // A resolução: a do Switch (1280x720). É o que o Eden devolve
    // para o display interno.
    std::int64_t largura = 1280;
    std::int64_t altura = 720;
    // O **binder** que o jogo recebe: um identificador por camada. É
    // por ele que o jogo passa os seus buffers ao compositor.
    std::uint64_t proximo_binder = 1;
    // A **janela** de cada camada: o buffer onde o jogo desenha.
    struct Janela {
        std::uint64_t endereco = 0;
        std::uint64_t tamanho = 0;
    };
    std::map<std::uint64_t, Janela> janelas;
};

// O estado é partilhado pelos três displays (eles falam do **mesmo**
// display) — como no Eden, onde o `Container` é comum aos três.
EstadoDoDisplay& Estado() {
    static EstadoDoDisplay e;
    return e;
}

// ─────────────────────────────────────────────────────────────────────────────
// A base comum dos três displays.
//
// Os handlers são **de instância** (e não estáticos) porque o evento de
// vsync é um objecto — e o `KEvent` da casa vive na sessão, como o do
// `applet_oe`.
// ─────────────────────────────────────────────────────────────────────────────
class DisplayBase {
protected:
    explicit DisplayBase(Core::System& system) : system_{system} {}

    // 1102: a resolução do display. `Out<s64>` × 2, mais o
    // `display_id` de entrada.
    void GetDisplayResolution(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o display_id
        ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(Estado().largura);
        rb.Push<std::int64_t>(Estado().altura);
    }

    // 1010: abrir um display pelo nome. Há um só display, por isso o
    // qualquer nome serve.
    void OpenDisplay(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(Estado().proximo_display++);
    }

    // 1011: abrir o display por omissão.
    void OpenDefaultDisplay(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(Estado().proximo_display++);
    }

    // 1000: a lista de displays. `Out<u64>` (o número) e o array.
    void ListDisplays(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(1);  // há um display
    }

    // 1101: ligar/desligar o display.
    void SetDisplayEnabled(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint32_t>();  // o estado
        (void)rp.PopRaw<std::uint64_t>();  // o display_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2010: criar uma camada gerida. `Out<u64>` (o id da camada).
    void CreateManagedLayer(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint32_t>();  // as flags
        (void)rp.PopRaw<std::uint64_t>();  // o display_id
        (void)rp.PopRaw<std::uint32_t>();  // o aruid
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(Estado().proxima_camada++);
    }

    // 6000: pôr a camada num stack.
    void AddToLayerStack(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint32_t>();  // o stack_id
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 6002: a visibilidade da camada.
    void SetLayerVisibility(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint8_t>();   // o visible
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2011: destruir a camada.
    void DestroyManagedLayer(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 5202: o evento de vsync. É um **handle de cópia** (o Eden devolve o
    // `KReadableEvent`).
    void GetDisplayVsyncEvent(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o display_id

        if (evento_ == nullptr) {
            auto& kernel = system_.Kernel();
            auto& processo = Kernel::GetCurrentProcess(kernel);
            evento_ = new Kernel::KEvent(kernel);
            evento_->Initialize(kernel, &processo);
        }

        ResponseBuilder rb{ctx, 3, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_->GetReadableEvent());
        rb.Push(ResultSuccess);
    }

    Core::System& system_;
    Kernel::KEvent* evento_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// O *parcel* — o formato do Eden (`nvnflinger/parcel.h`).
//
// O Eden serializa um `NativeWindow` num "parcel" Android:
//
//   - o **cabeçalho** (`ParcelHeader`, 16 bytes): o tamanho e o offset dos
//     dados, e o tamanho e o offset dos objectos;
//   - os **dados**: o `NativeWindow` (40 bytes);
//   - os **objectos**: um `u32` a zero (o `WriteInterface` do Eden
//     escreve `0U` no buffer de objectos).
//
// A NativeWindow (`vi_types.h`, 0x28 bytes):
//
//   u32 magic = 2;  u32 process_id = 1;  u64 id;  (2 palavras de padding)
//   u8 dispdrv[8] = "dispdrv\0";          (2 palavras de padding)
//
// **O `id` é o binder** — é por ele que o jogo passa os seus buffers ao
// compositor.
// ─────────────────────────────────────────────────────────────────────────────
struct NativeWindow {
    std::uint32_t magic = 2;
    std::uint32_t process_id = 1;
    std::uint64_t id = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
    char dispdrv[8] = {'d', 'i', 's', 'p', 'd', 'r', 'v', '\0'};
    std::uint32_t padding2 = 0;
    std::uint32_t padding3 = 0;
};
static_assert(sizeof(NativeWindow) == 0x28, "tamanho errado");

// O cabeçalho do parcel (o Eden: `ParcelHeader`).
struct ParcelHeader {
    std::uint32_t data_size;
    std::uint32_t data_offset;
    std::uint32_t objects_size;
    std::uint32_t objects_offset;
};
static_assert(sizeof(ParcelHeader) == 16, "tamanho errado");

// Serializa uma `NativeWindow` num parcel — a forma do Eden
// (`OutputParcel::WriteInterface` + `Serialize`).
std::vector<std::uint8_t> SerializarNativeWindow(const NativeWindow& janela) {
    std::vector<std::uint8_t> dados(sizeof(NativeWindow), 0);
    std::memcpy(dados.data(), &janela, sizeof(janela));

    const std::uint32_t object_size = 4;  // o `u32` a zero do `WriteInterface`
    std::vector<std::uint8_t> objectos(object_size, 0);

    std::vector<std::uint8_t> parcel(sizeof(ParcelHeader) + dados.size() + objectos.size(), 0);
    ParcelHeader cabecalho{};
    cabecalho.data_size = static_cast<std::uint32_t>(dados.size());
    cabecalho.data_offset = sizeof(ParcelHeader);
    cabecalho.objects_size = static_cast<std::uint32_t>(objectos.size());
    cabecalho.objects_offset = cabecalho.data_offset + cabecalho.data_size;
    std::memcpy(parcel.data(), &cabecalho, sizeof(cabecalho));
    std::memcpy(parcel.data() + cabecalho.data_offset, dados.data(), dados.size());
    std::memcpy(parcel.data() + cabecalho.objects_offset, objectos.data(), objectos.size());
    return parcel;
}

// ─────────────────────────────────────────────────────────────────────────────
// O *binder* (o "relay service") — o serviço de que o display devolve o
// "driver".
//
// O Eden (`nvnflinger/hos_binder_driver.cpp`) tem quatro comandos; a
// **forma** é o que importa aqui. O `TransactParcel` é o protocolo do
// *binder* (o "parcel" do Android) — o Eden responde com o que o
// *surface flinger* produz, e nós não temos *surface flinger*.
//
// **O que se responde, e porquê:** o `parcel` de resposta é um bloco de
// zeros. Não é a resposta certa, mas é uma resposta **com a forma
// certa** — e o que matou o jogo foi exactamente o contrário: um
// comando que devolve uma interface respondido com "sucesso e zeros",
// que o guest leu como um ponteiro nulo e desreferenciou (medido:
// `boot_p7e.log`, o salto para `0x837982bc`, no vão entre módulos).
// ─────────────────────────────────────────────────────────────────────────────
class HosBinderDriver final : public Service::ServiceFramework<HosBinderDriver> {
public:
    explicit HosBinderDriver(Core::System& system)
        : ServiceFramework{system, "IHOSBinderDriver"}, system_{system} {
        static const FunctionInfo functions[] = {
            {0, &HosBinderDriver::TransactParcel, "TransactParcel"},
            {1, &HosBinderDriver::AdjustRefcount, "AdjustRefcount"},
            {2, &HosBinderDriver::GetNativeHandle, "GetNativeHandle"},
            {3, &HosBinderDriver::TransactParcelAuto, "TransactParcelAuto"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0 e 3: transaccionar um *parcel*. A resposta é o *parcel* de
    // volta — um bloco de zeros, com o tamanho que o guest pediu.
    void TransactParcel(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const std::int32_t binder_id = rp.PopRaw<std::int32_t>();
        const std::uint32_t transacao = rp.PopRaw<std::uint32_t>();

        // O parcel de entrada: um buffer de entrada (o `InBuffer` do
        // Eden). A resposta vai para o buffer de saída (índice 0).
        const auto entrada = ctx.ReadBufferCopy(0);

        // O cabeçalho do parcel (`ParcelHeader`, 16 bytes).
        ParcelHeader cabecalho{};
        if (entrada.size() >= sizeof(cabecalho))
            std::memcpy(&cabecalho, entrada.data(), sizeof(cabecalho));

        LOG_DEBUG(Service_NVDRV,
                  "binder: TransactParcel id={} transacao={} parcel={} bytes "
                  "(dados={}@{}, objectos={}@{})",
                  binder_id, transacao, entrada.size(), cabecalho.data_size,
                  cabecalho.data_offset, cabecalho.objects_size, cabecalho.objects_offset);

        // Os primeiros bytes do parcel, em hexadecimal: é o que permite
        // decodificar o formato **real** em vez de o adivinhar.
        {
            // **Os limites são contados.** Um `snprintf` que devolve o
            // tamanho *que teria escrito* pode passar do fim do tampão, e
            // o `sizeof(linha) - n` deixa de ser um tamanho: vira um
            // número gigante e o motor escreve fora do tampão. Foi o que
            // aconteceu (o processo morreu no fim do arranque).
            char linha[320];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[binder] parcel:"));
            for (std::size_t i = 0; i < entrada.size() && i < 48 && n + 4 < sizeof(linha); ++i)
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " %02x", entrada[i]));
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        // A resposta: um parcel em branco — o cabeçalho, sem dados.
        std::vector<std::uint8_t> reply(sizeof(ParcelHeader), 0);
        ParcelHeader cab{};
        cab.data_size = 0;
        cab.data_offset = sizeof(ParcelHeader);
        cab.objects_size = 0;
        cab.objects_offset = sizeof(ParcelHeader);
        std::memcpy(reply.data(), &cab, sizeof(cab));

        const std::size_t tamanho = ctx.GetWriteBufferSize(0);
        if (tamanho > 0) {
            const std::size_t n = std::min(tamanho, reply.size());
            ctx.WriteBuffer(reply.data(), n, 0);
        }
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 3: o `TransactParcel` "auto" — a mesma transacção, com o
    // descritor de buffer automático (o Eden reusa o handler do 0).
    void TransactParcelAuto(HLERequestContext& ctx) {
        TransactParcel(ctx);
    }

    // 1: o contador de referências do *binder*.
    void AdjustRefcount(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::int32_t>();  // o binder_id
        (void)rp.PopRaw<std::int32_t>();  // o addval
        (void)rp.PopRaw<std::int32_t>();  // o type
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2: o handle nativo de um *binder* — o Eden devolve um
    // `OutCopyHandle<KReadableEvent>`.
    void GetNativeHandle(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::int32_t>();  // o binder_id
        (void)rp.PopRaw<std::uint32_t>(); // o type_id

        if (evento_ == nullptr) {
            auto& kernel = system_.Kernel();
            auto& processo = Kernel::GetCurrentProcess(kernel);
            evento_ = new Kernel::KEvent(kernel);
            evento_->Initialize(kernel, &processo);
        }

        ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_->GetReadableEvent());
    }

    Core::System& system_;
    Kernel::KEvent* evento_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Os três displays, com as tabelas do upstream.
//
// Os IDs são o espaço de IDs do display: o **mesmo** ID em displays
// diferentes é uma coisa diferente (o 2010 do manager é "criar camada
// gerida"; o 2010 da aplicação não existe). Por isso são três classes.
// ─────────────────────────────────────────────────────────────────────────────
class ViDisplayManager final : public Service::ServiceFramework<ViDisplayManager>,
                               public DisplayBase {
public:
    explicit ViDisplayManager(Core::System& system)
        : ServiceFramework{system, "IManagerDisplayService"}, DisplayBase{system} {
        static const FunctionInfo functions[] = {
            {200, nullptr, "AllocateProcessHeapBlock"},
            {201, nullptr, "FreeProcessHeapBlock"},
            {1102, &ViDisplayManager::GetDisplayResolution, "GetDisplayResolution"},
            {2010, &ViDisplayManager::CreateManagedLayer, "CreateManagedLayer"},
            {2011, &ViDisplayManager::DestroyManagedLayer, "DestroyManagedLayer"},
            {2012, nullptr, "CreateStrayLayer"},
            {2050, nullptr, "CreateIndirectLayer"},
            {2051, nullptr, "DestroyIndirectLayer"},
            {2052, nullptr, "CreateIndirectProducerEndPoint"},
            {2053, nullptr, "DestroyIndirectProducerEndPoint"},
            {2054, nullptr, "CreateIndirectConsumerEndPoint"},
            {2055, nullptr, "DestroyIndirectConsumerEndPoint"},
            {2060, nullptr, "CreateWatermarkCompositor"},
            {2062, nullptr, "SetWatermarkText"},
            {2063, nullptr, "SetWatermarkLayerStacks"},
            {2300, nullptr, "AcquireLayerTexturePresentingEvent"},
            {2301, nullptr, "ReleaseLayerTexturePresentingEvent"},
            {2302, nullptr, "GetDisplayHotplugEvent"},
            {2303, nullptr, "GetDisplayModeChangedEvent"},
            {2402, nullptr, "GetDisplayHotplugState"},
            {2501, nullptr, "GetCompositorErrorInfo"},
            {2601, nullptr, "GetDisplayErrorEvent"},
            {2701, nullptr, "GetDisplayFatalErrorEvent"},
            {4201, nullptr, "SetDisplayAlpha"},
            {4203, nullptr, "SetDisplayLayerStack"},
            {4205, nullptr, "SetDisplayPowerState"},
            {4206, nullptr, "SetDefaultDisplay"},
            {4207, nullptr, "ResetDisplayPanel"},
            {4208, nullptr, "SetDisplayFatalErrorEnabled"},
            {4209, nullptr, "IsDisplayPanelOn"},
            {4300, nullptr, "GetInternalPanelId"},
            {6000, &ViDisplayManager::AddToLayerStack, "AddToLayerStack"},
            {6001, nullptr, "RemoveFromLayerStack"},
            {6002, &ViDisplayManager::SetLayerVisibility, "SetLayerVisibility"},
            {6003, nullptr, "SetLayerConfig"},
            {6004, nullptr, "AttachLayerPresentationTracer"},
            {6005, nullptr, "DetachLayerPresentationTracer"},
            {6006, nullptr, "StartLayerPresentationRecording"},
            {6007, nullptr, "StopLayerPresentationRecording"},
            {6008, nullptr, "StartLayerPresentationFenceWait"},
            {6009, nullptr, "StopLayerPresentationFenceWait"},
            {6010, nullptr, "GetLayerPresentationAllFencesExpiredEvent"},
            {6011, nullptr, "EnableLayerAutoClearTransitionBuffer"},
            {6012, nullptr, "DisableLayerAutoClearTransitionBuffer"},
            {6013, nullptr, "SetLayerOpacity"},
            {6014, nullptr, "AttachLayerWatermarkCompositor"},
            {6015, nullptr, "DetachLayerWatermarkCompositor"},
            {7000, nullptr, "SetContentVisibility"},
            {8000, nullptr, "SetConductorLayer"},
            {8001, nullptr, "SetTimestampTracking"},
            {8100, nullptr, "SetIndirectProducerFlipOffset"},
            {8200, nullptr, "CreateSharedBufferStaticStorage"},
            {8201, nullptr, "CreateSharedBufferTransferMemory"},
            {8202, nullptr, "DestroySharedBuffer"},
            {8203, nullptr, "BindSharedLowLevelLayerToManagedLayer"},
            {8204, nullptr, "BindSharedLowLevelLayerToIndirectLayer"},
            {8207, nullptr, "UnbindSharedLowLevelLayer"},
            {8208, nullptr, "ConnectSharedLowLevelLayerToSharedBuffer"},
            {8209, nullptr, "DisconnectSharedLowLevelLayerFromSharedBuffer"},
            {8210, nullptr, "CreateSharedLayer"},
            {8211, nullptr, "DestroySharedLayer"},
            {8216, nullptr, "AttachSharedLayerToLowLevelLayer"},
            {8217, nullptr, "ForceDetachSharedLayerFromLowLevelLayer"},
            {8218, nullptr, "StartDetachSharedLayerFromLowLevelLayer"},
            {8219, nullptr, "FinishDetachSharedLayerFromLowLevelLayer"},
            {8220, nullptr, "GetSharedLayerDetachReadyEvent"},
            {8221, nullptr, "GetSharedLowLevelLayerSynchronizedEvent"},
            {8222, nullptr, "CheckSharedLowLevelLayerSynchronized"},
            {8223, nullptr, "RegisterSharedBufferImporterAruid"},
            {8224, nullptr, "UnregisterSharedBufferImporterAruid"},
            {8227, nullptr, "CreateSharedBufferProcessHeap"},
            {8228, nullptr, "GetSharedLayerLayerStacks"},
            {8229, nullptr, "SetSharedLayerLayerStacks"},
            {8291, nullptr, "PresentDetachedSharedFrameBufferToLowLevelLayer"},
            {8292, nullptr, "FillDetachedSharedFrameBufferColor"},
            {8293, nullptr, "GetDetachedSharedFrameBufferImage"},
            {8294, nullptr, "SetDetachedSharedFrameBufferImage"},
            {8295, nullptr, "CopyDetachedSharedFrameBufferImage"},
            {8296, nullptr, "SetDetachedSharedFrameBufferSubImage"},
            {8297, nullptr, "GetSharedFrameBufferContentParameter"},
            {8298, nullptr, "ExpandStartupLogoOnSharedFrameBuffer"},
        };
        RegisterHandlers(functions);
    }
};

class ViDisplaySystem final : public Service::ServiceFramework<ViDisplaySystem>,
                              public DisplayBase {
public:
    explicit ViDisplaySystem(Core::System& system)
        : ServiceFramework{system, "ISystemDisplayService"}, DisplayBase{system} {
        static const FunctionInfo functions[] = {
            {1200, nullptr, "GetZOrderCountMin"},
            {1202, nullptr, "GetZOrderCountMax"},
            {1203, nullptr, "GetDisplayLogicalResolution"},
            {1204, nullptr, "SetDisplayMagnification"},
            {2201, nullptr, "SetLayerPosition"},
            {2203, nullptr, "SetLayerSize"},
            {2204, nullptr, "GetLayerZ"},
            {2205, nullptr, "SetLayerZ"},
            {2207, nullptr, "SetLayerVisibility"},
            {2209, nullptr, "SetLayerAlpha"},
            {2210, nullptr, "SetLayerPositionAndSize"},
            {2312, nullptr, "CreateStrayLayer"},
            {2400, nullptr, "OpenIndirectLayer"},
            {2401, nullptr, "CloseIndirectLayer"},
            {2402, nullptr, "FlipIndirectLayer"},
            {3000, nullptr, "ListDisplayModes"},
            {3001, nullptr, "ListDisplayRgbRanges"},
            {3002, nullptr, "ListDisplayContentTypes"},
            {3200, nullptr, "GetDisplayMode"},
            {3201, nullptr, "SetDisplayMode"},
            {3202, nullptr, "GetDisplayUnderscan"},
            {3203, nullptr, "SetDisplayUnderscan"},
            {3204, nullptr, "GetDisplayContentType"},
            {3205, nullptr, "SetDisplayContentType"},
            {3206, nullptr, "GetDisplayRgbRange"},
            {3207, nullptr, "SetDisplayRgbRange"},
            {3208, nullptr, "GetDisplayCmuMode"},
            {3209, nullptr, "SetDisplayCmuMode"},
            {3210, nullptr, "GetDisplayContrastRatio"},
            {3211, nullptr, "SetDisplayContrastRatio"},
            {3214, nullptr, "GetDisplayGamma"},
            {3215, nullptr, "SetDisplayGamma"},
            {3216, nullptr, "GetDisplayCmuLuma"},
            {3217, nullptr, "SetDisplayCmuLuma"},
            {3218, nullptr, "SetDisplayCrcMode"},
            {6013, nullptr, "GetLayerPresentationSubmissionTimestamps"},
            {8225, nullptr, "GetSharedBufferMemoryHandleId"},
            {8250, nullptr, "OpenSharedLayer"},
            {8251, nullptr, "CloseSharedLayer"},
        };
        RegisterHandlers(functions);
    }
};

class ViDisplayApplication final : public Service::ServiceFramework<ViDisplayApplication>,
                                   public DisplayBase {
public:
    explicit ViDisplayApplication(Core::System& system)
        : ServiceFramework{system, "IApplicationDisplayService"}, DisplayBase{system} {
        static const FunctionInfo functions[] = {
            {100, &ViDisplayApplication::GetRelayService, "GetRelayService"},
            {101, &ViDisplayApplication::GetSystemDisplayService, "GetSystemDisplayService"},
            {102, &ViDisplayApplication::GetManagerDisplayService, "GetManagerDisplayService"},
            {103, &ViDisplayApplication::GetIndirectDisplayTransactionService,
             "GetIndirectDisplayTransactionService"},
            {1000, &ViDisplayApplication::ListDisplays, "ListDisplays"},
            {1010, &ViDisplayApplication::OpenDisplay, "OpenDisplay"},
            {1011, &ViDisplayApplication::OpenDefaultDisplay, "OpenDefaultDisplay"},
            {1020, nullptr, "CloseDisplay"},
            {1101, &ViDisplayApplication::SetDisplayEnabled, "SetDisplayEnabled"},
            {1102, &ViDisplayApplication::GetDisplayResolution, "GetDisplayResolution"},
            {2020, &ViDisplayApplication::OpenLayer, "OpenLayer"},
            {2021, nullptr, "CloseLayer"},
            {2030, nullptr, "CreateStrayLayer"},
            {2031, nullptr, "DestroyStrayLayer"},
            {2101, nullptr, "SetLayerScalingMode"},
            {2102, nullptr, "ConvertScalingMode"},
            {2450, nullptr, "GetIndirectLayerImageMap"},
            {2451, nullptr, "GetIndirectLayerImageCropMap"},
            {2460, nullptr, "GetIndirectLayerImageRequiredMemoryInfo"},
            {5202, &ViDisplayApplication::GetDisplayVsyncEvent, "GetDisplayVsyncEvent"},
            {5203, nullptr, "GetDisplayVsyncEventForDebug"},
        };
        RegisterHandlers(functions);
    }

private:
    // 2020: abrir uma camada — e devolver a **native window**, que é o
    // buffer onde o jogo desenha.
    //
    // O Eden (`application_display_service.cpp`, `OpenLayer`): abre o
    // display, abre a camada (e fica com o **binder** do produtor), e
    // devolve um *parcel* com a `NativeWindow` dentro.
    //
    // O parcel é o que o jogo sabe ler: o cabeçalho, a `NativeWindow` e o
    // objecto a zero — byte a byte, como o Eden.
    void OpenLayer(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        (void)rp.PopRaw<std::uint32_t>();  // o aruid

        // A janela: um buffer **real** na memória do guest, do tamanho do
        // display. O guest desenha lá dentro; o compositor da casa
        // apresenta-o.
        //
        // Onde? No espaço de VA da GPU (o mesmo que o `nvdrv` dá ao
        // jogo) — é memória do guest, e por isso o host lê-la
        // directamente.
        const std::uint64_t tamanho = static_cast<std::uint64_t>(Estado().largura) *
                                      static_cast<std::uint64_t>(Estado().altura) * 4u;
        std::uint64_t endereco = 0;
        if (!AlocarJanela(tamanho, endereco)) {
            LOG_WARNING(Service_VI, "IDisplayApplication: OpenLayer: sem espaço para a janela");
        }

        const std::uint64_t binder = Estado().proximo_binder++;
        NativeWindow janela{};
        janela.id = binder;
        const std::vector<std::uint8_t> parcel = SerializarNativeWindow(janela);

        LOG_DEBUG(Service_VI, "IDisplayApplication: OpenLayer: binder={} janela=0x{:X} bytes={}",
                  binder, endereco, tamanho);

        // A resposta: o tamanho do parcel (`Out<u64>`) e o parcel no
        // buffer de saída (`OutBuffer`).
        const std::size_t saida = ctx.GetWriteBufferSize(0);
        if (saida > 0) {
            const std::size_t n = std::min(saida, parcel.size());
            ctx.WriteBuffer(parcel.data(), n, 0);
        }

        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(parcel.size());
    }

    // Aloca o buffer de uma janela no espaço de VA da GPU.
    //
    // **O VA da GPU é gerido pelo `nvdrv`** (que é quem o dá ao jogo);
    // aqui alocamos da mesma reserva, e o `nvdrv` é quem sabe onde estão
    // as regiões. A alocação é feita aqui porque é a display que pede a
    // janela — e o `nvdrv` e' quem tem a reserva.
    //
    // Devolve `true` se alocar.
    static bool AlocarJanela(std::uint64_t tamanho, std::uint64_t& endereco) {
        // A reserva do VA da GPU: a mesma que o `nvdrv` usa (a região
        // de alias, 0x60000000..0x80000000, é do *alias*; o VA da GPU
        // começa em 0x98000000, depois do heap do guest).
        static std::uint64_t proximo = 0x98000000ull;
        constexpr std::uint64_t kFim = 0xC0000000ull;
        if (proximo + tamanho > kFim)
            return false;
        endereco = proximo;
        proximo += (tamanho + 0xFFF) & ~0xFFFull;
        return true;
    }

    // 100: o *relay service* (o driver do *binder*).
    void GetRelayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetRelayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<HosBinderDriver>(ctx, std::make_shared<HosBinderDriver>(system));
    }

    // 101: o display do sistema.
    void GetSystemDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetSystemDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplaySystem>(ctx, std::make_shared<ViDisplaySystem>(system));
    }

    // 102: o display do *manager*.
    void GetManagerDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetManagerDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplayManager>(ctx, std::make_shared<ViDisplayManager>(system));
    }

    // 103: o serviço de transacções do display indirecto.
    void GetIndirectDisplayTransactionService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetIndirectDisplayTransactionService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<HosBinderDriver>(ctx, std::make_shared<HosBinderDriver>(system));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// As três raízes: "vi:m", "vi:s" e "vi:u".
//
// Cada uma devolve o seu display com o `GetDisplayService` — o comando
// com o **ID** que o Eden usa (2 no manager, 1 no sistema, 0 na
// aplicação). É o ID que importa: é o que o guest pede.
// ─────────────────────────────────────────────────────────────────────────────
class ViRootManager final : public Service::ServiceFramework<ViRootManager> {
public:
    explicit ViRootManager(Core::System& system) : ServiceFramework{system, "vi:m"} {
        static const FunctionInfo functions[] = {
            {2, &ViRootManager::GetDisplayService, "GetDisplayService"},
            {3, nullptr, "GetDisplayServiceWithProxyNameExchange"},
            {100, nullptr, "PrepareFatal"},
            {101, nullptr, "ShowFatal"},
            {102, nullptr, "DrawFatalRectangle"},
            {103, nullptr, "DrawFatalText32"},
        };
        RegisterHandlers(functions);
    }

private:
    // **O display que o "vi:m" devolve é o da APLICAÇÃO.**
    //
    // O Eden (`manager_root_service.cpp`, `GetDisplayService`):
    //
    //     Result IManagerRootService::GetDisplayService(
    //         Out<SharedPointer<IApplicationDisplayService>> out_application_display_service,
    //         Policy policy)
    //
    // Ou seja: o "vi:m" (o *manager*) devolve o display **da aplicação** —
    // o `Policy` é que decide com que permissões. Devolver o display do
    // *manager* dava ao guest uma tabela de comandos diferente: ele pedia
    // o 100 (`GetRelayService`), que no display do *manager* não existe
    // (medido: `boot_p7h.log`, `HSR IDisplayManager: comando 100`).
    void GetDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "vi:m: GetDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplayApplication>(ctx,
            std::make_shared<ViDisplayApplication>(system));
    }
};

class ViRootSystem final : public Service::ServiceFramework<ViRootSystem> {
public:
    explicit ViRootSystem(Core::System& system) : ServiceFramework{system, "vi:s"} {
        static const FunctionInfo functions[] = {
            {1, &ViRootSystem::GetDisplayService, "GetDisplayService"},
            {3, nullptr, "GetDisplayServiceWithProxyNameExchange"},
        };
        RegisterHandlers(functions);
    }

private:
    void GetDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "vi:s: GetDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplaySystem>(ctx, std::make_shared<ViDisplaySystem>(system));
    }
};

class ViRootApplication final : public Service::ServiceFramework<ViRootApplication> {
public:
    explicit ViRootApplication(Core::System& system) : ServiceFramework{system, "vi:u"} {
        static const FunctionInfo functions[] = {
            {0, &ViRootApplication::GetDisplayService, "GetDisplayService"},
            {1, nullptr, "GetDisplayServiceWithProxyNameExchange"},
        };
        RegisterHandlers(functions);
    }

private:
    void GetDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "vi:u: GetDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplayApplication>(ctx,
            std::make_shared<ViDisplayApplication>(system));
    }
};

} // namespace

// Regista o `vi` no `sm:` — os três nomes, como o Eden (`vi.cpp`,
// `LoopProcess`).
void registar_vi(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("vi:m", std::make_shared<ViRootManager>(system));
    server_manager->RegisterNamedService("vi:s", std::make_shared<ViRootSystem>(system));
    server_manager->RegisterNamedService("vi:u", std::make_shared<ViRootApplication>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
