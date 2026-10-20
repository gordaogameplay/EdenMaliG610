// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "vendor/eden/core/hle/kernel/svc_results.h"
#include "vendor/eden/common/scope_exit.h"
#include "vendor/eden/core/hle/kernel/k_client_session.h"
#include "vendor/eden/core/hle/kernel/k_server_session.h"
#include "vendor/eden/core/hle/kernel/k_session.h"
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"  // PONTE: estes dois NAO usam KThread
#include "vendor/eden/core/hle/result.h"

namespace Kernel {

KClientSession::KClientSession(KernelCore& kernel) : KAutoObject{kernel} {}
KClientSession::~KClientSession() = default;

void KClientSession::Destroy(KernelCore& kernel) {
    m_parent->OnClientClosed(kernel);
    m_parent->Close(kernel);
}

void KClientSession::OnServerClosed() {}

Result KClientSession::SendSyncRequest(KernelCore& kernel, uintptr_t address, size_t size) {
    // Create a session request.
    KSessionRequest* request = KSessionRequest::Create(kernel);
    R_UNLESS(request != nullptr, ResultOutOfResource);
    SCOPE_EXIT {
        request->Close(kernel);
    };

    // Initialize the request.
    request->Initialize(kernel, nullptr, address, size);

    // Send the request.
    R_RETURN(m_parent->OnRequest(kernel, request));
}

Result KClientSession::SendAsyncRequest(KernelCore& kernel, KEvent* event, uintptr_t address, size_t size) {
    // Create a session request.
    KSessionRequest* request = KSessionRequest::Create(kernel);
    R_UNLESS(request != nullptr, ResultOutOfResource);
    SCOPE_EXIT {
        request->Close(kernel);
    };

    // Initialize the request.
    request->Initialize(kernel, event, address, size);

    // Send the request.
    R_RETURN(m_parent->OnRequest(kernel, request));
}

} // namespace Kernel
