// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// O `pl:u` — o servico da plataforma (a fonte partilhada do sistema).
//
// Ver `src/services/pl_u.cpp` para o porque de este ficheiro existir.
#pragma once

#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso

namespace g610::services {

// Regista o `pl:u` no `sm:`.
void registar_pl_u(Core::System& system);

} // namespace g610::services
