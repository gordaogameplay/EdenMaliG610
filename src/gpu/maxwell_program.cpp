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

// O programa que vai à G610.
//
// Cada linha tem o encoding do operando à vista. Estes campos não são de
// memória: `Rd`=[20:28), `Ra`=[0:8), `Rb`=[8:16), imediato 20=[20:39) com o
// sinal no bit 56, `c[bank][word]`=[20:34)/[34:39). Vem de
// `impl.cpp` no Eden (GetReg20, GetReg8, GetImm20, CbufAddr).
namespace {

constexpr Insn put_reg(unsigned shift, unsigned nbits, unsigned value) {
    const Insn m = (nbits >= 64) ? ~0ull : ((1ull << nbits) - 1ull);
    return (static_cast<Insn>(value) & m) << shift;
}

constexpr Insn put_imm20(std::uint64_t v) { return static_cast<Insn>(v & 0x7FFFFull) << 20; }
constexpr Insn put_imm32(std::uint32_t v) { return static_cast<Insn>(v) << 20; }

constexpr Insn put_cbuf(unsigned binding, unsigned word) {
    return (static_cast<Insn>(word & 0x3FFF) << 20) |
           (static_cast<Insn>(binding & 0x1F) << 34);
}

// NÃO se põe qualquer bit de "vou buscar a uma constante".
//
// `Instruction::is_cbuf` existe (instruction.h:36, bit 5) mas é para
// opcodes que TÊM as duas formas. `MOV (reg)` e `MOV (cbuf)` são entradas
// DISTINTAS na tabela, distinguidas pelos 16 bits de topo — é o próprio opcode
// que diz de onde vem o operando.
//
// Pôr o bit à mão foi tentado duas vezes e as duas deu errado: com o bit 0,
// `MOV R2, c[...]` escrevia em R3; com o bit 5, escrevia em R34. Em ambos os
// casos o GLSL compilava e a imagem saía errada. A lição não é "usar o bit
// certo" — é que **aqui não há bit para pôr**.
constexpr Insn cbuf_flag() { return 0; }

} // namespace

Program make_minimal_shader() {
    Program p;
    p.stage = "fragment";

    Op op = Op::Invalid;

    //  MOV R2, c[0][0x10]                        -> R2 = 0x3F800000 (1.0f)
    //
    //  O destino do MOV é [0:8) e NÃO [20:28) — em Maxwell o `MOV` tem um
    //  layout de operandos diferente do dos aritméticos. Confirmado em
    //  `move_register.cpp` (a union tem `BitField<0, 8> dest_reg`).
    if (find_by_name("MOV (cbuf)", op))
        p.code.push_back(make(op, put_reg(0, 8, 2) | put_cbuf(0, 0x10) | cbuf_flag()));

    //  MOV R3, c[0][0x14]                        -> R3 = 0.5f
    if (find_by_name("MOV (cbuf)", op))
        p.code.push_back(make(op, put_reg(0, 8, 3) | put_cbuf(0, 0x14) | cbuf_flag()));

    //  FMUL R4, R2, R3                           -> R4 = 0.5
    if (find_by_name("FMUL (reg)", op))
        p.code.push_back(make(op, put_reg(20, 8, 4) | put_reg(0, 8, 2) |
                                   put_reg(8, 8, 3)));

    //  FADD R4, R4, R2                           -> R4 = 1.5
    if (find_by_name("FADD (reg)", op))
        p.code.push_back(make(op, put_reg(20, 8, 4) | put_reg(0, 8, 4) |
                                   put_reg(8, 8, 2)));

    //  EXIT
    if (find_by_name("EXIT", op))
        p.code.push_back(make(op, 0));

    return p;
}

} // namespace ia::gpu::maxwell