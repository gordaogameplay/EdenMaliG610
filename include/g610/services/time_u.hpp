// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// O registo do `time:u`.
//
// O serviço em si vive em `src/services/time_u.cpp`; aqui fica só a
// declaração, porque o `EdenSystem` (em `src/eden_bridge.cpp`) é que o
// regista no `sm:`.
//
// O porque de este serviço existir está na cabeça do ficheiro da
// implementação: sem ele, o `sm:` **defere** o `GetService("time:u")`
// do arranque e o guest estaciona para sempre.
#pragma once

namespace Core {
class System;
}

namespace g610::services {

// Regista o `time:u` no `sm:` do sistema.
//
// O mesmo caminho que o `appletOE`: um `ServerManager` próprio.
void registar_time_u(Core::System& system);

} // namespace g610::services
