// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — gerador de programas Maxwell sintéticos.
//
// O objectivo é ter SASS válido SEM o jogo, para validar o descodificador e,
// mais tarde, o tradutor. É a mesma disciplina que validou o motor de CPU:
// primeiro um guest sintético, depois o jogo real.
//
// A construção de uma instrução é a inversa do descodificador:
//
//     insn = (valor & mascara) << 48 | (payload & ~mascara) << 48
//
// ou seja: põe-se a assinatura nos 16 bits do topo e o payload nos 48 bits
// de baixo. Um round-trip que passa é a prova de que o descodificador e o
// gerador falam a mesma língua.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "g610/gpu/maxwell_isa.hpp"

namespace g610::gpu::maxwell {

// Um programa: uma lista de instruções e o cabeçalho que o Maxwell põe à
// frente. O formato exacto do cabeçalho é o que se vai descobrir a seguir; por
// agora só as instruções, porque é a única coisa que sabemos construir sem
// inventar.
struct Program {
    std::vector<Insn> code;
    std::string      stage;    // "vertex" | "fragment" | "compute"
};

// Constrói uma instrução que decodifica para `op`, com o `payload` nos 48
// bits de baixo.
Insn make(Op op, std::uint64_t payload) noexcept;

// Um programa mínimo de fragment shader: o caminho mais curto que ainda
// produz pixels, e portanto o primeiro teste end-to-end do tradutor.
Program make_trivial_program();

// Um programa de `n` instruções que percorre a tabela toda, para exercitar o
// descodificador em volume sem escrever um assembler.
Program make_burst_program(std::size_t n);

// Um programa real, dentro do subconjunto que o frontend suporta, e que
// produz pixels.
//
// ESTE é o programa que vai ao G610. É deliberadamente SEM texturas: a
// cadeia inteira — descodificar, IR, GLSL, SPIR-V, executar na Mali — prova-se
// com um shader que lê constantes e escreve uma cor. Texturas vêm depois, e
// quando vierem sabe-se que a cadeia já funcionava antes delas.
Program make_minimal_shader();

// Um programa escrito com operandos explícitos, para testar o codificador
// de operandos sem depender do gerador.
Program make_program(const std::vector<maxwell::Insn>& code,
                     const char* stage);

// Um programa que exercita as três formas do ISCADD — carga directa, escala
// e negação — porque um campo que ninguém escreve é um campo em que ninguém
// sabe se está no sítio certo.
Program make_iscadd_shader();

} // namespace g610::gpu::maxwell