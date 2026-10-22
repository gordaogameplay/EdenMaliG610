// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// O `hid` — o servico de entrada (o comando partilhado do Switch).
//
// Ver `src/services/hid.cpp` para o porque de este ficheiro existir.
#pragma once

#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso

namespace g610::services {

// Regista o `hid` no `sm:`.
void registar_hid(Core::System& system);

} // namespace g610::services
