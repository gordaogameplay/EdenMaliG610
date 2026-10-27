// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/intrusive/rbtree.hpp>

#include "vendor/eden/common/common_funcs.h"
#include "vendor/eden/core/hle/kernel/k_auto_object.h"
#include "vendor/eden/core/hle/kernel/k_spin_lock.h"

namespace Kernel {

class KernelCore;
class KProcess;

class KAutoObjectWithListContainer {
public:
    YUZU_NON_COPYABLE(KAutoObjectWithListContainer);
    YUZU_NON_MOVEABLE(KAutoObjectWithListContainer);

    using ListType = boost::intrusive::rbtree<KAutoObjectWithList>;

    KAutoObjectWithListContainer(KernelCore& kernel) : m_lock(), m_object_list() {}

    void Initialize() {}
    void Finalize() {}

    void Register(KAutoObjectWithList* obj);
    void Unregister(KAutoObjectWithList* obj);
    size_t GetOwnedCount(KProcess* owner);

    /// Itera sobre todos os objetos registados, chamando `func` para cada um.
    /// Seguro para leitura concorrente (segura o lock interno).
    template <typename Func>
    void ForEach(Func&& func) {
        KScopedSpinLock lk(m_lock);
        for (auto& obj : m_object_list) {
            func(obj);
        }
    }

private:
    KSpinLock m_lock;
    ListType m_object_list;
};

} // namespace Kernel
