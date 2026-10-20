// EdenMaliIA — frontend: SASS Maxwell -> IR, para o subconjunto mínimo.
//
// A codificação dos operandos não é de memória. Sai de
// `shader_recompiler/frontend/maxwell/translate/impl/impl.cpp` do Eden, e cada
// campo tem a sua fonte:
//
//   Rd        bits [20:28)   GetReg20
//   Ra        bits [0:8)     (o simétrico de GetReg8 = [8:16))
//   Rb        bits [8:16)    GetReg8
//   Rc        bits [39:47)   GetReg39
//   cbuf      offset [20:34) em PALAVRAS, binding [34:39)
//             -> CbufAddr()
//   imm 20    valor [20:39), sinal no bit 56        GetImm20
//   imm 32    valor [20:52)                          GetImm32
//   predicado bits [16:20)
//
// Repara-se no detalhe do cbuf: o offset vem em PALAVRAS de 4 bytes, e o IR
// trabalha em BYTES. A conversão está na função certa e tem um teste — um
// factor de 4 perdido aqui desloca todas as constantes do shader, e o
// resultado é uma imagem com as cores trocadas em vez de um erro.

#include "ia/gpu/ir.hpp"
#include "ia/gpu/maxwell_isa.hpp"
#include "ia/gpu/maxwell_program.hpp"

#include <cstring>

namespace ia::gpu {

namespace {

using maxwell::Insn;

// ── campos de operando ────────────────────────────────────────────────────

constexpr std::uint8_t reg_a(Insn i)  noexcept { return static_cast<std::uint8_t>(i & 0xFF); }
constexpr std::uint8_t reg_b(Insn i)  noexcept { return static_cast<std::uint8_t>((i >> 8) & 0xFF); }
constexpr std::uint8_t reg_c(Insn i)  noexcept { return static_cast<std::uint8_t>((i >> 39) & 0xFF); }
constexpr std::uint8_t reg_d(Insn i)  noexcept { return static_cast<std::uint8_t>((i >> 20) & 0xFF); }

// c[bank][word_offset]
constexpr std::uint32_t cbuf_binding(Insn i) noexcept { return static_cast<std::uint32_t>((i >> 34) & 0x1F); }
constexpr std::uint32_t cbuf_word(Insn i)    noexcept { return static_cast<std::uint32_t>((i >> 20) & 0x3FFF); }

// imediato de 20 bits, com sinal (GetImm20)
std::uint64_t imm20(Insn i) noexcept {
    const std::uint64_t v = (i >> 20) & 0x7FFFFull;
    return ((i >> 56) & 1ull) ? (static_cast<std::uint64_t>(
                                    static_cast<std::int64_t>(v) - (1ll << 19)) &
                                0xFFFFFFFFull)
                              : v;
}

// imediato de 32 bits (GetImm32)
std::uint32_t imm32(Insn i) noexcept {
    return static_cast<std::uint32_t>((i >> 20) & 0xFFFFFFFFull);
}

ir::Value v_reg(std::uint8_t r, ir::Type t) {
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

// O DESTINO do MOV não está em [20:28).
//
// Nos aritméticos (FADD, FMUL, IADD3) o destino é `Rd` = [20:28) e a fonte
// esquerda é `Ra` = [0:8). No `MOV` é o contrário: o destino é [0:8) e a
// fonte é [20:28) (`MOV`'s union em move_register.cpp tem
// `BitField<0, 8> dest_reg`, e `MOV_reg` passa `GetReg20(insn)` como FONTE).
//
// Isto apanhou-nos: o frontend gerava `R16`, `R23` em vez de `R2`, `R3`, e o
// GLSL compilava na mesma. GLSL que compila com os registos errados é o pior
// tipo de bug — a imagem sai errada e ninguém percebe porquê.
//
// A lição vale para o resto: o layout dos operandos NÃO é uniforme entre
// opcodes, e supor que é é exactamente o erro que a regra do projecto proíbe.
constexpr std::uint8_t mov_dst(Insn i) noexcept { return reg_a(i); }

// NÃO se decide a origem do operando por um bit.
//
// `MOV (reg)` e `MOV (cbuf)` são opcodes distintos na tabela, e o frontend
// sabe qual é porque o descodificador o devolveu. Não há bit para ler.

} // namespace

ir::Program translate(const maxwell::Program& prog) {
    ir::Program out;
    out.stage = prog.stage;

    for (Insn insn : prog.code) {
        const maxwell::Op op = maxwell::decode(insn);
        ir::Instr n;
        n.from_opcode = std::string(maxwell::name_of(op));

        switch (op) {
        case maxwell::Op::MOV_reg:
        case maxwell::Op::MOV_cbuf: {
            n.op   = ir::Op::Mov;
            n.dst  = mov_dst(insn);
            n.type = ir::Type::U32;
            // `MOV (cbuf)` tem a sua propria entrada na tabela, por isso
            // aqui ja sabemos de onde vem. Nao ha bit para consultar.
            n.a    = (op == maxwell::Op::MOV_cbuf) ? v_cbuf(insn, ir::Type::U32)
                                                   : v_reg(reg_d(insn), ir::Type::U32);
            break;
        }
        case maxwell::Op::MOV_imm: {
            n.op   = ir::Op::Mov;
            n.dst  = mov_dst(insn);
            n.type = ir::Type::U32;
            n.a    = v_imm32(static_cast<std::uint32_t>(imm20(insn)), ir::Type::U32);
            break;
        }
        case maxwell::Op::MOV32I: {
            n.op   = ir::Op::Mov;
            n.dst  = mov_dst(insn);
            n.type = ir::Type::U32;
            n.a    = v_imm32(imm32(insn), ir::Type::U32);
            break;
        }
        case maxwell::Op::FADD_reg:
        case maxwell::Op::FADD_cbuf:
        case maxwell::Op::FADD_imm: {
            n.op   = ir::Op::FAdd;
            n.dst  = reg_d(insn);
            n.type = ir::Type::F32;
            n.a    = v_reg(reg_a(insn), ir::Type::F32);
            n.b    = (op == maxwell::Op::FADD_cbuf)  ? v_cbuf(insn, ir::Type::F32)
                    : (op == maxwell::Op::FADD_imm) ? v_imm32(static_cast<std::uint32_t>(imm20(insn)),
                                                            ir::Type::F32)
                                                    : v_reg(reg_b(insn), ir::Type::F32);
            break;
        }
        case maxwell::Op::FMUL_reg:
        case maxwell::Op::FMUL_cbuf:
        case maxwell::Op::FMUL_imm: {
            n.op   = ir::Op::FMul;
            n.dst  = reg_d(insn);
            n.type = ir::Type::F32;
            n.a    = v_reg(reg_a(insn), ir::Type::F32);
            n.b    = (op == maxwell::Op::FMUL_cbuf)  ? v_cbuf(insn, ir::Type::F32)
                    : (op == maxwell::Op::FMUL_imm) ? v_imm32(static_cast<std::uint32_t>(imm20(insn)),
                                                            ir::Type::F32)
                                                    : v_reg(reg_b(insn), ir::Type::F32);
            break;
        }
        case maxwell::Op::IADD3_reg:
        case maxwell::Op::IADD3_cbuf:
        case maxwell::Op::IADD3_imm: {
            n.op   = ir::Op::IAdd;
            n.dst  = reg_d(insn);
            n.type = ir::Type::U32;
            n.a    = v_reg(reg_a(insn), ir::Type::U32);
            n.b    = (op == maxwell::Op::IADD3_cbuf)  ? v_cbuf(insn, ir::Type::U32)
                    : (op == maxwell::Op::IADD3_imm) ? v_imm32(static_cast<std::uint32_t>(imm20(insn)),
                                                            ir::Type::U32)
                                                    : v_reg(reg_b(insn), ir::Type::U32);
            break;
        }
        // `IMAD_reg` NÃO entra no subconjunto, apesar de ser a forma canónica
        // de carregar uma constante. O destino do IMAD não está em [20:28) —
        // esse campo é o offset do banco — e a posição correcta não foi
        // confirmada. Adivinhá-la produziria constantes erradas com o GLSL a
        // compilar, que é a falha que custa mais caro a encontrar.
        //
        // Para o subconjunto mínimo usa-se `MOV (cbuf)`, cujo layout está
        // confirmado em move_register.cpp.
        case maxwell::Op::IMAD_reg: {
            n.op = ir::Op::Unknown;
            ++out.unsupported;
            out.code.push_back(n);
            continue;
        }
        case maxwell::Op::LOP3_reg:
        case maxwell::Op::LOP3_cbuf:
        case maxwell::Op::LOP3_imm: {
            // A máscara lógica está nos bits [20:29)? Não: no LOP3 a
            // operação vem nos bits [20:23) e `Rc` é o terceiro operando.
            // Sem confirmar o campo, NÃO se inventa: fica por fazer e conta
            // como não suportado.
            n.op = ir::Op::Unknown;
            ++out.unsupported;
            out.code.push_back(n);
            continue;
        }
        case maxwell::Op::EXIT:
            n.op = ir::Op::Exit;
            break;

        default:
            n.op = ir::Op::Unknown;
            ++out.unsupported;
            break;
        }

        out.code.push_back(n);
    }

    return out;
}

} // namespace ia::gpu