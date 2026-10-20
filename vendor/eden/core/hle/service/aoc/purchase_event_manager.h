// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// **Adaptado** do Eden (`src/core/hle/service/aoc/purchase_event_manager.h`).
//
// O que mudou: nada. Este servico nao toca no emulador de ficheiros —
// e' o gerenciador de eventos de compra, e nao ha compras num motor de
// execucao nativa.

#pragma once

#include "vendor/eden/core/hle/service/cmif_types.h"
#include "vendor/eden/core/hle/service/kernel_helpers.h"
#include "vendor/eden/core/hle/service/os/event.h"
#include "vendor/eden/core/hle/service/service.h"

namespace Service::AOC {

class IPurchaseEventManager final : public ServiceFramework<IPurchaseEventManager> {
public:
    explicit IPurchaseEventManager(Core::System& system_);
    ~IPurchaseEventManager() override;

    Result SetDefaultDeliveryTarget(ClientProcessId process_id,
                                    InBuffer<BufferAttr_HipcMapAlias> in_buffer);
    Result SetDeliveryTarget(u64 unknown, InBuffer<BufferAttr_HipcMapAlias> in_buffer);
    Result GetPurchasedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result PopPurchasedProductInfo();
    Result PopPurchasedProductInfoWithUid();

private:
    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* purchased_event;
};

} // namespace Service::AOC
