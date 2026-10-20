// O registo do `appletOE`.
//
// O serviço em si vive em `src/services/applet_oe.cpp`; aqui fica só a
// declaração, porque o `EdenSystem` (em `src/eden_bridge.cpp`) é que o
// regista no `sm:`.
//
// Ver o ficheiro da implementação para o porque de este serviço ser nosso
// e não o do Eden.
#pragma once

namespace Core {
class System;
}

namespace g610::services {

// Regista o `appletOE` no `sm:` do sistema.
//
// O `sm:` é o `Service::SM::ServiceManager` do Eden, e o registo de um
// serviço faz-se por um `ServerManager` — o mesmo caminho que o `apm` e o
// `lm` usam.
void registar_applet_oe(Core::System& system);

} // namespace g610::services
