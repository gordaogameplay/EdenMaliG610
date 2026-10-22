// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// O `set:` — as preferencias do sistema, **nosso**.
//
// ## O que e'
//
// O `set:` (o "ISettingsServer" do Eden) e' o servico que responde ao
// `nn::settings` do SDK: a **lingua**, a **regiao**, o mapa de teclas, o
// apelido do dispositivo.
//
// ## Porque este ficheiro existe
//
// **Medido** (`/tmp/boot_r22.log`): o SDK pede `GetService("set")` ao
// `sm:`, e a casa **nao o tinha**:
//
//     Service: SM: GetService: nome lido = "set"
//     Service: SM: Server is not registered! service=set
//
// O `sm:` devolve `ResultNotRegistered` e o SDK **aborta** — o mesmo
// sintoma de sempre, o leitor CMIF a recusar a resposta
// (`SetTerminateResult modulo=10 desc=212`).
//
// Hmm! O `sm:` **difere** o pedido quando o servico nao existe, e o
// diferimento no motor nativo devolve uma resposta de 0 palavras — o
// guest le-a como um CMIF invalido. E' o mesmo caminho que o `fsp-srv`
// (ver `src/services/fsp_srv.cpp`) e que o `time:u`.
//
// A **cadeia** que se mediu no abort: o SDK le o idioma
// (`nn::settings::GetLanguageCode` -> o `am`, comando 21), e depois vai
// buscar o `set:` — e o `set:` nao existia.
//
// ## De onde vem a forma
//
// Do Eden: `core/hle/service/set/settings_server.cpp` (a tabela dos 13
// comandos) e `settings_types.h` (os codigos de lingua). Os **IDs** e os
// **nomes** sao copiados a mao, como nos outros servicos da casa — as
// tabelas **nao se inventam**.
//
// O **conteudo** e' o minimo: a lingua e' a inglesa (o jogo e' ingles) e a
// regiao e' a global. Tudo o que o jogo pede e' servido com a **mesma
// forma** que o Eden daria.
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "g610/services/set.hpp"
#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

namespace g610::services {

namespace {

using Service::HLERequestContext;

using IPC::RequestParser;
using IPC::ResponseBuilder;

// ─────────────────────────────────────────────────────────────────────────────
// As linguas
// ─────────────────────────────────────────────────────────────────────────────
//
// `nn::settings::LanguageCode` (o Eden, `settings_types.h:175`). O valor e'
// um u64 com o nome da lingua em ASCII nos ultimos bytes.
enum class LanguageCode : std::uint64_t {
    JA = 0x000000000000616A,       // "ja"
    EN_US = 0x00000053552D6E65,     // "en-US"
    FR = 0x0000000000007266,        // "fr"
    DE = 0x0000000000006564,        // "de"
    IT = 0x0000000000007469,        // "it"
    ES = 0x0000000000007365,        // "es"
    ZH_CN = 0x0000004E432D687A,     // "zh-CN"
    KO = 0x0000000000006F6B,        // "ko"
    NL = 0x0000000000006C6E,        // "nl"
    PT = 0x0000000000007470,        // "pt"
    RU = 0x0000000000007572,        // "ru"
    ZH_TW = 0x00000057542D687A,     // "zh-TW"
    EN_GB = 0x00000042472D6E65,     // "en-GB"
    FR_CA = 0x00000041432D7266,     // "fr-CA"
    ES_419 = 0x00003931342D7365,    // "es-419"
    ZH_HANS = 0x00736E61482D687A,   // "zh-Hans"
    ZH_HANT = 0x00746E61482D687A,   // "zh-Hant"
    PT_BR = 0x00000052422D7470,     // "pt-BR"
};

// A lista do Eden (`settings_types.h:251`), pela mesma ordem.
constexpr std::array<LanguageCode, 18> kLinguas = {{
    LanguageCode::JA,      LanguageCode::EN_US,   LanguageCode::FR,
    LanguageCode::DE,      LanguageCode::IT,     LanguageCode::ES,
    LanguageCode::ZH_CN,   LanguageCode::KO,     LanguageCode::NL,
    LanguageCode::PT,      LanguageCode::RU,     LanguageCode::ZH_TW,
    LanguageCode::EN_GB,   LanguageCode::FR_CA,  LanguageCode::ES_419,
    LanguageCode::ZH_HANS, LanguageCode::ZH_HANT, LanguageCode::PT_BR,
}};

// O Eden: `PRE_4_0_0_MAX_ENTRIES` = 0xF e `POST_4_0_0_MAX_ENTRIES` = 0x40.
constexpr std::int32_t kMaxEntradasAntigas = 0x0F;
constexpr std::int32_t kMaxEntradasNovas = 0x40;

// A lingua do sistema: **ingles** (o jogo e' ingles) — o indice 1 da lista
// do Eden, que e' o `EN_US`.
constexpr std::size_t kIndiceDaLingua = 1;

// A regiao: `PlatformRegion::Global` = 1 (o Eden, `settings_types.h`).
constexpr std::uint32_t kRegiaoGlobal = 1;

// ─────────────────────────────────────────────────────────────────────────────
// O servico
// ─────────────────────────────────────────────────────────────────────────────

class Set final : public Service::ServiceFramework<Set> {
public:
    explicit Set(Core::System& system) : ServiceFramework{system, "set"} {
        // A tabela e' a do Eden (`settings_server.cpp:93-108`) — os IDs e os
        // nomes **nao se inventam**.
        static const FunctionInfo functions[] = {
            {0, &Set::GetLanguageCode, "GetLanguageCode"},
            {1, &Set::GetAvailableLanguageCodes, "GetAvailableLanguageCodes"},
            {2, &Set::MakeLanguageCode, "MakeLanguageCode"},
            {3, &Set::GetAvailableLanguageCodeCount, "GetAvailableLanguageCodeCount"},
            {4, &Set::GetRegionCode, "GetRegionCode"},
            {5, &Set::GetAvailableLanguageCodes2, "GetAvailableLanguageCodes2"},
            {6, &Set::GetAvailableLanguageCodeCount2, "GetAvailableLanguageCodeCount2"},
            {7, nullptr, "GetKeyCodeMap"},
            {8, &Set::GetQuestFlag, "GetQuestFlag"},
            {9, nullptr, "GetKeyCodeMap2"},
            {10, nullptr, "GetFirmwareVersionForDebug"},
            {11, &Set::GetDeviceNickName, "GetDeviceNickName"},
            {12, nullptr, "GetKeyCodeMapByPort"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0: a lingua do sistema, como um `LanguageCode` (u64).
    void GetLanguageCode(HLERequestContext& ctx) {
        LOG_DEBUG(Service_SET, "GetLanguageCode: 1 (en-US)");
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<std::uint64_t>(kLinguas[kIndiceDaLingua]));
    }

    // 1: a lista de linguas disponiveis — a contagem e a lista num buffer
    // de saida.
    void GetAvailableLanguageCodes(HLERequestContext& ctx) {
        const std::size_t capacity = ctx.GetWriteBufferSize(0) / sizeof(std::uint64_t);
        const std::size_t n = std::min(kLinguas.size(), capacity);
        LOG_DEBUG(Service_SET, "GetAvailableLanguageCodes: {} linguas", n);
        std::vector<std::uint64_t> lista(n);
        for (std::size_t i = 0; i < n; ++i) {
            lista[i] = static_cast<std::uint64_t>(kLinguas[i]);
        }
        if (n > 0) {
            ctx.WriteBuffer(lista.data(), lista.size() * sizeof(std::uint64_t), 0);
        }
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<std::int32_t>(n));
    }

    // 2: o `LanguageCode` de um indice de lingua.
    void MakeLanguageCode(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto indice = rp.PopRaw<std::uint32_t>();
        ResponseBuilder rb{ctx, 4};
        if (indice >= kLinguas.size()) {
            LOG_WARNING(Service_SET, "MakeLanguageCode: indice {} invalido", indice);
            rb.Push(Result{ErrorModule::Settings, 625}); // `ResultInvalidLanguage`
            return;
        }
        rb.Push(ResultSuccess);
        rb.Push(static_cast<std::uint64_t>(kLinguas[indice]));
    }

    // 3: quantas linguas a versao antiga do SDK conhece.
    void GetAvailableLanguageCodeCount(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(kMaxEntradasAntigas);
    }

    // 4: a regiao do sistema.
    void GetRegionCode(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(kRegiaoGlobal);
    }

    // 5: a lista de linguas, na versao com o buffer mapeado.
    void GetAvailableLanguageCodes2(HLERequestContext& ctx) {
        const std::size_t capacity = ctx.GetWriteBufferSize(0) / sizeof(std::uint64_t);
        const std::size_t n = std::min(kLinguas.size(), capacity);
        LOG_DEBUG(Service_SET, "GetAvailableLanguageCodes2: {} linguas", n);
        std::vector<std::uint64_t> lista(n);
        for (std::size_t i = 0; i < n; ++i) {
            lista[i] = static_cast<std::uint64_t>(kLinguas[i]);
        }
        if (n > 0) {
            ctx.WriteBuffer(lista.data(), lista.size() * sizeof(std::uint64_t), 0);
        }
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<std::int32_t>(n));
    }

    // 6: quantas linguas a versao nova do SDK conhece.
    void GetAvailableLanguageCodeCount2(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(kMaxEntradasNovas);
    }

    // 8: a bandeira de "missao" (o Eden: `Out<bool>`).
    void GetQuestFlag(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<std::uint8_t>(0));
    }

    // 11: o apelido do dispositivo — o Eden devolve uma cadeia; a casa
    // devolve o que o Eden devolve (uma `SettingItemName` de 0x48 bytes)
    // com o nome vazio.
    void GetDeviceNickName(HLERequestContext& ctx) {
        const std::size_t capacity = ctx.GetWriteBufferSize(0);
        LOG_DEBUG(Service_SET, "GetDeviceNickName: {} bytes", capacity);
        if (capacity > 0) {
            std::vector<std::uint8_t> nome(capacity, 0);
            ctx.WriteBuffer(nome.data(), nome.size(), 0);
        }
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// O registo
// ─────────────────────────────────────────────────────────────────────────────

// Regista o `set:` no `sm:` — o mesmo caminho do `time:u`, do `appletOE` e
// do `fsp-srv`: um `ServerManager` proprio que fica no meta-registo do pump.
void registar_set(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("set", std::make_shared<Set>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
