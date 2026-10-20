#include "ia/gpu/maxwell_isa.hpp"

#include <array>

namespace ia::gpu::maxwell {

namespace {

// Tabela de (nome, máscara, valor). O índice é o valor da opacidade.
constexpr std::array<OpcodePattern, 280> kPatterns = {{
#define MOP(name, cute, mask, value) OpcodePattern{mask, value},
#include "ia/gpu/maxwell_isa.inc"
#undef MOP
}};

constexpr std::array<std::string_view, 280> kNames = {{
#define MOP(name, cute, mask, value) cute,
#include "ia/gpu/maxwell_isa.inc"
#undef MOP
}};

// Os wildcards: os bits que NÃO fazem parte da assinatura.
//
// Numa instrução gerada por um sintetizador, os bits livres têm de levar
// alguma coisa. Zero é a escolha neutra e é o que o round-trip espera.
constexpr Insn kWildcardFill = 0;

} // namespace

Op decode(Insn insn) noexcept {
    const std::uint16_t top = top16(insn);
    for (std::size_t i = 0; i < kPatterns.size(); ++i) {
        if ((top & kPatterns[i].mask) == kPatterns[i].value)
            return static_cast<Op>(i);
    }
    return Op::Invalid;
}

std::string_view name_of(Op op) noexcept {
    const std::size_t i = static_cast<std::size_t>(op);
    if (i >= kNames.size())
        return "?";
    return kNames[i];
}

OpcodePattern pattern_of(Op op) noexcept {
    const std::size_t i = static_cast<std::size_t>(op);
    if (i >= kPatterns.size())
        return OpcodePattern{0xFFFF, 0xFFFF};
    return kPatterns[i];
}

std::size_t opcode_count() noexcept { return kPatterns.size(); }

} // namespace ia::gpu::maxwell