// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — a ISA Maxwell, tal como a dapatmos medir.
//
// Isto NÃO é documentação nem memória. É a tabela de 280 opcodes com os seus
// padrões de bits, transcrita de `maxwell.inc` (ver a nota de proveniência no
// topo de `maxwell_isa.inc`).
//
// Porque importa: a NVIDIA nunca publicou a ISA Maxwell. Sem esta tabela não
// há descodificador possível — e sem descodificador não há tradutor para a
// Valhall. A tabela É a matéria-prima do tradutor.
//
// O que medimos, e não supusemos:
//   - a opcode ocupa os bits [63:48] da palavra de 64 bits, MSB primeiro
//     (`Shader::Maxwell::Decode`, Eden decode.cpp, com `MaskValueFromEncoding`
//     a começar em `u64(1) << 63` e a percorrer 16 bits);
//   - os wildcards são `-` dentro do padrão;
//   - o valor bruto de uma opcode é o SEU ÍNDICE na tabela, não um campo
//     numérico dentro da codificação.
#pragma once

#include <cstdint>
#include <string_view>

namespace g610::gpu::maxwell {

// Uma palavra SASS de 64 bits.
using Insn = std::uint64_t;

// Mascara e valor dos 16 bits que identificam a opcode, já no sítio deles
// (bits [63:48]). Reconstroem-se a partir de máscara/valor de 16 bits.
struct OpcodePattern {
    std::uint16_t mask;
    std::uint16_t value;
};

// O índice na tabela é o valor da opcode. Enumerar aqui dá ao compilador a
// chance de o confirmar contra a tabela.
enum class Op : std::uint16_t {
#define MOP(name, cute, mask, value) name,
#include "g610/gpu/maxwell_isa.inc"
#undef MOP
    Invalid = 0xFFFF,
};

// Os 16 bits do topo de `insn`, já reposicionados em [63:48].
// Devolvidos como (mascara, valor) para comparar com a tabela.
constexpr std::uint16_t top16(Insn insn) noexcept {
    return static_cast<std::uint16_t>(insn >> 48);
}

// Descodifica uma palavra SASS. `Op::Invalid` se nenhuma opacidade casar.
Op decode(Insn insn) noexcept;

// O nome legível ("FADD (reg)"), para diagnóstico.
std::string_view name_of(Op op) noexcept;

// O padrão (mascara, valor) de uma opacidade — usado pelo gerador sintético
// para construir uma instrução que decodifique para ela, e pelos testes de
// ida-e-volta.
OpcodePattern pattern_of(Op op) noexcept;

// Quantas opacidades a tabela tem. Só para testes: uma tabela que cresce sem
// ninguém dar conta é uma tabela que ninguém conhece.
std::size_t opcode_count() noexcept;

} // namespace g610::gpu::maxwell