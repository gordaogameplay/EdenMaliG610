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

#include <cstdint>

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

// Cria uma **camada gerida** e devolve o id.
//
// E' o que o `am` faz no `CreateManagedDisplayLayer` (o Eden:
// `core/hle/service/am/display_layer_manager.cpp:64`): abrir o display
// "Default" e criar a camada no `vi:m`.
//
// **Porquê que isto vive aqui e não no `am`**: o id da camada é um id do
// `vi` (o mesmo espaço de ids que o `vi:m` usa), e o `am` não tem o
// `vi`. A casa dá o id ao `am` por esta via.
//
// **Medido** (`/tmp/boot_r25.log`): com o `am` a devolver **zero**, o
// jogo usava o id como se fosse um ponteiro e faultava
// (`ldr w8, [x0, #0x38]` com `x0` = 0, em 0x8177d290).
std::uint64_t criar_camada_gerida();

} // namespace g610::services
