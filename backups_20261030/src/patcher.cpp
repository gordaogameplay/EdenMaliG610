// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação do patcher estático.
#include "g610/patcher.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace g610 {

using arm::Disposition;

namespace {
// NOP arquitectural. WFI/YIELD/SEV são hints; neutralizá-los dá ao motor um
// ponto de yield determinístico e evita que o guest durma à espera de um
// evento que só o host pode tratar.
constexpr std::uint32_t kNop = 0xD503201Fu;
} // namespace

std::string PatchReport::summary() const {
    std::ostringstream o;
    o << "palavras varridas : " << words_scanned << "\n"
      << "SVC reescritos     : " << svc_patched;
    if (saw_any_svc)
        o << "  (primeiro: #" << first_svc << ")";
    o << "\n"
      << "hints -> NOP       : " << hints_nop << "\n"
      << "MRS / MSR vistos   : " << mrs_seen << " / " << msr_seen << "\n"
      << "MRS TPIDRRO->TPIDR : " << tpidrro_to_tpidr << "\n"
      << "MRS temporiz->trap : cntfrq " << mrs_cntfrq_to_trap
      << ", cntpct " << mrs_cntpct_to_trap
      << ", cntvct " << mrs_cntvct_to_trap << "\n"
      << "exclusivos vistos  : " << exclusives_seen << "\n";
    return o.str();
}

void StaticPatcher::clear() {
    report_ = PatchReport{};
    sites_.clear();
}

// Aplica a transformação a uma palavra. Devolve true se a palavra mudou.
//
// Exposto isoladamente para teste: a lógica de reescrita é pequena o suficiente
// para ser verificada instrução a instrução, e é exactamente o tipo de código
// em que um erro custa um salto para lixo no A76.
bool StaticPatcher::patch_word(std::uint32_t& insn, PatchSite& site) {
    site = PatchSite{};
    site.before = insn;
    site.kind = static_cast<std::uint8_t>(arm::classify(insn));

    switch (arm::classify(insn)) {
    case Disposition::PatchToBrk: {
        const arm::SVC svc{insn};
        site.svc = svc.number();
        insn = svc.to_brk();
        site.after = insn;
        return true;
    }
    case Disposition::TrapToHost: {
        // `MRS <Xt>, TPIDRRO_EL0` passa a `MRS <Xt>, TPIDR_EL0`. Um bit.
        //
        // O comentário que estava aqui dizia que reescrever MRS exigiria uma
        // sequência multi-instrução num sítio arbitrário, "mais arriscoso do
        // que tratar a trap". Era verdade para os MRS que precisavam de ser
        // emulados com um estado por thread — o Eden faz isso com
        // `MRS` + `LDR`, e daí 8 bytes no lugar de 4 e 25 MB a reposicionar.
        //
        // Este não precisa de estado nenhum: o registo de destino é o mesmo,
        // a largura é a mesma, e o valor que o guest quer é precisamente o
        // que `TPIDR_EL0` devolve — desde que o motor instale lá a base do
        // TLS do guest. Ver `NativeEngine`.
        // Os registos do temporizador do ARM viram **trap** ao host, nao
        // corre nativo: o `cntfrq` aqui devolve a frequencia (24_000_000
        // constante — o "agora" do SDK congela e o `WaitTimerEvent` da
        // parede nao vence, medido), e os contadores tem epoca do board,
        // estranha a escala 19,2 MHz que o SDK fixa de dentro. Ver o bloco
        // de comentarios em `instructions.hpp`: trap `brk #(0x40|Rt)`, o Rt
        // viagem no imediato e o handler responde com ticks do guest.
        if (arm::MRS{insn}.verify() &&
            (arm::is_mrs_cntfrq(insn) || arm::is_mrs_cntpct(insn) ||
             arm::is_mrs_cntvct(insn))) {
            insn = arm::mrs_tick_to_brk(insn);
            site.after = insn;
            return true;
        }
        if (arm::MRS{insn}.verify() && arm::is_mrs_tpidrro(insn)) {
            insn = arm::tpidrro_to_tpidr(insn);
            site.after = insn;
            return true;
        }
        // Os restantes MRS/MSR e os load/store exclusivos ficam como estão.
        site.after = insn;
        return false;
    }
    case Disposition::RunNatively:
        site.after = insn;
        return false;
    }
    return false;
}

bool StaticPatcher::patch(std::vector<std::uint8_t>& image,
                          std::uint64_t text_offset, std::uint64_t text_size,
                          PatchReport& report, std::string& error) {
    clear();

    // Limites coerentes: um NPDM adulterado ou cabeçalhos absurdos fariam
    // isto ler fora do vector.
    if (text_offset > image.size() || text_size > image.size() - text_offset) {
        error = "regiao de texto [" + std::to_string(text_offset) + ", +" +
                std::to_string(text_size) + ") fora da imagem de " +
                std::to_string(image.size()) + " bytes";
        return false;
    }
    if ((text_offset % 4) != 0 || (text_size % 4) != 0) {
        error = "regiao de texto nao alinhada a 4 bytes";
        return false;
    }

    const std::size_t first = static_cast<std::size_t>(text_offset);
    const std::size_t words = static_cast<std::size_t>(text_size) / 4;
    auto* base = image.data();

    // A contagem vai para o membro, e o relatório que o chamador passa é
    // apenas a semente — é preenchido no fim. Escrever em `report_` durante o
    // varrimento e devolver o parâmetro era a forma de os contadores se
    // perderem: o teste lia a cópia vazia.
    report_ = PatchReport{};
    report_.words_scanned = words;

    for (std::size_t i = 0; i < words; ++i) {
        const std::size_t off = first + i * 4;
        std::uint32_t insn;
        std::memcpy(&insn, base + off, sizeof(insn));

        const Disposition d = arm::classify(insn);
        if (d == Disposition::RunNatively)
            continue;

        // Hint antes da reescrita: patch_word não toca em hints.
        const bool is_hint = arm::Hint{insn}.verify();

        PatchSite site;
        bool changed = patch_word(insn, site);
        site.offset = off;

        if (is_hint) {
            insn = kNop;
            site.after = kNop;
            changed = true;
            ++report_.hints_nop;
        } else if (changed) {
            // Distingue o patch pelo encoding original: os registos do
            // temporizador (as traps 0x40|Rt do relogio do guest), o TLS,
            // e por fim os SVC.
            if (arm::is_mrs_cntfrq(site.before) ||
                arm::is_mrs_cntpct(site.before) ||
                arm::is_mrs_cntvct(site.before)) {
                if (arm::is_mrs_cntfrq(site.before))
                    ++report_.mrs_cntfrq_to_trap;
                else if (arm::is_mrs_cntpct(site.before))
                    ++report_.mrs_cntpct_to_trap;
                else
                    ++report_.mrs_cntvct_to_trap;
            } else if (arm::is_mrs_tpidrro(site.before)) {
                ++report_.tpidrro_to_tpidr;
            } else {
                ++report_.svc_patched;
                if (!report_.saw_any_svc) {
                    report_.first_svc = site.svc;
                    report_.saw_any_svc = true;
                }
            }
        } else {
            // Trap ao host: observa-se para o relatório, não se reescreve.
            if (arm::MRS{insn}.verify())       ++report_.mrs_seen;
            else if (arm::MSR{insn}.verify())  ++report_.msr_seen;
            else                               ++report_.exclusives_seen;
        }

        if (changed)
            std::memcpy(base + off, &insn, sizeof(insn));
        sites_.push_back(site);
    }

    // Devolver o relatório completo ao chamador.
    report = report_;

    tel_.exec().translated_blocks.fetch_add(1, std::memory_order_relaxed);
    error.clear();
    return true;
}

} // namespace g610
