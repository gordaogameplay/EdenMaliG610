// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// **Adaptado** do Eden (`src/core/hle/service/aoc/addon_content_manager.cpp`).
//
// O que mudou, e porquê:
//
// 1. A lista de DLC (`AccumulateAOCTitleIDs`) e' **vazia**. No Eden vem
//    do `ContentProvider` — o emulador de ficheiros. Um motor de execucao
//    nativa nao tem NCA nem `ContentProvider`.
//
// 2. O `GetAddOnContentBaseId` nao le' os metadados de control parental
//    (`PatchManager`), porque sao do emulador de ficheiros. O Eden tambem
//    trata essa leitura como **opcional** (`if (res.first == nullptr)`), e
//    devolve o base title id calculado a partir do id do titulo.
//
// 3. As funcoes de mascara do title id (`GetBaseTitleID`,
//    `GetAOCBaseTitleID`, `GetAOCID`) vem do `FileSys::common_funcs.h` do
//    Eden. Sao `constexpr` e nao dependem de nada — mas vivem em
//    `file_sys/`, que e' a arvore do emulador de ficheiros. Estao aqui,
//    com a fonte citada.

#include <algorithm>
#include <numeric>
#include <vector>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/common/settings.h"
#include "vendor/eden/core/system_ref.h"  // PONTE: substitui core/core.h
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/service/aoc/addon_content_manager.h"
#include "vendor/eden/core/hle/service/aoc/purchase_event_manager.h"
#include "vendor/eden/core/hle/service/cmif_serialization.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"

namespace Service::AOC {

namespace {

// ── as mascaras do title id ──
//
// De `src/core/file_sys/common_funcs.h` (Eden):
//
//     constexpr u64 AOC_TITLE_ID_MASK = 0x7FF;
//     constexpr u64 AOC_TITLE_ID_OFFSET = 0x1000;
//     constexpr u64 BASE_TITLE_ID_MASK = 0xFFFFFFFFFFFFE000;
//
// Sao aritmetica de bits — nao ha nada do emulador de ficheiros nelas. Mas
// vivem em `file_sys/`, que e' a arvore do emulador. Copiar tres
// `constexpr` e' melhor do que vendorizar a arvore inteira.
constexpr u64 kAocTitleIdMask = 0x7FF;
constexpr u64 kAocTitleIdOffset = 0x1000;
constexpr u64 kBaseTitleIdMask = 0xFFFFFFFFFFFFE000;

constexpr u64 GetBaseTitleID(u64 title_id) {
    return title_id & kBaseTitleIdMask;
}

constexpr u64 GetAOCBaseTitleID(u64 title_id) {
    return GetBaseTitleID(title_id) + kAocTitleIdOffset;
}

constexpr u64 GetAOCID(u64 aoc_title_id) {
    return aoc_title_id & kAocTitleIdMask;
}

} // namespace

static bool CheckAOCTitleIDMatchesBase(u64 title_id, u64 base) {
    return GetBaseTitleID(title_id) == base;
}

IAddOnContentManager::IAddOnContentManager(Core::System& system_)
    : ServiceFramework{system_, "aoc:u"}, add_on_content{},
      service_context{system_, "aoc:u"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CountAddOnContentByApplicationId"},
        {1, nullptr, "ListAddOnContentByApplicationId"},
        {2, D<&IAddOnContentManager::CountAddOnContent>, "CountAddOnContent"},
        {3, D<&IAddOnContentManager::ListAddOnContent>, "ListAddOnContent"},
        {4, nullptr, "GetAddOnContentBaseIdByApplicationId"},
        {5, D<&IAddOnContentManager::GetAddOnContentBaseId>, "GetAddOnContentBaseId"},
        {6, nullptr, "PrepareAddOnContentByApplicationId"},
        {7, D<&IAddOnContentManager::PrepareAddOnContent>, "PrepareAddOnContent"},
        {8, D<&IAddOnContentManager::GetAddOnContentListChangedEvent>, "GetAddOnContentListChangedEvent"},
        {9, nullptr, "GetAddOnContentLostErrorCode"},
        {10, D<&IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId>, "GetAddOnContentListChangedEventWithProcessId"},
        {11, D<&IAddOnContentManager::NotifyMountAddOnContent>, "NotifyMountAddOnContent"},
        {12, D<&IAddOnContentManager::NotifyUnmountAddOnContent>, "NotifyUnmountAddOnContent"},
        {13, nullptr, "IsAddOnContentMountedForDebug"},
        {50, D<&IAddOnContentManager::CheckAddOnContentMountStatus>, "CheckAddOnContentMountStatus"},
        {100, D<&IAddOnContentManager::CreateEcPurchasedEventManager>, "CreateEcPurchasedEventManager"},
        {101, D<&IAddOnContentManager::CreatePermanentEcPurchasedEventManager>, "CreatePermanentEcPurchasedEventManager"},
        {110, nullptr, "CreateContentsServiceManager"},
        {200, nullptr, "SetRequiredAddOnContentsOnContentsAvailabilityTransition"},
        {300, nullptr, "SetupHostAddOnContent"},
        {301, nullptr, "GetRegisteredAddOnContentPath"},
        {302, nullptr, "UpdateCachedList"},
    };
    // clang-format on

    RegisterHandlers(functions);

    aoc_change_event = service_context.CreateEvent("GetAddOnContentListChanged:Event");
}

IAddOnContentManager::~IAddOnContentManager() {
    service_context.CloseEvent(aoc_change_event);
}

Result IAddOnContentManager::CountAddOnContent(Out<u32> out_count, ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called. process_id={}", process_id.pid);

    const auto current = system.GetApplicationProcessProgramID();

    const auto& disabled = Settings::values.disabled_addons[current];
    if (std::find(disabled.begin(), disabled.end(), "DLC") != disabled.end()) {
        *out_count = 0;
        R_SUCCEED();
    }

    // A lista e' vazia (ver o cabecalho), logo a contagem e' zero — mas
    // **a conta e' feita na mesma**, para que a resposta tenha a forma
    // que o guest espera.
    *out_count = static_cast<u32>(
        std::count_if(add_on_content.begin(), add_on_content.end(),
                      [current](u64 tid) { return CheckAOCTitleIDMatchesBase(tid, current); }));

    R_SUCCEED();
}

Result IAddOnContentManager::ListAddOnContent(Out<u32> out_count,
                                              OutBuffer<BufferAttr_HipcMapAlias> out_addons,
                                              u32 offset, u32 count, ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called with offset={}, count={}, process_id={}", offset, count,
              process_id.pid);

    const auto current = GetBaseTitleID(system.GetApplicationProcessProgramID());

    std::vector<u32> out;
    const auto& disabled = Settings::values.disabled_addons[current];
    if (std::find(disabled.begin(), disabled.end(), "DLC") == disabled.end()) {
        for (u64 content_id : add_on_content) {
            if (GetBaseTitleID(content_id) != current) {
                continue;
            }

            out.push_back(static_cast<u32>(GetAOCID(content_id)));
        }
    }

    // TODO(DarkLordZach): Find the correct error code.
    R_UNLESS(out.size() >= offset, ResultUnknown);

    *out_count = static_cast<u32>(std::min<size_t>(out.size() - offset, count));
    std::rotate(out.begin(), out.begin() + offset, out.end());

    std::memcpy(out_addons.data(), out.data(), *out_count * sizeof(u32));

    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentBaseId(Out<u64> out_title_id,
                                                   ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called. process_id={}", process_id.pid);

    // O Eden, quando a leitura dos metadados de control parental falha,
    // devolve o base title id **calculado** a partir do id do titulo. E'
    // esse o caminho de um motor nativo: nao ha metadados para ler.
    const auto title_id = system.GetApplicationProcessProgramID();
    *out_title_id = GetAOCBaseTitleID(title_id);

    R_SUCCEED();
}

Result IAddOnContentManager::PrepareAddOnContent(s32 addon_index, ClientProcessId process_id) {
    LOG_WARNING(Service_AOC, "(STUBBED) called with addon_index={}, process_id={}", addon_index,
                process_id.pid);

    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentListChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_event = &aoc_change_event->GetReadableEvent();

    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId(
    OutCopyHandle<Kernel::KReadableEvent> out_event, ClientProcessId process_id) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_event = &aoc_change_event->GetReadableEvent();

    R_SUCCEED();
}

Result IAddOnContentManager::NotifyMountAddOnContent() {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    R_SUCCEED();
}

Result IAddOnContentManager::NotifyUnmountAddOnContent() {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    R_SUCCEED();
}

Result IAddOnContentManager::CheckAddOnContentMountStatus() {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    R_SUCCEED();
}

Result IAddOnContentManager::CreateEcPurchasedEventManager(
    OutInterface<IPurchaseEventManager> out_interface) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_interface = std::make_shared<IPurchaseEventManager>(system);

    R_SUCCEED();
}

Result IAddOnContentManager::CreatePermanentEcPurchasedEventManager(
    OutInterface<IPurchaseEventManager> out_interface) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_interface = std::make_shared<IPurchaseEventManager>(system);

    R_SUCCEED();
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    server_manager->RegisterNamedService("aoc:u", std::make_shared<IAddOnContentManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::AOC
