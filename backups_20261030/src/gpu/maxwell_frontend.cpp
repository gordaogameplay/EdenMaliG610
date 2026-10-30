// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — frontend: SASS Maxwell -> IR, para o subconjunto mínimo.
//
// A codificação dos operandos não é de memória. Sai de três ficheiros do
// Eden, e cada campo tem a sua fonte:
//
//   Rd   (destino)   bits [ 0,  8)   FADD/FMUL/IADD3/ISCADD/MOV: TODOS
//                                        `BitField<0, 8, IR::Reg> dest_reg`
//   Ra   (fonte A)   bits [ 8, 16)   `BitField<8, 8> src_a`; GetReg8
//   predicado        bits [16, 20)
//   Rb   (fonte B)   bits [20, 28)   GetReg20
//   Rc   (fonte C)   bits [39, 47)   GetReg39 (terceiro operando do IADD3)
//
//   e o campo [20, 39) é o OPERANDO B, que conforme o opcode significa:
//     registo     -> Rb        em [20, 28)
//     constante   -> cbuf      offset [20, 34) em PALAVRAS, binding [34, 39)
//     imediato    -> imm20     valor [20, 39), sinal no bit 56
//
// ## O erro que este ficheiro já teve
//
// A primeira versão punha `Rd` em [20, 28) e `Ra` em [0, 8). Os testes
// passavam todos, porque o GERADOR de SASS sintética usava a mesma
// convenção errada. O teste concordava com o bug — que é a forma mais
// perigosa de um teste passar.
//
// Só apareceu ao ir confirmar a fonte, e a fonte é inequívoca: `FADD`,
// `FMUL`, `IADD3`, `ISCADD` e `MOV` têm TODOS `dest_reg` em `BitField<0, 8>`.
// Um layout de operandos que se supõe uniforme e não é — e que se descobre
// estar invertido, não só deslocado — é exactamente o que a regra do
// projecto proíbe: não inventar, ir à fonte.
//
// ## RZ
//
// `IR::Reg::RZ == 255` (`reg.h`, com `static_assert` e `NUM_USER_REGS = 255`).
// Um campo de registo com o valor 0xFF não é R255: é o registo zero. É isso
// que torna `ISCADD Rd, RZ, c[bank][off]` a forma canónica de carregar uma
// constante — o `0 << escala` desaparece e sobra a constante.

#include "g610/gpu/ir.hpp"
#include "g610/gpu/maxwell_isa.hpp"
#include "g610/gpu/maxwell_program.hpp"

#include <cstring>

namespace g610::gpu {

namespace {

using maxwell::Insn;

// ── campos de operando ────────────────────────────────────────────────────
//
// Nomes pelas Letters que o Eden usa: `GetReg20` é o campo em [20, 28) e
// não o "registo 20". Um nome que descreve a posição e não o papel evita
// exactamente a confusão que a primeira versão teve.

constexpr std::uint8_t reg_d(Insn i) noexcept { return static_cast<std::uint8_t>(i & 0xFF); }
constexpr std::uint8_t reg_a(Insn i) noexcept { return static_cast<std::uint8_t>((i >> 8) & 0xFF); }
constexpr std::uint8_t reg_b(Insn i) noexcept { return static_cast<std::uint8_t>((i >> 20) & 0xFF); }
constexpr std::uint8_t reg_c(Insn i) noexcept { return static_cast<std::uint8_t>((i >> 39) & 0xFF); }

// c[bank][word_offset]
constexpr std::uint32_t cbuf_binding(Insn i) noexcept { return static_cast<std::uint32_t>((i >> 34) & 0x1F); }
constexpr std::uint32_t cbuf_word(Insn i)    noexcept { return static_cast<std::uint32_t>((i >> 20) & 0x3FFF); }

// imediato de 20 bits, com sinal (GetImm20)
std::uint32_t imm20(Insn i) noexcept {
    const std::uint64_t v = (i >> 20) & 0x7FFFFull;
    if (((i >> 56) & 1ull) == 0)
        return static_cast<std::uint32_t>(v);
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(v) - (1ll << 19));
}

// imediato de 32 bits (GetImm32)
std::uint32_t imm32(Insn i) noexcept {
    return static_cast<std::uint32_t>((i >> 20) & 0xFFFFFFFFull);
}

// ISCADD: bits [39,44) escala, 47 cc, 48 neg_b, 49 neg_a.
constexpr std::uint8_t isc_shift(Insn i) noexcept { return static_cast<std::uint8_t>((i >> 39) & 0x1F); }
constexpr bool isc_neg_a(Insn i) noexcept { return ((i >> 49) & 1) != 0; }
constexpr bool isc_neg_b(Insn i) noexcept { return ((i >> 48) & 1) != 0; }

// O registo zero. Um campo de 8 bits com 0xFF é RZ, não R255.
constexpr bool e_registo_zero(std::uint8_t r) noexcept { return r == 0xFF; }

constexpr ir::Type kPadraoDeRegisto = ir::Type::U32;

ir::Value v_reg(std::uint8_t r, ir::Type t) {
    // RZ vale 0 em qualquer leitura. Traduzir para uma constante aqui, e não
    // no backend, é o que faz `ISCADD Rd, RZ, c[..]` cair no caminho simples.
    if (e_registo_zero(r)) {
        ir::Value v;
        v.kind = ir::Value::Kind::Imm32;
        v.imm  = 0;
        v.type = t;
        return v;
    }
    ir::Value v;
    v.kind = ir::Value::Kind::Reg;
    v.reg  = r;
    v.type = t;
    return v;
}

ir::Value v_imm32(std::uint32_t x, ir::Type t) {
    ir::Value v;
    v.kind = ir::Value::Kind::Imm32;
    v.imm  = x;
    v.type = t;
    return v;
}

ir::Value v_cbuf(Insn i, ir::Type t) {
    ir::Value v;
    v.kind    = ir::Value::Kind::Cbuf;
    v.binding = cbuf_binding(i);
    // palavras -> bytes. A conversão que, feita ao contrário, desloca tudo.
    v.offset  = cbuf_word(i) * 4u;
    v.type    = t;
    return v;
}

} // namespace

ir::Program translate(const maxwell::Program& prog) {
    ir::Program out;
    out.stage = prog.stage;

    // O tipo com que cada registo foi ESCRITO pela última vez.
    //
    // Maxwell não tem registos tipados, e é precisamente por isso que isto
    // é necessário: `MOV R2, c[0][0x10]` escreve um padrão de bits, e um
    // `FMUL R4, R2, R3` a seguir tem de saber que esses bits são um float e
    // não um inteiro. Sem este histórico, o backend ou converte o número e
    // obtém lixo, ou não compila. Que era o que acontecia.
    //
    // Começa em U32 porque é o que se pode assumir de um registo que ninguém
    // escreveu: um padrão de bits, que é o que o hardware tem.
    ir::Type reg_tipo[16];
    for (int i = 0; i < 16; ++i)
        reg_tipo[i] = kPadraoDeRegisto;

    for (Insn insn : prog.code) {
        const maxwell::Op op = maxwell::decode(insn);
        ir::Instr n;
        n.from_opcode = std::string(maxwell::name_of(op));

        switch (op) {
        // ── MOV ──────────────────────────────────────────────────────────
        // `MOV (reg)`: destino [0,8), fonte [20,28). É o mesmo layout do
        // `ISCADD` com RZ em A e escala 0 — mas é um opcode à parte, e
        // implementá-lo pelo `ISCADD` seria inventar uma equivalência que
        // ninguém mediu. Fica o seu.
        case maxwell::Op::MOV_reg:
        case maxwell::Op::MOV_cbuf: {
            n.op   = ir::Op::Mov;
            n.dst  = reg_d(insn);
            n.type = ir::Type::U32;
            n.a    = (op == maxwell::Op::MOV_cbuf) ? v_cbuf(insn, ir::Type::U32)
                                                   : v_reg(reg_b(insn), reg_tipo[reg_b(insn)]);
            break;
        }
        case maxwell::Op::MOV_imm: {
            n.op   = ir::Op::Mov;
            n.dst  = reg_d(insn);
            n.type = ir::Type::U32;
            n.a    = v_imm32(imm20(insn), ir::Type::U32);
            break;
        }
        case maxwell::Op::MOV32I: {
            n.op   = ir::Op::Mov;
            n.dst  = reg_d(insn);
            n.type = ir::Type::U32;
            n.a    = v_imm32(imm32(insn), ir::Type::U32);
            break;
        }

        // ── ISCADD: (A << escala) + B ───────────────────────────────────
        //
        // Este é o opcodes que os shaders da Switch usam, e é a razão de o
        // Eden implementar `ISCADD_*` e deixar `IMAD_*` em
        // `not_implemented.cpp`. Cobre, com o mesmo opcode:
        //
        //     ISCADD Rd, RZ, c[bank][off]   ->  carregar uma constante
        //     ISCADD Rd, RZ, Rb             ->  mover registo
        //     ISCADD Rd, Ra, c[bank][off]   ->  escala por uma potencia
        //
        // Os três caem no mesmo GLSL sem nenhum caso especial, porque o
        // RZ traduzido para 0 e `0 << n` é 0. É por isso que a tradução de
        // RZ para constante — e não um caso especial no backend — é a
        // decisão certa.
        case maxwell::Op::ISCADD_reg:
        case maxwell::Op::ISCADD_cbuf:
        case maxwell::Op::ISCADD_imm:
        case maxwell::Op::ISCADD32I: {
            n.op   = ir::Op::ISCAdd;
            n.dst  = reg_d(insn);
            n.type = ir::Type::U32;
            n.a    = v_reg(reg_a(insn), reg_tipo[reg_a(insn) & 0x0F]);
            n.shift = isc_shift(insn);
            n.neg_a = isc_neg_a(insn);
            n.neg_b = isc_neg_b(insn);

            switch (op) {
            case maxwell::Op::ISCADD_reg:
                n.b = v_reg(reg_b(insn), reg_tipo[reg_b(insn) & 0x0F]);
                break;
            case maxwell::Op::ISCADD_cbuf:
                n.b = v_cbuf(insn, ir::Type::U32);
                break;
            case maxwell::Op::ISCADD_imm:
            case maxwell::Op::ISCADD32I:
                n.b = v_imm32(op == maxwell::Op::ISCADD_imm ? imm20(insn) : imm32(insn),
                              ir::Type::U32);
                break;
            default:
                break;
            }

            // `neg_a` E `neg_b` juntos não são duas negações: é a forma
            // PREDICADA do opcode, e o Eden trata-a como `B + 1`. Copiar o
            // que o Eden faz é a escolha correcta — é o comportamento
            // medido contra hardware real — mas NÃO é o mesmo que predicado,
            // e o IR não tem predicados. Se um shader usar isto a sério, o
            // resultado vai estar errado e não vamos saber porquê. Está
            // escrito por causa disso.
            n.condicionado = n.neg_a && n.neg_b;
            break;
        }

        // ── FADD / FMUL ──────────────────────────────────────────────────
        case maxwell::Op::FADD_reg:
        case maxwell::Op::FADD_cbuf:
        case maxwell::Op::FADD_imm: {
            n.op   = ir::Op::FAdd;
            n.dst  = reg_d(insn);
            n.type = ir::Type::F32;
            n.a    = v_reg(reg_a(insn), reg_tipo[reg_a(insn) & 0x0F]);
            n.b    = (op == maxwell::Op::FADD_cbuf)  ? v_cbuf(insn, ir::Type::F32)
                    : (op == maxwell::Op::FADD_imm) ? v_imm32(imm20(insn), ir::Type::F32)
                                                    : v_reg(reg_b(insn), reg_tipo[reg_b(insn) & 0x0F]);
            break;
        }
        case maxwell::Op::FMUL_reg:
        case maxwell::Op::FMUL_cbuf:
        case maxwell::Op::FMUL_imm: {
            n.op   = ir::Op::FMul;
            n.dst  = reg_d(insn);
            n.type = ir::Type::F32;
            n.a    = v_reg(reg_a(insn), reg_tipo[reg_a(insn) & 0x0F]);
            n.b    = (op == maxwell::Op::FMUL_cbuf)  ? v_cbuf(insn, ir::Type::F32)
                    : (op == maxwell::Op::FMUL_imm) ? v_imm32(imm20(insn), ir::Type::F32)
                                                    : v_reg(reg_b(insn), reg_tipo[reg_b(insn) & 0x0F]);
            break;
        }

        // ── IADD3 ────────────────────────────────────────────────────────
        case maxwell::Op::IADD3_reg:
        case maxwell::Op::IADD3_cbuf:
        case maxwell::Op::IADD3_imm: {
            n.op   = ir::Op::IAdd;
            n.dst  = reg_d(insn);
            n.type = ir::Type::U32;
            n.a    = v_reg(reg_a(insn), reg_tipo[reg_a(insn) & 0x0F]);
            n.b    = (op == maxwell::Op::IADD3_cbuf)  ? v_cbuf(insn, ir::Type::U32)
                    : (op == maxwell::Op::IADD3_imm) ? v_imm32(imm20(insn), ir::Type::U32)
                                                    : v_reg(reg_b(insn), reg_tipo[reg_b(insn) & 0x0F]);
            break;
        }

        case maxwell::Op::EXIT:
            n.op = ir::Op::Exit;
            break;

        default:
            n.op = ir::Op::Unknown;
            ++out.unsupported;
            break;
        }

        // A instrução passa a ser a dona do tipo do destino.
        if (n.op != ir::Op::Exit && n.dst < 16)
            reg_tipo[n.dst] = n.type;

        out.code.push_back(n);
    }

    return out;
}

} // namespace g610::gpu