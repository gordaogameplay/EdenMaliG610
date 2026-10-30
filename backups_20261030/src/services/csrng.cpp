// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// O `csrng` — o gerador de bytes aleatorios, **nosso**.
//
// ## O que e'
//
// O `csrng` e' o servico que o SDK usa para pedir entropia (`nn::csrng`).
// No Eden vive em `core/hle/service/spl/csrng.cpp` e tem **um** comando:
// `GenerateRandomBytes` (0), que enche o buffer de escrita do pedido com
// bytes aleatorios e devolve sucesso.
//
// ## Porque este ficheiro existe
//
// **Medido** (`/tmp/boot_hid.log`): depois do `hid`, o SDK pede
// `GetService("csrng")` e a casa nao o tinha. O `sm:` defere o pedido e o
// guest aborta no leitor CMIF:
//
//     Service::SM: GetService: nome lido = "csrng"
//     Service::SM: Server is not registered! service=csrng
//     Service::SM: Waiting for service csrng to become available
//     Service: Audio: SetTerminateResult: modulo=10 desc=212
//
// O Eden faz o mesmo que a casa vai fazer: `std::mt19937` e
// `ctx.WriteBuffer` (`spl_module.cpp:69-82`). Nao e' um CSPRNG de verdade,
// mas o guest so' precisa de bytes que nao se repitam.
#include "g610/services/csrng.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

namespace g610::services {

namespace {

using Service::HLERequestContext;
using IPC::ResponseBuilder;

class Csrng final : public Service::ServiceFramework<Csrng> {
public:
    explicit Csrng(Core::System& system) : ServiceFramework{system, "csrng"} {
        // A tabela e' a do Eden (`spl/csrng.cpp:10-15`).
        static const FunctionInfo functions[] = {
            {0, &Csrng::GenerateRandomBytes, "GenerateRandomBytes"},
        };
        RegisterHandlers(functions);
    }

private:
    void GenerateRandomBytes(HLERequestContext& ctx) {
        const std::size_t tamanho = ctx.GetWriteBufferSize();
        LOG_DEBUG(Service_SPL, "csrng::GenerateRandomBytes: {} bytes", tamanho);

        std::vector<std::uint8_t> dados(tamanho);
        std::uniform_int_distribution<std::uint16_t> distribuicao(
            0, std::numeric_limits<std::uint8_t>::max());
        std::generate(dados.begin(), dados.end(),
                      [&] { return static_cast<std::uint8_t>(distribuicao(rng_)); });

        ctx.WriteBuffer(dados);

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    std::mt19937 rng_{std::random_device{}()};
};

} // namespace

void registar_csrng(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("csrng", std::make_shared<Csrng>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
