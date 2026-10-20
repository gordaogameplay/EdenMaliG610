// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// **Adaptado** do Eden (`src/core/hle/service/aoc/addon_content_manager.h`).
//
// O que mudou: nada na assinatura. O `aoc:u` e' o **gerenciador de DLC**
// (add-on content), e o Eden implementa-o em cima do `ContentProvider` — o
// emulador de ficheiros, que sabe quais os DLC instalados.
//
// **Um motor de execucao nativa nao tem DLC.** Nao ha NCA, nao ha
// `ContentProvider`, nao ha titulo carregado de um `.nca`. O que este
// motor tem e' o **programa ja decomposto**.
//
// Mas o guest **precisa que o servico exista**: o `sm:` deixa o pedido
// pendente enquanto o servico nao aparecer (`sm.cpp:200-203`,
// `ctx.SetIsDeferred()`), e a sessao do `sm:` fica bloqueada — o que
// bloqueia **todos** os pedidos seguintes do guest ao `sm:`.
//
// Medido: sem o `aoc:u`, o guest pede-o, o `sm:` fica a espera, e os
// pedidos seguintes (`pctl`, `pctl:s`, `pctl:a`, `pctl:r`) **nunca saem da
// fila**.

#pragma once

#include "vendor/eden/core/hle/service/cmif_types.h"
#include "vendor/eden/core/hle/service/kernel_helpers.h"
#include "vendor/eden/core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::AOC {

class IPurchaseEventManager;

class IAddOnContentManager final : public ServiceFramework<IAddOnContentManager> {
public:
    explicit IAddOnContentManager(Core::System& system);
    ~IAddOnContentManager() override;

    Result CountAddOnContent(Out<u32> out_count, ClientProcessId process_id);
    Result ListAddOnContent(Out<u32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_addons,
                            u32 offset, u32 count, ClientProcessId process_id);
    Result GetAddOnContentBaseId(Out<u64> out_title_id, ClientProcessId process_id);
    Result PrepareAddOnContent(s32 addon_index, ClientProcessId process_id);
    Result GetAddOnContentListChangedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetAddOnContentListChangedEventWithProcessId(
        OutCopyHandle<Kernel::KReadableEvent> out_event, ClientProcessId process_id);
    Result NotifyMountAddOnContent();
    Result NotifyUnmountAddOnContent();
    Result CheckAddOnContentMountStatus();
    Result CreateEcPurchasedEventManager(OutInterface<IPurchaseEventManager> out_interface);
    Result CreatePermanentEcPurchasedEventManager(
        OutInterface<IPurchaseEventManager> out_interface);

private:
    // **A lista de DLC e' vazia.** No Eden vem do `ContentProvider`
    // (`AccumulateAOCTitleIDs`): os DLC instalados, filtrados pelo
    // estado de cada um. Aqui nao ha DLC, e a lista vazia e' a resposta
    // honesta.
    std::vector<u64> add_on_content;
    KernelHelpers::ServiceContext service_context;

    Kernel::KEvent* aoc_change_event;
};

void LoopProcess(Core::System& system);

} // namespace Service::AOC
