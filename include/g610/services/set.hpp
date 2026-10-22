// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// O `set:` — as preferencias do sistema (lingua, regiao, mapa de teclas).
//
// Ver `src/services/set.cpp` para o porque de este ficheiro existir.
#pragma once

#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso

namespace g610::services {

// Regista o `set:` no `sm:`.
void registar_set(Core::System& system);

} // namespace g610::services
