// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

// **Parche da casa** (medida do core de 23/10, LOGX6): o `Unregister` do
// desligar cai no walk do rbtree num nó com left/right/parent a zero — a
// memória re-usada (o `Free` do primeiro `Destroy` deixou `m_initialized`
// a true, e um segundo Destroy passa o gate). O traço de baixo (`G610_
// OBJ_TRACE=1`) grava cada Register, cada Unregister e o estado do hook
// (3 ponteiros a partir do objecto, o layout medido no core: node = object
// +0x10) para o post-processamento achar a colisão: UNREG do mesmo
// endereço que já saiu (e não voltou a registar) = double destroy.
#include <cstdio>
#include <cstdlib>

#include "vendor/eden/core/hle/kernel/k_auto_object_container.h"

namespace Kernel {

namespace {
// `static FILE* de getenv` — o handler de sinais não chama isto (o walk
// corre em `main` no desligar), mas `fprintf(stderr, ...)` dentro do
// Unregister de cada objecto do kernel ao destruir precisa de saber
// rapidinho se está ligado.
bool obj_trace_on() {
    static const bool on = std::getenv("G610_OBJ_TRACE") != nullptr;
    return on;
}
} // namespace

void KAutoObjectWithListContainer::Register(KAutoObjectWithList* obj) {
    if (obj_trace_on()) {
        // REG sem estado do hook: o hook do nó FOI inicializado pelo
        // construtor do objecto (a árvore só o usa quando está ligado).
        std::fprintf(stderr, "[obj] REG  %p\n", (void*)obj);
    }
    // KScopedInterruptDisable di;
    KScopedSpinLock lk(m_lock);

    m_object_list.insert_unique(*obj);
}

void KAutoObjectWithListContainer::Unregister(KAutoObjectWithList* obj) {
    // **Parche da casa** (medida do core de 23/10, LOGX6 + LOGZ2): o
    // `erase` do rbtree cai num nó cujos três ponteiros estão a zero — um
    // nó que NÃO está na árvore (o construtor do reuso de servo põe o
    // hook a zero; o slot da raiz fica a apontar-lhe). Erase de nó que
    // não existe é comportamento indefinido: está a ser o segfault do
    // 139. Um nó LEGÍTIMO tem SEMPRE um destes três non-nulos (parent,
    // left ou right) — por isso `todos a zero ⇒ skip` é seguro.
    //
    // O skip não repara o estado da árvore (a raiz continua torta) —
    // o reparo completo depende de a casa fechar a ROOTA da criação do
    // nó estranho: o traço (G610_OBJ_TRACE=1) grava as rotas.
    const void* objecto = reinterpret_cast<const void*>(obj);
    const void* camp1 = *reinterpret_cast<const void* const*>(objecto + 0x10);
    const void* camp2 = *reinterpret_cast<const void* const*>(objecto + 0x18);
    const void* camp3 = *reinterpret_cast<const void* const*>(objecto + 0x20);
    if (camp1 == nullptr && camp2 == nullptr && camp3 == nullptr) {
        if (obj_trace_on())
            std::fprintf(stderr,
                         "[obj] SKIP %p ra=%p ra1=%p ra2=%p ra3=%p\n",
                         (void*)obj,
                         __builtin_return_address(0),
                         __builtin_return_address(1),
                         __builtin_return_address(2),
                         __builtin_return_address(3));
        return;
    }
    if (obj_trace_on())
        std::fprintf(stderr,
                     "[obj] UNREG %p cs=%p %p %p\n",
                     (void*)obj, camp1, camp2, camp3);
    // KScopedInterruptDisable di;
    KScopedSpinLock lk(m_lock);

    m_object_list.erase(*obj);
}

size_t KAutoObjectWithListContainer::GetOwnedCount(KProcess* owner) {
    // KScopedInterruptDisable di;
    KScopedSpinLock lk(m_lock);

    return std::count_if(m_object_list.begin(), m_object_list.end(),
                         [&](const auto& obj) { return obj.GetOwner() == owner; });
}

} // namespace Kernel
