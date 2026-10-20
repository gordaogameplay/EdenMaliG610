// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// O controller do `apm`, **nosso**.
//
// Substitui `core/hle/service/apm/apm_controller.{h,cpp}` do Eden.
//
// ## O que muda
//
// O controller do Eden tem três dependências que não queremos:
//
//   1. `Core::Timing::CoreTiming&` — que **nunca é usado**. O próprio Eden o
//      diz nos TODOs do fim do `.cpp`:
//
//          // TODO(DarkLordZach): Actually signal core_timing to change clock speed.
//          // TODO(Rodrigo): Remove [[maybe_unused]] when core_timing is used.
//
//      Um `[[maybe_unused]]` é o Eden a admitir que o parâmetro não serve
//      para nada. Trazer o `CoreTiming` inteiro — que é o subsistema de
//      temporização do emulador — para usar uma referência que ninguém lê
//      seria levar o emulador atrás de uma assinatura vazia.
//
//   2. `Settings::IsDockedMode()` — que decide se o modo de performance é
//      `Normal` ou `Boost`. Este motor corre um jogo, não um console IVA, e o
//      jogo **não está docked**. A resposta é `Normal`, e está escrito como tal.
//
//   3. `SetClockSpeed` — que no Eden tambem nao faz nada (outro TODO).
//
// O que fica é o **estado observável**: o modo de performance, e a
// configuração de performance por modo. É isso que o SDK lê.
//
// ## O comportamento, o do Eden
//
// Medido no `apm_controller.cpp` do Eden:
//
//   - a configuração por omissão é `Config7` (que é 1020 MHz);
//   - `SetPerformanceConfiguration` valida a configuração contra a tabela de
//     frequências e ignora as inválidas;
//   - `GetCurrentPerformanceConfiguration` insere a omissão se o modo ainda
//     não tiver uma;
//   - `SetFromCpuBoostMode` mapeia `Normal`->`Config7`, `FastLoad`->`Config13`
//     e `Partial`->`Config15`.
//
// A tabela de frequências **não** é usada: `SetClockSpeed` no Eden é um TODO
// que não faz nada, e o nosso `SetClockSpeed` é o mesmo TODO, com a diferença
// de que nem sequer precisa de um relógio para o não chamar.
//
// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <map>

#include "vendor/eden/common/common_types.h"

namespace Service::APM {

enum class PerformanceConfiguration : u32 {
    Config1 = 0x00010000,
    Config2 = 0x00010001,
    Config3 = 0x00010002,
    Config4 = 0x00020000,
    Config5 = 0x00020001,
    Config6 = 0x00020002,
    Config7 = 0x00020003,
    Config8 = 0x00020004,
    Config9 = 0x00020005,
    Config10 = 0x00020006,
    Config11 = 0x92220007,
    Config12 = 0x92220008,
    Config13 = 0x92220009,
    Config14 = 0x9222000A,
    Config15 = 0x9222000B,
    Config16 = 0x9222000C,
};

// This is nn::oe::CpuBoostMode
enum class CpuBoostMode : u32 {
    Normal = 0,   // Boost mode disabled
    FastLoad = 1, // CPU + GPU -> Config 13, 14, 15, or 16
    Partial = 2,  // GPU Only -> Config 15 or 16
};

// This is nn::oe::PerformanceMode
enum class PerformanceMode : s32 {
    Invalid = -1,
    Normal = 0,
    Boost = 1,
};

// O estado da performance do sistema.
//
// A versão do Eden tem um `CoreTiming&` que nunca usa; aqui não há.
class Controller {
public:
    Controller();
    ~Controller();

    void SetPerformanceConfiguration(PerformanceMode mode, PerformanceConfiguration config);
    void SetFromCpuBoostMode(CpuBoostMode mode);

    PerformanceMode GetCurrentPerformanceMode() const;
    PerformanceConfiguration GetCurrentPerformanceConfiguration(PerformanceMode mode);

private:
    void SetClockSpeed(u32 mhz);

    std::map<PerformanceMode, PerformanceConfiguration> configs;
};

} // namespace Service::APM
