// EdenMaliIA — gerador de programas Maxwell sintéticos.
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

#include "ia/gpu/maxwell_isa.hpp"

namespace ia::gpu::maxwell {

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

} // namespace ia::gpu::maxwell