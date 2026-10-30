// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-2.0-only
// O registo do `fsp-srv`.
//
// O serviço em si vive em `src/services/fsp_srv.cpp`; aqui fica so a
// declaracao, porque quem o regista no `sm:` e' o `EdenSystem` (em
// `src/eden_bridge.cpp`).
//
// O porque deste servico existir esta' na cabeca do ficheiro da
// implementacao: o SDK pede o `fsp-srv` no arranque, e sem ele o `sm:`
// **defere** o pedido — o guest fica preso no `SendSyncRequest` e aborta.
#pragma once

namespace Core {
class System;
}

namespace g610::services {

// Regista o `fsp-srv` no `sm:` do sistema.
//
// O mesmo caminho que o `appletOE` e o `time:u`: um `ServerManager` proprio.
void registar_fsp_srv(Core::System& system);

} // namespace g610::services
