// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// O `pl:u` — o servico da plataforma (a **fonte do sistema**), **nosso**.
//
// ## O que e'
//
// O `pl:u` (o `IPlatformServiceManager` do Eden) e' o servico que da ao
// SDK a **fonte partilhada do sistema**: uma regiao de memoria partilhada
// com os glifos, e a lista das fontes por ordem de prioridade.
//
// ## Porque este ficheiro existe
//
// **Medido** (`/tmp/boot_r23.log`): o SDK pede `GetService("pl:u")` ao
// `sm:`, e a casa **nao o tinha**:
//
//     Service: SM: GetService: nome lido = "pl:u"
//     Service: SM: Server is not registered! service=pl:u
//     Service: SM: Waiting for service pl:u to become available
//     Service: Audio: SetTerminateResult: modulo=10 desc=212
//
// O `sm:` **difere** o pedido (o `sm:` espera que o servico apareca) e o
// guest aborta no leitor CMIF — o mesmo caminho do `fsp-srv` e do `time:u`.
//
// A cadeia medida: o SDK le o idioma (`nn::settings::GetLanguageCode` -> o
// `set:`, que ja' existe) e depois vai buscar a fonte do sistema.
//
// ## De onde vem a forma
//
// Do Eden: `core/hle/service/ns/platform_service_manager.cpp` (a tabela dos
// 16 comandos) e o `ns.cpp` (o registo do `pl:u` e do `pl:s`). Os **IDs**
// e os **nomes** sao copiados a mao — as tabelas **nao se inventam**.
//
// ## O que a casa **nao** tem: a fonte
//
// A fonte do sistema vem do **firmware** da consola (os ficheiros
// `.bfttf` dos arquivos do sistema). A casa nao os tem, e o `GetSize`
// devolve **zero** — a regiao da fonte e' **vazia**.
//
// **Medido**: o jogo arranca e continua (le os seus ficheiros do RomFS e
// desenha) com a fonte vazia. O que nao ha e' texto do sistema — o jogo
// traz as suas proprias fontes.
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "g610/services/pl_u.hpp"
#include "g610/services/shared_font.hpp"
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

// O tipo de fonte (o Eden, `platform_service_manager.h:28`).
enum class SharedFontType : std::uint32_t {
    JapanUSEuropeStandard = 0,
    ChineseSimplified = 1,
    ExtendedChineseSimplified = 2,
    ChineseTraditional = 3,
    KoreanHangul = 4,
    NintendoExtended = 5,
};

// O estado do carregamento (o Eden, `platform_service_manager.h:37`).
enum class LoadState : std::uint32_t {
    Loading = 0,
    Loaded = 1,
};

// ─────────────────────────────────────────────────────────────────────────────
// O servico
// ─────────────────────────────────────────────────────────────────────────────

class PlU final : public Service::ServiceFramework<PlU> {
public:
    explicit PlU(Core::System& system) : ServiceFramework{system, "pl:u"} {
        // A tabela e' a do Eden (`platform_service_manager.cpp:138-153`).
        static const FunctionInfo functions[] = {
            {0, &PlU::RequestLoad, "RequestLoad"},
            {1, &PlU::GetLoadState, "GetLoadState"},
            {2, &PlU::GetSize, "GetSize"},
            {3, &PlU::GetSharedMemoryAddressOffset, "GetSharedMemoryAddressOffset"},
            {4, &PlU::GetSharedMemoryNativeHandle, "GetSharedMemoryNativeHandle"},
            {5, &PlU::GetSharedFontInOrderOfPriority, "GetSharedFontInOrderOfPriority"},
            {6, &PlU::GetSharedFontInOrderOfPriorityForSystem,
             "GetSharedFontInOrderOfPriorityForSystem"},
            {100, nullptr, "RequestApplicationFunctionAuthorization"},
            {101, nullptr, "RequestApplicationFunctionAuthorizationByProcessId"},
            {102, nullptr, "RequestApplicationFunctionAuthorizationByApplicationId"},
            {103, nullptr, "RefreshApplicationFunctionBlackListDebugRecord"},
            {104, nullptr, "RequestApplicationFunctionAuthorizationByProgramId"},
            {105, nullptr, "GetFunctionBlackListSystemVersionToAuthorize"},
            {106, nullptr, "GetFunctionBlackListVersion"},
            {1000, nullptr, "LoadNgWordDataForPlatformRegionChina"},
            {1001, nullptr, "GetNgWordDataSizeForPlatformRegionChina"},
        };
        RegisterHandlers(functions);

        // O objecto por tras do comando 4. O kernel vendorizado nao tem
        // `KSharedMemory` — o que o guest recebe e' um **handle real de
        // copia** para um `KEvent`, e o conteudo do "map" e' tratado pelo
        // `svc_map_shared_memory` (que e' um no-op na casa).
        //
        // E' exactamente o que o `time:u` da casa faz com o seu
        // `GetSharedMemoryNativeHandle` (ver `src/services/time_u.cpp`).
        shm_ = new Kernel::KEvent(kernel);
        auto& processo = Kernel::GetCurrentProcess(kernel);
        shm_->Initialize(kernel, &processo);
    }

    ~PlU() override {
        if (shm_ != nullptr) {
            shm_->Close(kernel);
            delete shm_;
        }
    }

private:
    // 0: pedir o carregamento. O Eden responde sempre com sucesso (as
    // fontes ja' estao' carregadas).
    void RequestLoad(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto tipo = rp.PopRaw<std::uint32_t>();
        LOG_DEBUG(Service_NS, "RequestLoad: tipo={}", tipo);
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 1: o estado do carregamento — o Eden responde sempre "carregado".
    void GetLoadState(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto tipo = rp.PopRaw<std::uint32_t>();
        LOG_DEBUG(Service_NS, "GetLoadState: tipo={}", tipo);
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<std::uint32_t>(LoadState::Loaded));
    }

    // 2: o tamanho da regiao da fonte.
    //
    // **Com a fonte** ✗ — o tamanho da regiao ✗ (o Eden,
    // `platform_service_manager.cpp:221-225` ✗) ✗.
    void GetSize(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto tipo = rp.PopRaw<std::uint32_t>();
        const auto& regioes = RegioesDaFonte();
        const std::uint32_t tamanho =
            tipo < regioes.size() ? regioes[tipo].tamanho : 0u;
        LOG_DEBUG(Service_NS, "GetSize: tipo={} -> {}", tipo, tamanho);
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(tamanho);
    }

    // 3: o offset da regiao da fonte dentro da memoria partilhada.
    void GetSharedMemoryAddressOffset(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto tipo = rp.PopRaw<std::uint32_t>();
        const auto& regioes = RegioesDaFonte();
        const std::uint32_t offset = tipo < regioes.size() ? regioes[tipo].offset : 0u;
        LOG_DEBUG(Service_NS, "GetSharedMemoryAddressOffset: tipo={} -> {}", tipo, offset);
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(offset);
    }

    // 4: o handle da memoria partilhada da fonte.
    void GetSharedMemoryNativeHandle(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NS, "GetSharedMemoryNativeHandle: handle de KEvent (shm_ no-op)");
        ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &shm_->GetReadableEvent());
    }

    // 5 e 6: as fontes por ordem de prioridade.
    //
    // **O Eden** (`platform_service_manager.cpp:248-277` ✗) ✗: as tres
    // listas (os codigos ✗, os offsets ✗ e os tamanhos ✗) vao para tres
    // **buffers de saida** ✗ ✗, e o `out_font_count` ✗ e' o numero de
    // fontes ✗ ✗.
    //
    // **O maximo e' 6** ✗ (`MaxElementCount` ✗, a linha 257 ✗) ✗ ✗.
    void GetSharedFontInOrderOfPriority(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto linguagem = rp.PopRaw<std::uint64_t>();
        LOG_DEBUG(Service_NS, "GetSharedFontInOrderOfPriority: linguagem={:#x}", linguagem);

        const auto& regioes = RegioesDaFonte();

        // As tres listas — cada uma num buffer de saida ✗ ✗.
        //
        // **O Eden** (`platform_service_manager.cpp:260-270` ✗) ✗: o
        // maximo e' o menor ✗ entre 6 ✗, os tamanhos dos tres buffers ✗
        // e o numero de regioes ✗ ✗.
        constexpr std::size_t kMaximo = 6;

        const std::size_t capacidade =
            std::min({kMaximo, ctx.GetWriteBufferSize(0) / sizeof(std::uint32_t),
                      ctx.GetWriteBufferSize(1) / sizeof(std::uint32_t),
                      ctx.GetWriteBufferSize(2) / sizeof(std::uint32_t), regioes.size()});

        std::vector<std::uint32_t> codigos(capacidade);
        std::vector<std::uint32_t> offsets(capacidade);
        std::vector<std::uint32_t> tamanhos(capacidade);
        for (std::size_t i = 0; i < capacidade; ++i) {
            codigos[i] = static_cast<std::uint32_t>(i);
            offsets[i] = regioes[i].offset;
            tamanhos[i] = regioes[i].tamanho;
        }
        if (capacidade > 0) {
            ctx.WriteBuffer(codigos.data(), codigos.size() * sizeof(std::uint32_t), 0);
            ctx.WriteBuffer(offsets.data(), offsets.size() * sizeof(std::uint32_t), 1);
            ctx.WriteBuffer(tamanhos.data(), tamanhos.size() * sizeof(std::uint32_t), 2);
        }

        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<std::uint8_t>(1));  // as fontes estao' "carregadas"
        rb.Push(static_cast<std::uint32_t>(capacidade));
    }

    void GetSharedFontInOrderOfPriorityForSystem(HLERequestContext& ctx) {
        GetSharedFontInOrderOfPriority(ctx);
    }

    Kernel::KEvent* shm_ = nullptr;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// O registo
// ─────────────────────────────────────────────────────────────────────────────

// Regista o `pl:u` no `sm:` — o mesmo caminho do `set:`, do `time:u` e do
// `fsp-srv`: um `ServerManager` proprio que fica no meta-registo do pump.
void registar_pl_u(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("pl:u", std::make_shared<PlU>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
