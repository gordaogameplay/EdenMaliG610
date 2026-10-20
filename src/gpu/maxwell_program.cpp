#include "g610/gpu/maxwell_program.hpp"

namespace g610::gpu::maxwell {

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

    // A assinatura ocupa os bits [63:48]. Os bits LIVRES do padrão (os `-`)
    // são onde o payload entra.
    //
    // A primeira versão calculava a máscara ao contrário — `payload & wild`
    // em vez de `payload & ~wild` — e o resultado era que TODOS os operandos
    // liam zero. O teste apanhou-o porque o `FMUL` aBdava `R0 = R0 * R0` em vez
    // de `R4 = R2 * R3`. Um tradutor que produz GLSL válido e com os
    // operandos todos a zero é o pior tipo de bug: compila, e a imagem sai
    // errada sem ninguém perceber porquê.
    const std::uint64_t sig  = static_cast<std::uint64_t>(p.value) << 48;
    const std::uint64_t wild = (static_cast<std::uint64_t>(~p.mask) & 0xFFFFull) << 48;

    return sig | (payload & ~wild);
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

Program make_program(const std::vector<Insn>& code, const char* stage) {
    Program p;
    p.code  = code;
    p.stage = stage;
    return p;
}

// Os operandos, com o layout CORRIGIDO.
//
// Rd = [ 0,  8)   Ra = [ 8, 16)   Rb = [20, 28)
// cbuf: offset [20, 34) em palavras, binding [34, 39)
// ISCADD: escala [39, 44), neg_b no 48, neg_a no 49
// imm20: valor [20, 39), sinal no bit 56   |   imm32: valor [20, 52)
//
// A versão anterior punha o destino em [20, 28) e o teste passava na mesma,
// porque o gerador e o frontend partilhavam a mesma convenção errada. Um
// teste que concorda com o bug não é um teste — é uma assinatura. Só a
// fonte desfaz isto, e a fonte é unânime: `dest_reg` é `BitField<0, 8>` em
// FADD, FMUL, IADD3, ISCADD e MOV.
namespace {

constexpr Insn put_reg(unsigned shift, unsigned nbits, unsigned value) {
    const Insn m = (nbits >= 64) ? ~0ull : ((1ull << nbits) - 1ull);
    return (static_cast<Insn>(value) & m) << shift;
}

// O registo zero. `IR::Reg::RZ == 255`, com static_assert no Eden.
constexpr unsigned kRegZero = 0xFF;

constexpr Insn put_cbuf(unsigned binding, unsigned word) {
    return (static_cast<Insn>(word & 0x3FFF) << 20) |
           (static_cast<Insn>(binding & 0x1F) << 34);
}

// O ISCADD: (A << escala) + B.
constexpr Insn iscadd(unsigned rd, unsigned ra, unsigned escala,
                      unsigned neg_a = 0, unsigned neg_b = 0) {
    return put_reg(0, 8, rd) | put_reg(8, 8, ra) |
           put_reg(39, 5, escala) | (static_cast<Insn>(neg_a) << 49) |
           (static_cast<Insn>(neg_b) << 48);
}

} // namespace

Program make_minimal_shader() {
    Program p;
    p.stage = "fragment";

    Op op = Op::Invalid;

    //  ISCADD R2, RZ, c[0][0x10]   -> R2 = 0x3F800000 (1.0f)
    //
    //  Esta é a forma como os shaders da Switch carregam uma constante: o
    //  multiplicando é o registo zero, a escala é 0, e o que fica é a
    //  constante. O `0 << 0` desaparece na aritmética e não precisa de
    //  nenhum caso especial em lado nenhum.
    if (find_by_name("ISCADD (cbuf)", op))
        p.code.push_back(make(op, iscadd(2, kRegZero, 0) | put_cbuf(0, 0x10)));

    //  ISCADD R3, RZ, c[0][0x14]   -> R3 = 0x3F000000 (0.5f)
    if (find_by_name("ISCADD (cbuf)", op))
        p.code.push_back(make(op, iscadd(3, kRegZero, 0) | put_cbuf(0, 0x14)));

    //  FMUL R4, R2, R3              -> R4 = 0.5
    if (find_by_name("FMUL (reg)", op))
        p.code.push_back(make(op, put_reg(0, 8, 4) | put_reg(8, 8, 2) |
                                   put_reg(20, 8, 3)));

    //  FADD R4, R4, R2              -> R4 = 1.5
    if (find_by_name("FADD (reg)", op))
        p.code.push_back(make(op, put_reg(0, 8, 4) | put_reg(8, 8, 4) |
                                   put_reg(20, 8, 2)));

    //  EXIT
    if (find_by_name("EXIT", op))
        p.code.push_back(make(op, 0));

    return p;
}

// Um programa que exercita AS TRÊS formas do ISCADD de uma vez, para que o
// teste não passe só porque a forma que ele usa é a mais fácil.
//
//     ISCADD R1, RZ, c[0][0x20]        -> R1 = c (carga directa)
//     ISCADD R2, R1, c[0][0x24], esc=2  -> R2 = c2 * 4 + c
//     ISCADD R3, RZ, 0x2A, neg_a        -> R3 = 0 - 0x2A
//
// O shift e a negação são campos que ninguém tinha testado. Um campo que
// ninguém exercita é um campo em que ninguém sabe se o bit está no sítio
// certo — e a única forma de saber é escrever a instrução e ver o número.
Program make_iscadd_shader() {
    Program p;
    p.stage = "fragment";
    Op op = Op::Invalid;

    if (find_by_name("ISCADD (cbuf)", op))
        p.code.push_back(make(op, iscadd(1, kRegZero, 0) | put_cbuf(0, 0x20)));

    if (find_by_name("ISCADD (cbuf)", op))
        p.code.push_back(make(op, iscadd(2, 1, 2) | put_cbuf(0, 0x24)));

    if (find_by_name("ISCADD (imm)", op))
        p.code.push_back(make(op, iscadd(3, kRegZero, 0, /*neg_a=*/1, /*neg_b=*/1) |
                                   (static_cast<Insn>(0x2A & 0x7FFFF) << 20)));

    if (find_by_name("EXIT", op))
        p.code.push_back(make(op, 0));

    return p;
}

} // namespace g610::gpu::maxwell