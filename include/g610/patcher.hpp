// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — patcher estático.
//
// Corre uma única vez, no carregamento do programa. Percorre a região de
// texto, identifica as instruções que precisam de tratamento e reescreve-as
// no lugar. Depois disto, o código do guest executa sem qualquer tradução em
// tempo de execução: o A76 corre o binário.
//
// O que é reescrito:
//
//   SVC #n   →  BRK #n    (mesmo imm16, a trap leva o número do serviço)
//   WFI      →  NOP       (o host não pode dormir por causa de um guest)
//   YIELD    →  NOP
//   SEV      →  NOP
//
// MRS/MSR e os load/store exclusivos não são reescritos: são deixados como
// estão e tratados pelos signal handlers quando levantam trap. Reescrever
// MRS exigiria saber o registo destino em tempo de patching, o que é
// possível mas cria sequências de mais de uma instrução num sítio arbitrário
// — o que é mais arriscado do que tratar a trap.
//
// Regra de ouro: só se mexe no texto. Um literal `0xD40000C1` numa tabela de
// constantes casa com o encoding de `SVC #6`; se o patcher varresse dados,
// corromperia o programa de forma silenciosa.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "g610/instructions.hpp"
#include "g610/telemetry.hpp"

namespace g610 {

// Relatório do que o patcher fez. É informação de diagnóstico, não
// decoração: se o patcher não apanhar os SVC de um programa, o guest não
// arranca, e a contagem diz porquê.
struct PatchReport {
    std::uint64_t words_scanned   = 0;
    std::uint64_t svc_patched     = 0;
    std::uint64_t hints_nop       = 0; // WFI/YIELD/SEV neutralizados
    std::uint64_t mrs_seen        = 0;
    // `MRS <Xt>, TPIDRRO_EL0` reescritos como `MRS <Xt>, TPIDR_EL0`. Se for
    // 0 e o guest faultar a ler o TLS, o patch não foi aplicado.
    std::uint64_t tpidrro_to_tpidr = 0;

    // Quantas vezes `MRS <Xt>, CNTPCT_EL0` foi reescrito em
    // `MRS <Xt>, CNTVCT_EL0`. O `cntpct_el0` e' o contador fisico, que so o
    // EL1 le; o `cntvct_el0` e' o virtual, que o Linux expoe. Ver
    // `instructions.hpp`.
    std::uint64_t cntpct_to_cntvct = 0;
    std::uint64_t msr_seen        = 0;
    std::uint64_t exclusives_seen = 0;
    std::uint32_t first_svc       = 0;
    bool          saw_any_svc     = false;

    std::string summary() const;
};

// Uma ocorrência encontrada.
struct PatchSite {
    std::uint64_t offset{};   // byte dentro da imagem
    std::uint32_t before{};
    std::uint32_t after{};
    std::uint32_t svc{};      // número, para SVC
    std::uint8_t  kind{};     // arm::Disposition
};

class StaticPatcher {
public:
    explicit StaticPatcher(tel::Telemetry& tel) : tel_(tel) {}

    // Reescreve a imagem in-place. `text_offset`/`text_size` delimitam a região
    // de código dentro de `image`. Devolve false com `error` se os limites
    // não forem coerentes.
    //
    // `report` recebe o resultado do varrimento: é preenchido aqui e
    // copiado para o membro interno. Passar o relatório como saída (e não
    // apenas devolvê-lo pelo accessor) é deliberado — quem chama precisa do
    // número para decidir se o programa é executável, e o accessor obriga a
    // uma segunda chamada.
    bool patch(std::vector<std::uint8_t>& image,
               std::uint64_t text_offset, std::uint64_t text_size,
               PatchReport& report, std::string& error);

    // Patch de uma única instrução, exposto para teste.
    static bool patch_word(std::uint32_t& insn, PatchSite& site);

    const std::vector<PatchSite>& sites() const { return sites_; }
    const PatchReport& report() const { return report_; }
    void clear();

private:
    tel::Telemetry&       tel_;
    PatchReport           report_{};
    std::vector<PatchSite> sites_;
};

} // namespace g610
