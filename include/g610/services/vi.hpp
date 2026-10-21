// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O registo do `vi` (o servico de video / display).
//
// O servico em si vive em `src/services/vi.cpp`; aqui fica so a
// declaracao, porque quem o regista no `sm:` e' o `EdenSystem` (em
// `src/eden_bridge.cpp`).
//
// O porque deste servico existir esta' na cabeca do ficheiro da
// implementacao: o SDK pede o `vi` no arranque, e sem ele o `sm:` **defere**
// o pedido — o guest fica preso no `SendSyncRequest` e aborta.
#pragma once

namespace Core {
class System;
}

namespace g610::services {

// Regista o `vi` no `sm:` do sistema (os tres nomes: "vi:m", "vi:s",
// "vi:u"), como o Eden.
//
// O mesmo caminho que o `appletOE`, o `time:u`, o `fsp-srv` e o
// `nvdrv`: um `ServerManager` proprio.
void registar_vi(Core::System& system);

} // namespace g610::services
