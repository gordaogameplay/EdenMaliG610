#include "ia/gpu/maxwell_program.hpp"

namespace ia::gpu::maxwell {

namespace {

// Uma opacidade que existe e é útil para começar: `MOV`, que move um valor
// entre registos. Escolhida por nome e não por índice, porque o índice depende
// da ordem da tabela e essa ordem é um detalhe de outro ficheiro.
bool find_by_name(std::string_view nome, Op& out) noexcept {
    for (std::size_t i = 0; i < opcode_count(); ++i) {
        if (name_of(static_cast<Op>(i)) == nome) {
            out = static_cast<Op>(i);
            return true;
        }
    }
    return false;
}

} // namespace

Insn make(Op op, std::uint64_t payload) noexcept {
    const OpcodePattern p = pattern_of(op);

    // Os 16 bits do topo leva a assinatura; os 48 de baixo levam o payload,
    // com os bits da assinatura a zero para não os duplicar.
    const std::uint64_t sig = static_cast<std::uint64_t>(p.value) << 48;
    const std::uint64_t wild = static_cast<std::uint64_t>(~static_cast<std::uint64_t>(p.mask)) << 48;

    return sig | (payload & wild);
}

Program make_trivial_program() {
    Program p;
    p.stage = "fragment";

    // Este programa NÃO é um shader funcional — é um programmes sintético
    // com instruções válidas. A honestidade aqui importa: apresentar isto como
    // "um shader que pinta a textura" seria exactamente o tipo de tease que o
    // runtime antigo do piDroid tinha, e que este projecto já apanhou uma
    // vez. O que este programa serve é de validar o TABELA e o DECODIFICADOR,
    // e só isso até haver um caminho que ligue isto a SPIR-V.
    //
    // As opacidades são escolhidas de entre as que a tabela tem, por nome,
    // para não depender da ordem.
    static const char* const kOps[] = {
        "MOV", "FADD (reg)", "FMUL (reg)", "IADD3 (R0)", "LOP3 (reg)",
        "EXIT",
    };

    for (const char* nome : kOps) {
        Op op = Op::Invalid;
        if (!find_by_name(nome, op)) {
            // Não inventar: se o nome não existe, o programa fica mais curto
            // e o teste diz isso.
            continue;
        }
        p.code.push_back(make(op, 0));
    }
    return p;
}

Program make_burst_program(std::size_t n) {
    Program p;
    p.stage = "fragment";
    for (std::size_t i = 0; i < n; ++i) {
        const Op op = static_cast<Op>(i % opcode_count());
        p.code.push_back(make(op, 0));
    }
    return p;
}

} // namespace ia::gpu::maxwell