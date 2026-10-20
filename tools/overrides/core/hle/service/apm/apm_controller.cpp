// O controller do `apm` — a nossa implementacao.
//
// Substitui `core/hle/service/apm/apm_controller.cpp` do Eden. A nota sobre o
// que muda esta no `apm_controller.h` ao lado.
//
// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <utility>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/hle/service/apm/apm_controller.h"

namespace Service::APM {

constexpr auto DEFAULT_PERFORMANCE_CONFIGURATION = PerformanceConfiguration::Config7;

Controller::Controller()
    : configs{
          {PerformanceMode::Normal, DEFAULT_PERFORMANCE_CONFIGURATION},
          {PerformanceMode::Boost, DEFAULT_PERFORMANCE_CONFIGURATION},
      } {}

Controller::~Controller() = default;

void Controller::SetPerformanceConfiguration(PerformanceMode mode,
                                           PerformanceConfiguration config) {
    static constexpr std::array<std::pair<PerformanceConfiguration, u32>, 16> config_to_speed{{
        {PerformanceConfiguration::Config1, 1020},
        {PerformanceConfiguration::Config2, 1020},
        {PerformanceConfiguration::Config3, 1224},
        {PerformanceConfiguration::Config4, 1020},
        {PerformanceConfiguration::Config5, 1020},
        {PerformanceConfiguration::Config6, 1224},
        {PerformanceConfiguration::Config7, 1020},
        {PerformanceConfiguration::Config8, 1020},
        {PerformanceConfiguration::Config9, 1020},
        {PerformanceConfiguration::Config10, 1020},
        {PerformanceConfiguration::Config11, 1020},
        {PerformanceConfiguration::Config12, 1020},
        {PerformanceConfiguration::Config13, 1785},
        {PerformanceConfiguration::Config14, 1785},
        {PerformanceConfiguration::Config15, 1020},
        {PerformanceConfiguration::Config16, 1020},
    }};

    const auto iter = std::find_if(config_to_speed.cbegin(), config_to_speed.cend(),
                                   [config](const auto& entry) { return entry.first == config; });

    if (iter == config_to_speed.cend()) {
        LOG_ERROR(Service_APM, "Invalid performance configuration value provided: {}", config);
        return;
    }

    SetClockSpeed(iter->second);
    configs.insert_or_assign(mode, config);
}

void Controller::SetFromCpuBoostMode(CpuBoostMode mode) {
    static constexpr std::array<PerformanceConfiguration, 3> BOOST_MODE_TO_CONFIG_MAP{{
        PerformanceConfiguration::Config7,
        PerformanceConfiguration::Config13,
        PerformanceConfiguration::Config15,
    }};

    SetPerformanceConfiguration(PerformanceMode::Boost,
                                BOOST_MODE_TO_CONFIG_MAP.at(static_cast<u32>(mode)));
}

PerformanceMode Controller::GetCurrentPerformanceMode() const {
    // `Settings::IsDockedMode()` no Eden. Aqui o console **nao esta docked**:
    // este motor corre um jogo, e o modo de performance e' o `Normal`.
    return PerformanceMode::Normal;
}

PerformanceConfiguration Controller::GetCurrentPerformanceConfiguration(PerformanceMode mode) {
    if (configs.find(mode) == configs.end()) {
        configs.insert_or_assign(mode, DEFAULT_PERFORMANCE_CONFIGURATION);
    }

    return configs[mode];
}

void Controller::SetClockSpeed(u32 mhz) {
    LOG_DEBUG(Service_APM, "called, mhz={:08X}", mhz);
    // O Eden tambem nao faz nada aqui (o TODO dele diz "Actually signal
    // core_timing to change clock speed"). Nao ha relogio para mudar, e nao ha
    // o que mudar: o clock do guest e' o clock do host.
}

} // namespace Service::APM
