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
// Medido (`boot_p6j.log`): o fault foi um `std::find` dentro do
// `CountAddOnContent`, com o `aoc` a escrever num endereço de host
// (`0xaaaacd87d140`).
//
// ## A solução
//
// Este ficheiro **é** a ponte. Passa a haver um só `Settings::Values` no
// binário, e o `aoc` passa a ver o mesmo objecto que o resto.
//
// O `settings.h` do Eden tem 928 linhas de configuração que **nenhum**
// ficheiro que compilamos lê — medido: só `use_auto_stub`,
// `use_debug_asserts` e `disabled_addons` são lidos, e estão todos na ponte.
//
// SPDX-FileCopyrightText: 2026 Eden Emulator Project
// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "vendor/eden/common/settings_bridge.h"
