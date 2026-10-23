// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "vendor/eden/core/hle/kernel/k_auto_object.h"
#include "vendor/eden/core/hle/kernel/k_auto_object_container.h"
#include "vendor/eden/core/hle/kernel/k_kernel_core_bridge.h"  // PONTE

#include <cstdio>
#include <cstdlib>

namespace Kernel {

template <class Derived>
class KSlabAllocated {
public:
    constexpr KSlabAllocated() = default;

    size_t GetSlabIndex(KernelCore& kernel) const {
        return kernel.SlabHeap<Derived>().GetIndex(static_cast<const Derived*>(this));
    }

public:
    static void InitializeSlabHeap(KernelCore& kernel, void* memory, size_t memory_size) {
        kernel.SlabHeap<Derived>().Initialize(memory, memory_size);
    }

    static Derived* Allocate(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().Allocate(kernel);
    }

    static void Free(KernelCore& kernel, Derived* obj) {
        kernel.SlabHeap<Derived>().Free(obj);
    }

    static size_t GetObjectSize(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetObjectSize();
    }

    static size_t GetSlabHeapSize(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapSize();
    }

    static size_t GetPeakIndex(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetPeakIndex();
    }

    static uintptr_t GetSlabHeapAddress(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapAddress();
    }

    static size_t GetNumRemaining(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetNumRemaining();
    }
};

template <typename Derived, typename Base>
class KAutoObjectWithSlabHeap : public Base {
    static_assert(std::is_base_of<KAutoObject, Base>::value);

private:
    static Derived* Allocate(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().Allocate(kernel);
    }

    static void Free(KernelCore& kernel, Derived* obj) {
        kernel.SlabHeap<Derived>().Free(obj);
    }

public:
    explicit KAutoObjectWithSlabHeap(KernelCore& kernel) : Base(kernel) {}
    virtual ~KAutoObjectWithSlabHeap() = default;

    virtual void Destroy(KernelCore& kernel) override {
        const bool is_initialized = this->IsInitialized();
        uintptr_t arg = 0;
        if (is_initialized) {
            arg = this->GetPostDestroyArgument();
            this->Finalize(kernel);
        }
        Free(kernel, static_cast<Derived*>(this));
        if (is_initialized) {
            Derived::PostDestroy(kernel, arg);
        }
    }

    virtual bool IsInitialized() const {
        return true;
    }
    virtual uintptr_t GetPostDestroyArgument() const {
        return 0;
    }

    size_t GetSlabIndex(KernelCore& kernel) const {
        return SlabHeap<Derived>(kernel).GetObjectIndex(static_cast<const Derived*>(this));
    }

public:
    static void InitializeSlabHeap(KernelCore& kernel, void* memory, size_t memory_size) {
        kernel.SlabHeap<Derived>().Initialize(memory, memory_size);
    }

    static Derived* Create(KernelCore& kernel) {
        Derived* obj = Allocate(kernel);
        if (obj != nullptr) {
            KAutoObject::Create(obj);
            // **Traço da casa** (a cisma do 139):_printa cada `Create` de
            // objecto do kernel — com o traço do container vemos, num
            // corrida, o REUSO completo: criação → registro → destruição →
            // criação NO MESMO endereço (o construtor aponta o hook a
            // zero e o container tem o slot do nó velho — o walk cai).
            if (std::getenv("G610_OBJ_TRACE") != nullptr)
                std::fprintf(stderr, "[obj] CRIADO %p\n", (void*)obj);
        }
        return obj;
    }

    static size_t GetObjectSize(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetObjectSize();
    }

    static size_t GetSlabHeapSize(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapSize();
    }

    static size_t GetPeakIndex(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetPeakIndex();
    }

    static uintptr_t GetSlabHeapAddress(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapAddress();
    }

    static size_t GetNumRemaining(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetNumRemaining();
    }
};

template <typename Derived, typename Base>
class KAutoObjectWithSlabHeapAndContainer : public Base {
    static_assert(std::is_base_of_v<KAutoObjectWithList, Base>);

private:
    static Derived* Allocate(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().Allocate(kernel);
    }

    static void Free(KernelCore& kernel, Derived* obj) {
        kernel.SlabHeap<Derived>().Free(obj);
    }

public:
    KAutoObjectWithSlabHeapAndContainer(KernelCore& kernel) : Base(kernel) {}
    virtual ~KAutoObjectWithSlabHeapAndContainer() {}

    virtual void Destroy(KernelCore& kernel) override {
        const bool is_initialized = this->IsInitialized();
        uintptr_t arg = 0;
        if (is_initialized) {
            kernel.ObjectListContainer().Unregister(this);
            arg = this->GetPostDestroyArgument();
            this->Finalize(kernel);
        }
        Free(kernel, static_cast<Derived*>(this));
        if (is_initialized) {
            Derived::PostDestroy(kernel, arg);
        }
    }

    virtual bool IsInitialized() const {
        return true;
    }
    virtual uintptr_t GetPostDestroyArgument() const {
        return 0;
    }

    size_t GetSlabIndex(KernelCore& kernel) const {
        return SlabHeap<Derived>(kernel).GetObjectIndex(static_cast<const Derived*>(this));
    }

public:
    static void InitializeSlabHeap(KernelCore& kernel, void* memory, size_t memory_size) {
        kernel.SlabHeap<Derived>().Initialize(memory, memory_size);
        kernel.ObjectListContainer().Initialize();
    }

    static Derived* Create(KernelCore& kernel) {
        Derived* obj = Allocate(kernel);
        if (obj != nullptr)
            KAutoObject::Create(obj);
        else
            return nullptr;
        // **Traço da casa** (a cisma do 139): grava cada `Create` de objecto
        // de kernel registrado no container — com o traço do container
        // (k_auto_object_container.cpp) vemos numa corrida o REUSO
        // completo: criação → registro → destruição → criação NO MESMO
        // ENDEREÇO (o construtor põe o hook do nó a zero, e o slot da
        // raiz da árvore continua a apontar-lhe: o walk cai = o core).
        if (std::getenv("G610_OBJ_TRACE") != nullptr)
            std::fprintf(stderr, "[obj] CRIADO %p\n", (void*)obj);
        return obj;
    }

    static void Register(KernelCore& kernel, Derived* obj) {
        return kernel.ObjectListContainer().Register(obj);
    }

    static size_t GetObjectSize(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetObjectSize();
    }

    static size_t GetSlabHeapSize(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapSize();
    }

    static size_t GetPeakIndex(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetPeakIndex();
    }

    static uintptr_t GetSlabHeapAddress(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetSlabHeapAddress();
    }

    static size_t GetNumRemaining(KernelCore& kernel) {
        return kernel.SlabHeap<Derived>().GetNumRemaining();
    }
};

} // namespace Kernel
