// SPDX-FileCopyrightText: 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <climits>
#include <cstddef>
#include <type_traits>

#include "vendor/eden/common/common_types.h"

namespace Common {

/// Gets the size of a specified type T in bits.
template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr std::size_t BitSize() {
    return std::size_t(sizeof(T) * CHAR_BIT);
}

// ── A API de 32 e 64 bits do upstream ───────────────────────────────────
//
// **Medido**: o `video_core` do upstream usa `Common::Log2Ceil64` ✓
// (`maxwell_3d.cpp:272` ✓) e `Common::Log2Ceil32` ✓ — e o `core/` vendorizado
// já usava `Common::Log2Ceil64` ✓ (`nvhost_as_gpu.cpp:129` ✓). A casa tinha
// só a API por *template* ✗ (`Log2Ceil<T>` ✗), que o upstream já não tem ✗.
//
// Estas são as funções **do upstream** ✓, copiadas tal como estão ✓ (a
// parte sistemática ✗ — a casa não reescreve aritmética de bits ✗).

[[nodiscard]] constexpr u32 MostSignificantBit32(const u32 value) {
    return 31U - static_cast<u32>(std::countl_zero(value));
}

[[nodiscard]] constexpr u32 MostSignificantBit64(const u64 value) {
    return 63U - static_cast<u32>(std::countl_zero(value));
}

[[nodiscard]] constexpr u32 Log2Floor32(const u32 value) {
    return MostSignificantBit32(value);
}

[[nodiscard]] constexpr u32 Log2Floor64(const u64 value) {
    return MostSignificantBit64(value);
}

[[nodiscard]] constexpr u32 Log2Ceil32(const u32 value) {
    const u32 log2_f = Log2Floor32(value);
    return log2_f + static_cast<u32>((value ^ (1U << log2_f)) != 0U);
}

[[nodiscard]] constexpr u32 Log2Ceil64(const u64 value) {
    const u64 log2_f = Log2Floor64(value);
    return static_cast<u32>(log2_f + static_cast<u64>((value ^ (1ULL << log2_f)) != 0ULL));
}

template <typename T>
    requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool IsPow2(T value) {
    return std::has_single_bit(value);
}

// ── A API por template, que a casa já tinha ─────────────────────────────
//
// **Medido**: o `core/` vendorizado usa `Common::BitSize<u64>()` ✓ em
// vários sítios ✗ (`k_memory_manager.h:197` ✓, `k_thread.cpp:121` ✓,
// `k_memory_region_type.h:78` ✓) — e o upstream tirou-lhe o *template* ✗
// (o `BitSize()` dele nem compila ✗ quando alguém o instancia ✗).
//
// Logo: a API por template **fica** ✓ (é usada ✗) e a do upstream **entra**
// ✓. São nomes diferentes ✓ — não há sobreposição ✗.

template<typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr u32 MostSignificantBit(const T value) {
    return u32(sizeof(T) * CHAR_BIT - 1 - std::countl_zero(value));
}

template<typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr T Log2Floor(const T value) {
    return T(MostSignificantBit<T>(value));
}

template<typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr T Log2Ceil(const T value) {
    const T log2_f = Log2Floor<T>(value);
    return T(log2_f + T((value ^ (T(1ULL) << log2_f)) != T(0ULL)));
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] T NextPow2(T value) {
    return T(1ULL << (sizeof(T) * CHAR_BIT - std::countl_zero(value - 1U)));
}

template <size_t bit_index, typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool Bit(const T value) {
    static_assert(bit_index < BitSize<T>(), "bit_index must be smaller than size of T");
    return ((value >> bit_index) & T(1)) == T(1);
}

} // namespace Common
