// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O registo do `nvdrv`.
//
// O serviço em si vive em `src/services/nvdrv.cpp`; aqui fica so a
// declaracao, porque quem o regista no `sm:` e' o `EdenSystem` (em
// `src/eden_bridge.cpp`).
//
// O porque deste servico existir esta' na cabeca do ficheiro da
// implementacao: o SDK pede o `nvdrv` no arranque, e sem ele o `sm:`
// **defere** o pedido — o guest fica preso no `SendSyncRequest` e aborta
// no reader CMIF.
#pragma once

namespace Core {
class System;
}

namespace g610::services {

// Regista o `nvdrv` no `sm:` do sistema.
//
// O mesmo caminho que o `appletOE`, o `time:u` e o `fsp-srv`: um
// `ServerManager` proprio.
void registar_nvdrv(Core::System& system);

} // namespace g610::services
