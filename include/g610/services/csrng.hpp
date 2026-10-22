// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// O `csrng` — o gerador de bytes aleatorios.
//
// Ver `src/services/csrng.cpp` para o porque de este ficheiro existir.
#pragma once

#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso

namespace g610::services {

// Regista o `csrng` no `sm:`.
void registar_csrng(Core::System& system);

} // namespace g610::services
