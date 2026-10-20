// EdenMaliG610 — backend: IR -> GLSL.
//
// Porquê GLSL e não SPIR-V: o `glslang` está instalado no Pi e faz
// GLSL -> SPIR-V. Escrever um emissor de SPIR-V seria milhares de linhas para
// obter o mesmo binário. O GLSL é uma lingua de origem documentada, o que
// faz desta camada auditável — o que não seria verdade com binários.
//
// E há uma vantagem que vale mais: o GLSL é um artefacto que se pode ler. Um
// tradutor de shaders que ninguém consegue ler não é um tradutor, é uma
// máquina negra.

#include "g610/gpu/ir.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace g610::gpu {

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

// O passo de um array de `uint` dentro de um UBO, em bytes.
//
// NÃO é 4. É 16.
//
// A primeira versão dividia o offset por 4 e achava que sim. Compilou,
// validou, executou na Mali-G610 — e devolveu 0.000000 em vez de 1.500000.
// O que falhou foi o seguinte, medido no SPIR-V gerado:
//
//     OpDecorate %_arr_uint_uint_256 ArrayStride 16
//
// O `std140` (que é o único layout de UBO que o Vulkan permite, se não for
// scalar block layout) ALINHA CADA ELEMENTO de um array de escalares num
// vec4. Um `uint[256]` ocupa 4096 bytes, não 1024, com três quartos de
// memoria a ser enchimento.
//
// E o pior: o erro não dava nenhum sinal. Zero é um número válido, a GLSL
// compilava, e o único sintoma era um pixel preto. Isto é a razão de este
// projecto medir o `ArrayStride` no binário em vez de o supor — ver o
// teste `gpu_exec`.
//
// Se a declaração do banco mudar, este número tem de mudar com ela. É por
// isso que a função existe em vez de ser um literal espalhado pelo ficheiro.
std::uint32_t cbuf_stride_bytes() noexcept { return 16; }

std::string cbuf_ref(const ir::Value& v) {
    return "u_c[" + std::to_string(v.binding) + "][" +
           std::to_string(v.offset / cbuf_stride_bytes()) + "]";
}

const char* glsl_type(ir::Type t) {
    switch (t) {
    case ir::Type::F32: return "float";
    case ir::Type::F64: return "double";
    case ir::Type::U64: return "uint64_t";
    default:            return "uint";
    }
}


std::string operand(const ir::Value& v);

// O operando convertido para o tipo que a INSTRUÇÃO espera.
//
// E aqui está uma distinção que separa um shader que funciona de um shader
// que devolve lixo.
//
// Em Maxwell um registo não tem tipo: `MOV R2, c[0][0x10]` escreve um PADRÃO
// DE BITS, e um `FMUL R4, R2, R3` a seguir interpreta esse registo como
// float — reinterpreta os 32 bits, não converte o número. O GLSL tem as duas
// coisas separadas:
//
//     float(x)           -> conversão NUMÉRICA: o inteiro 1065353216 vira
//                           o float 1065353216.0
//     uintBitsToFloat(x) -> reinterpretação de bits: vira 1.0
//
// A primeira versão usava `float()`. Compilava, o `spirv-val` validava,
// executava na Mali-G610, e devolvia 1126040644330979328.000000 em vez de
// 1.500000 — que é 1065353216.0 * 1056964608.0 + 1065353216.0, exactamente
// os dois padrões de bits (0x3F800000 e 0x3F000000) tratados como números.
//
// Vale a pena o que a medição revelou: o banco de constantes estava
// PERFEITO. Os valores que chegavam ao shader eram 0x3F800000 e 0x3F000000 —
// os dois certos, à mão e ao bit. O tradutor não tinha um problema de
// dados; tinha um problema de INTERPRETAÇÃO. E nenhum teste do tipo "os
// valores que chegam são os que se escreveram" o apanharia, porque os
// valores eram os certos.
// A negação de um operando do ISCADD.
//
// Para `uint` a negação é `(0 - x)`, que é a aritmética modular do tipo — a
// mesma que o hardware faz. Escrever `-x` também funcionaria, mas `0 - x` é
// explícito e não depende de o compilador decidir que sentido tem o menos.
std::string talvez_neg(const std::string& x, ir::Type t, bool neg) {
    if (!neg)
        return x;
    return "(0u - " + x + ")";
}

std::string converter(const ir::Value& v, ir::Type alvo) {
    const std::string base = operand(v);
    if (v.type == alvo)
        return base;

    // 32 bits reinterpretados. É o caso normal: um registo escrito por um MOV
    // de uma constante é um padrão de bits, não um número.
    if (v.type == ir::Type::U32 && alvo == ir::Type::F32)
        return "uintBitsToFloat(" + base + ")";
    if (v.type == ir::Type::F32 && alvo == ir::Type::U32)
        return "floatBitsToUint(" + base + ")";

    // Só para o que sobrar. Uma conversão numérica a sério.
    return std::string(glsl_type(alvo)) + "(" + base + ")";
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
    o << "// Gerado por EdenMaliG610 a partir de SASS Maxwell.\n";
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
            o << "    " << reg_name(i.dst) << " = " << converter(i.a, dt) << ";\n";
            break;
        }
        case ir::Op::FAdd: {
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            o << "    " << reg_name(i.dst) << " = "
              << converter(i.a, dt) << " + " << converter(i.b, dt) << ";\n";
            break;
        }
        case ir::Op::FMul: {
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            o << "    " << reg_name(i.dst) << " = "
              << converter(i.a, dt) << " * " << converter(i.b, dt) << ";\n";
            break;
        }
        case ir::Op::IAdd: {
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            o << "    " << reg_name(i.dst) << " = "
              << converter(i.a, dt) << " + " << converter(i.b, dt) << ";\n";
            break;
        }
        case ir::Op::ISCAdd: {
            // (A << escala) + B, com as modificações do opcode.
            //
            // `0 << n` é 0 no GLSL como em qualquer lado, por isso o RZ que o
            // frontend traduziu para constante 0 desaparece sozinho. É por
            // isso que `ISCADD Rd, RZ, c[..]` — a forma de carregar uma
            // constante nos shaders da Switch — não precisa de caso nenhum
            // aqui.
            const ir::Type dt = (i.dst < 16) ? reg_type[i.dst] : i.type;
            const std::string a = talvez_neg(converter(i.a, dt), dt, i.neg_a && !i.condicionado);
            std::string b = talvez_neg(converter(i.b, dt), dt, i.neg_b && !i.condicionado);
            if (i.condicionado)
                b = "(" + b + " + 1)";
            o << "    " << reg_name(i.dst) << " = ";
            if (i.shift != 0)
                o << "(" << a << " << " << static_cast<unsigned>(i.shift) << ") + ";
            else
                o << a << " + ";
            o << b << ";\n";
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

} // namespace g610::gpu

// A parte de fora do anonimo, para o teste poder usar o mesmo número que o
// backend usou. Um teste que recalcula a regra por conta própria está a
// testar a sua própria cópia da regra, e não a regra.
std::uint32_t glsl_cbuf_stride_bytes() noexcept {
    return ::g610::gpu::cbuf_stride_bytes();
}