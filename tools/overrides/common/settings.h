// Substitui `src/common/settings.h` do Eden.
//
// ## Porque
//
// O `settings_bridge.h` (nossa) e o `settings.h` (do Eden) definem **dois
// `Settings::Values` diferentes**, e os dois declaram `Settings::values` —
// o **mesmo símbolo**, com tipos diferentes. O ligador resolve os dois para
// o mesmo objecto, e quem foi compilado contra o `Values` do Eden lê os
// campos **nos deslocamentos do Eden**.
//
// A leitura que o `aoc` faz (`addon_content_manager.cpp:121`)
//
//     Settings::values.disabled_addons[current]
//
// é exactamente essa: um campo que o nosso `Values` não tem, lido a um
// deslocamento que é o meio de outro objecto. O que lá está é lixo, e o
// `std::find` sobre um vector lixo mata o motor.
//
// Medido (`boot_p6j.log`): o fault foi um `std::find` dentro da
// `CountAddOnContent`, com o `aoc` a escrever num endereço de host
// (`0xaaaacd87d140`).
//
// ## A solução
//
// Este ficheiro **é** a ponte. Passa a haver um só `Settings::Values` no
// binário, e o `aoc` passa a ver o mesmo objecto que o resto.
//
// ## Os campos que o motor Maxwell precisa
//
// Com o `video_core` do upstream a compilar na casa, os campos que ele lê
// já não são três — são **dez** (medido: `grep -rhoE
// "Settings::values\.[a-z_0-9]+"` sobre os ficheiros que a casa
// compila do `video_core`):
//
//     astc_recompression, use_reactive_flushing, nvdec_emulation,
//     sync_memory_operations, disable_macro_hle, dump_macros,
//     disable_macro_jit, use_fast_gpu_time, fast_gpu_time
//
// e mais um, no cabeçalho da interface do renderizador:
// `skip_cpu_inner_invalidation`.
//
// E o upstream **lê-os como `Setting<T>::GetValue()`** ✗, enquanto a
// casa os lia como `bool` ✗ — por isso o `Valor<T>` de baixo serve os
// dois usos ✓ (conversão implícita ✓ e `GetValue()` ✓).
//
// SPDX-FileCopyrightText: 2026 Eden Emulator Project
// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vendor/eden/common/settings_bridge.h"
