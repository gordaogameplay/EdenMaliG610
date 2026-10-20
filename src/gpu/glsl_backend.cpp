// EdenMaliIA — backend: IR -> GLSL.
//
// Porquê GLSL e não SPIR-V: o `glslang` está instalado no Pi e faz
// GLSL -> SPIR-V. Escrever um emissor de SPIR-V seria milhares de linhas para
// obter o mesmo binário. O GLSL é uma lingua de origem documentada, o que
// faz desta camada auditável — o que não seria verdade com binários.
//
// E há uma vantagem que vale mais: o GLSL é um artefacto que se pode ler. Um
// tradutor de shaders que ninguém consegue ler não é um tradutor, é uma
// máquina negra.

#include "ia/gpu/ir.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace ia::gpu {

namespace {

std::string reg_name(unsigned r) { return "R" + std::to_string(r); }

// Um literal float que o GLSL aceita.
//
// `0x3f800000` tem de sair `1.0`, e não `1065353216`.0 — que é o bug que um
// emissor ingenuo escreve e que só aparece como "a imagem está toda preta".
std::string f32_literal(std::uint32_t bits) {
    float v;
    std::memcpy(&v, &bits, sizeof v);
    char b[64];
    std::snprintf(b, sizeof b, "%.9gf", static_cast<double>(v));
    return b;
}

std::string u32_literal(std::uint32_t v) { return std::to_string(v) + "u"; }

// O banco de constantes vira um array de uniforms indexado por binding.
// O SPIR-V gerado pelo glslang mapeia isto para um UBO.
std::string cbuf_ref(const ir::Value& v) {
    return "u_c[" + std::to_string(v.binding) + "][" +
           std::to_string(v.offset / 4) + "]";
}

// O operando, convertido para o tipo que a INSTRUÇÃO espera.
//
// Isto não é cosmético. Em Maxwell um registo não tem tipo: R2 pode ser
// escrito por um `MOV` inteiro e lido por um `FMUL` como float. O GLSL
// não tem nada disso — `float + uint` é erro de compilação — e o glslang
// recusa com "wrong operand types".
//
// A primeira versão emitia `R4 + R2` e o GLSL não compilava. Compilar era o
// teste; se não houvesse teste, isto chegava ao host como um shader que não
// se consegue compilar, e a culpa parecia do PanVK.
const char* glsl_type(ir::Type t) {
    switch (t) {
    case ir::Type::F32: return "float";
    case ir::Type::F64: return "double";
    case ir::Type::U64: return "uint64_t";
    default:            return "uint";
    }
}


std::string operand(const ir::Value& v);

std::string operand_as(const ir::Value& v, ir::Type t) {
    return std::string(glsl_type(t)) + "(" + operand(v) + ")";
}

std::string operand(const ir::Value& v) {
    switch (v.kind) {
    case ir::Value::Kind::Reg:
        return reg_name(v.reg);
    case ir::Value::Kind::Imm32:
        return (v.type == ir::Type::F32) ? f32_literal(static_cast<std::uint32_t>(v.imm))
                                         : u32_literal(static_cast<std::uint32_t>(v.imm));
    case ir::Value::Kind::Imm64:
    case ir::Value::Kind::Cbuf:
        return cbuf_ref(v);
    }
    return "0u";
}

} // namespace

std::string to_glsl(const ir::Program& p) {
    std::ostringstream o;

    // `#version 310 es` — ES 3.1 é o mínimo para UBO com array e para
    // `layout(location=)` na saída. É o que o Vulkan 1.4 do G610 quer.
    o << "#version 310 es\n";
    o << "// Gerado por EdenMaliIA a partir de SASS Maxwell.\n";
    o << "// Subconjunto minimo: sem texturas, sem controlo de fluxo.\n";
    o << "// Estagio: " << p.stage << "\n\n";

    // O banco de constantes é um UBO por binding. Declara-se o maior que o
    // programa usa, e não um numero inventado.
    unsigned max_binding = 0;
    for (const ir::Instr& i : p.code) {
        for (const ir::Value* v : {&i.a, &i.b, &i.c}) {
            if (v->kind == ir::Value::Kind::Cbuf && v->binding > max_binding)
                max_binding = v->binding;
        }
    }
    // Qualidades de precisao: obrigatórias em GLSL ES. Sem elas o glslang
    // recusa com "type requires declaration of default precision qualifier" —
    // o que é o compilador a dizer que o shader não é para o hardware, e não
    // um detalhe de estilo.
    o << "precision highp float;\n";
    o << "precision highp int;\n\n";

    o << "layout(std140, binding = 0) uniform Ubo {\n";
    o << "    uint u_c[" << (max_binding + 1) << "][256];\n";
    o << "};\n\n";

    o << "layout(location = 0) out vec4 o_color;\n\n";
    o << "void main() {\n";

    // Cada registo é UMA variável, com o tipo da sua ÚLTIMA escrita.
    //
    // GLSL não tem SSA, e um registo Maxwell pode ser float num momento e
    // inteiro no seguinte. Declarar `uint R0` e `float R0` seguidas dá erro de
    // compilação; declarar só uma delas e assignment a outra sem conversão
    // dá o bug silencioso.
    //
    // Por isso: uma declaração por registo, e cada escrita põe uma conversão
    // explícita. Num subconjunto em que o registo mantém o tipo, a conversão
    // é uma no-op e o GLSL é o mesmo que se queria.
    ir::Type reg_type[16];
    for (int i = 0; i < 16; ++i)
        reg_type[i] = ir::Type::U32;

    for (const ir::Instr& i : p.code) {
        if (i.op == ir::Op::Exit || i.dst >= 16)
            continue;
        reg_type[i.dst] = i.type;
    }

    for (int r = 0; r < 16; ++r) {
        bool escrito = false;
        for (const ir::Instr& i : p.code)
            if (i.op != ir::Op::Exit && i.dst == r)
                escrito = true;
        if (!escrito)
            continue;
        o << "    " << glsl_type(reg_type[r]) << " " << reg_name(static_cast<unsigned>(r))
          << " = " << glsl_type(reg_type[r]) << "(0);\n";
    }

    for (const ir::Instr& i : p.code) {
        switch (i.op) {
        case ir::Op::Mov: {
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            o << "    " << reg_name(i.dst) << " = "
              << glsl_type(dt) << "(" << operand_as(i.a, dt) << ");\n";
            break;
        }
        case ir::Op::FAdd: {
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            o << "    " << reg_name(i.dst) << " = " << glsl_type(dt) << "("
              << operand_as(i.a, dt) << " + " << operand_as(i.b, dt) << ");\n";
            break;
        }
        case ir::Op::FMul: {
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            o << "    " << reg_name(i.dst) << " = " << glsl_type(dt) << "("
              << operand_as(i.a, dt) << " * " << operand_as(i.b, dt) << ");\n";
            break;
        }
        case ir::Op::IAdd: {
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            o << "    " << reg_name(i.dst) << " = " << glsl_type(dt) << "("
              << operand_as(i.a, dt) << " + " << operand_as(i.b, dt) << ");\n";
            break;
        }
        case ir::Op::StoreColor:
            o << "    o_color = vec4(" << operand(i.a) << ");\n";
            break;
        case ir::Op::Exit:
            break;
        case ir::Op::LOp3:
        case ir::Op::Unknown:
            // Não se escreve nada. Um operando inventado aqui produziria GLSL
            // válido e errado, que é a pior falha possível: compila, e a
            // imagem sai errada sem ninguém perceber porquê.
            break;
        }
    }

    // A saída de cor.
    //
    // O guest escreve o alvo com `ST.E.CON`, cujo encoding NÃO foi confirmado
    // e que por isso não está no subconjunto. Até lá, escreve-se o último
    // registo float calculado — que é o que um shader deste tipo produz.
    //
    // Está escrito porque um `o_color = vec4(R0)` com R0 não declarado é
    // GLSL que não compila, e `R0` era o que a primeira versão fazia.
    int ultimo_float = -1;
    for (const ir::Instr& i : p.code)
        if (i.op != ir::Op::Exit && i.type == ir::Type::F32 && i.dst < 16)
            ultimo_float = i.dst;
    if (ultimo_float >= 0)
        o << "    o_color = vec4(" << reg_name(static_cast<unsigned>(ultimo_float)) << ");\n";

    o << "}\n";
    return o.str();
}

} // namespace ia::gpu