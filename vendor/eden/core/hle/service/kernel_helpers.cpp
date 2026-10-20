// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "vendor/eden/core/system_ref.h"  // PONTE: substitui core/core.h
#include "vendor/eden/core/core_timing.h"
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/kernel/k_readable_event.h"
#include "vendor/eden/core/hle/kernel/k_resource_limit.h"
#include "vendor/eden/core/hle/kernel/k_scoped_resource_reservation.h"
#include "vendor/eden/core/hle/service/kernel_helpers.h"

namespace Service::KernelHelpers {

ServiceContext::ServiceContext(Core::System& system_, std::string name_)
    : kernel(system_.Kernel())
{
    // **O processo e' sempre o do guest.**
    //
    // O Eden cria um processo novo quando nao ha um
    // (`kernel_helpers.cpp:20-30`), e esse `KProcess` e' o **real** — com
    // page tables, com o scheduler, com o VFS. E' o emulador.
    //
    // Este motor tem um unico processo: o do guest. E' o unico que
    // corre, e o unico que o codigo do Eden precisa de ver — o
    // `KProcess` da nossa ponte (`k_process_bridge.h`) expoe o que a
    // camada de IPC usa.
    process = Kernel::GetCurrentProcessPointer(kernel);
    ASSERT(process != nullptr);
}

ServiceContext::~ServiceContext() = default;

Kernel::KEvent* ServiceContext::CreateEvent(std::string&& name) {
    // Reserve a new event from the process resource limit
    Kernel::KScopedResourceReservation event_reservation(kernel, process,
                                                         Kernel::LimitableResource::EventCountMax);
    if (!event_reservation.Succeeded()) {
        LOG_CRITICAL(Service, "Resource limit reached!");
        return {};
    }

    // Create a new event.
    auto* event = Kernel::KEvent::Create(kernel);
    if (!event) {
        LOG_CRITICAL(Service, "Unable to create event!");
        return {};
    }

    // Initialize the event.
    event->Initialize(kernel, process);

    // Commit the thread reservation.
    event_reservation.Commit();

    // Register the event.
    Kernel::KEvent::Register(kernel, event);

    return event;
}

void ServiceContext::CloseEvent(Kernel::KEvent* event) {
    if (event) {
        event->GetReadableEvent().Close(kernel);
        event->Close(kernel);
    }
}

} // namespace Service::KernelHelpers
