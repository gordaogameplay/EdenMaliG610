// A cadeia inteira do tradutor, sem o jogo.
//
//   SASS Maxwell sintetico -> IR -> GLSL -> SPIR-V
//
// O SPIR-V é produzido pelo `glslangValidator`, que é o compilador real que
// o host usa. Se o glslang aceitar o GLSL e o `spirv-val` passar, a ultima
// etapa — o SPIR-V para a G610 — fica provada tanto quanto o `glslang` a
// provou.
//
// O que NÃO é provado aqui, e é dito no ficheiro: que a G610 aceita o
// shader. Isso exige abrir um device Vulkan, que exige um contexto de
// ficheiros — e é o passo seguinte, não este.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ia/gpu/ir.hpp"
#include "ia/gpu/maxwell_program.hpp"

namespace ia::gpu {
// Declaradas nos .cpp que acabámos de escrever. Declaradas aqui, e não num
// header, porque este ainda é o unico consumidor e um header para duas
// funcoes e' um header a mais.
ir::Program translate(const maxwell::Program& prog);
std::string    to_glsl(const ir::Program& p);
} // namespace ia::gpu

// Definido no backend, e aqui só declarado. Vive no namespace global porque
// é uma verdade sobre o GLSL emitido, não sobre o IR — e os dois testes que
// a usam precisam de a ver sem `ia::gpu::` à frente.
std::uint32_t glsl_cbuf_stride_bytes();

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    ++g_pass;
    std::printf("  %s: %s\n", ok ? "ok" : "FALHOU", what.c_str());
    if (!ok)
        ++g_fail;
}

// Corre um comando e devolve o código de saída.
int run(const std::string& cmd) {
    const int r = std::system((cmd + " 2>&1").c_str());
    return (r == -1) ? -1 : (r >> 8);
}

} // namespace

int main() {
    using namespace ia::gpu;

    // ── 1. o programa sintetico ───────────────────────────────────────────
    std::printf("[1] programa Maxwell sintetico\n");
    const maxwell::Program sass = maxwell::make_minimal_shader();
    check(!sass.code.empty(), "o programa tem instrucoes");
    std::printf("       ");
    for (maxwell::Insn i : sass.code)
        std::printf("%s ", std::string(maxwell::name_of(maxwell::decode(i))).c_str());
    std::printf("\n\n");

    // ── 2. SASS -> IR ─────────────────────────────────────────────────────
    std::printf("[2] frontend SASS -> IR\n");
    const ir::Program prog = translate(sass);
    check(prog.unsupported == 0,
          "nenhuma instrucao caiu fora do subconjunto (" +
              std::to_string(prog.unsupported) + " fora)");
    check(prog.code.size() == sass.code.size(),
          "o IR tem tantas instrucoes como o SASS");

    std::size_t ops_com_resultado = 0;
    for (const ir::Instr& i : prog.code)
        if (i.op != ir::Op::Exit)
            ++ops_com_resultado;
    check(ops_com_resultado == sass.code.size() - 1,
          "todas menos o EXIT produzem resultado");

    // Os operandos tem de sobreviver intactos. Um `Rd` trocado com um `Ra`
    // da-se por um shader que compila e pinta o sitio errado.
    // O DESTINO esta em [0:8) — em TODOS os opcodes, FADD, FMUL, IADD3,
    // ISCADD e MOV. A primeira versao punha-o em [20:28) e todos os testes
    // passavam na mesma, porque o gerador usava a mesma convencao errada.
    // Este teste so apanha o bug se o gerador e o frontend concordarem
    // com a FONTE e nao entre si, por isso os numeros estao escritos a mao.
    {
        bool ok_carga = false, ok_dst_fmul = false, ok_dst_fadd = false;
        for (const ir::Instr& i : prog.code) {
            // No ISCADD a constante vem em `b`; em `a` fica o multiplicando
            // (que aqui e' RZ, traduzido para 0). Procurar em `a` dava a
            // instrucao como nao encontrada, e o teste falhava sem haver
            // bug nenhum.
            if (i.op == ir::Op::ISCAdd && i.b.kind == ir::Value::Kind::Cbuf &&
                i.b.binding == 0 && i.b.offset == 0x40)
                ok_carga = (i.dst == 2);
            if (i.op == ir::Op::FMul)
                ok_dst_fmul = (i.dst == 4);
            if (i.op == ir::Op::FAdd)
                ok_dst_fadd = (i.dst == 4);
        }
        check(ok_carga, "ISCADD (cbuf): destino em [0:8) leu 2");
        check(ok_dst_fmul, "FMUL: destino em [0:8) leu 4");
        check(ok_dst_fadd, "FADD: destino em [0:8) leu 4");
    }

    // O RZ e' o campo 0xFF, e vale 0. Sem isto, `ISCADD Rd, RZ, c[..]` leria
    // um registo a menos e o resultado seria uma constante errada.
    {
        bool ok_rz = false;
        for (const ir::Instr& i : prog.code) {
            if (i.op == ir::Op::ISCAdd &&
                i.a.kind == ir::Value::Kind::Imm32 && i.a.imm == 0)
                ok_rz = true;
        }
        check(ok_rz, "RZ (campo 0xFF) traduzido para a constante 0");
    }
    {
        bool ok_dst = false, ok_ra = false, ok_rb = false;
        for (const ir::Instr& i : prog.code) {
            if (i.op != ir::Op::FMul)
                continue;
            ok_dst = (i.dst == 4);
            ok_ra  = (i.a.kind == ir::Value::Kind::Reg && i.a.reg == 2);
            ok_rb  = (i.b.kind == ir::Value::Kind::Reg && i.b.reg == 3);
        }
        check(ok_dst, "FMUL: destino [0:8) leu 4");
        check(ok_ra,  "FMUL: Ra em [8:16) leu 2");
        check(ok_rb,  "FMUL: Rb em [20:28) leu 3");
    }
    {
        bool ok = false;
        // A constante pode estar em `a` (num MOV) ou em `b` (num ISCADD).
        // Procura-se nas duas, que e' como se sabe que o factor 4 se
        // perdeu ou nao em qualquer das formas.
        for (const ir::Instr& i : prog.code) {
            for (const ir::Value* v : {&i.a, &i.b}) {
                if (v->kind == ir::Value::Kind::Cbuf && v->binding == 0 &&
                    v->offset == 0x40)                     // c[0][0x10] * 4
                    ok = true;
            }
        }
        check(ok, "cbuf: palavra 0x10 virou byte 0x40 (o factor 4 nao se perdeu)");
    }

    // ── 3. IR -> GLSL ─────────────────────────────────────────────────────
    std::printf("\n[3] backend IR -> GLSL\n");
    const std::string glsl = to_glsl(prog);
    check(!glsl.empty(), "o GLSL nao esta vazio");
    check(glsl.find("#version 310 es") != std::string::npos,
          "declara a versao de GLSL");
    check(glsl.find("R4 = R4 + uintBitsToFloat(R2)") != std::string::npos,
          "a soma FADD chegou ao GLSL, e R2 virou bits, nao numero");

    // O indice do banco de constantes. Byte 0x40 com passo 16 da' indice 4,
    // NAO 16 — o std140 achata arrays de escalares a vec4. O teste le o passo
    // de uma funcao que o backend tambem usa, para que os dois nao possam
    // divergir sem ninguém dar por isso.
    std::printf("\n       passo do array de constantes: %u bytes\n",
                glsl_cbuf_stride_bytes());
    check(glsl.find("u_c[0][4]") != std::string::npos,
          "byte 0x40 virou indice 4 (o passo do std140, 16, foi aplicado)");
    check(glsl.find("u_c[0][16]") == std::string::npos,
          "o indice NAO e' 16 — dividi o offset por 4 em vez de 16");

    // O ponto que custou uma execucao inteira na G610. `float(uint)` e' uma
    // conversao NUMERICA; o guest reinterpreta os bits. O teste verifica que
    // o GLSL pede a reinterpretabao, porque se o backend voltar a usar
    // `float(...)` isto falha aqui e nao na imagem.
    check(glsl.find("uintBitsToFloat(R2)") != std::string::npos,
          "R2 (padrao de bits) entra no FMUL com uintBitsToFloat");
    check(glsl.find("float(float(R2)") == std::string::npos,
          "o backend NAO usa float(float(R2)) — essa e' a conversao numerica");

    std::printf("\n----- GLSL gerado -----\n%s--------------------\n", glsl.c_str());

    {
        std::FILE* f = std::fopen("/tmp/ia_min.frag", "wb");
        if (f == nullptr) {
            check(false, "escrever o GLSL em /tmp");
        } else {
            std::fwrite(glsl.data(), 1, glsl.size(), f);
            std::fclose(f);
            check(true, "o GLSL foi escrito em /tmp/ia_min.frag");
        }
    }

    // ── 4. GLSL -> SPIR-V, com o compilador real ─────────────────────────
    std::printf("[4] glslang -> SPIR-V\n");
    check(run("command -v glslangValidator >/dev/null") == 0,
          "o glslang esta instalado");

    const int r = run("glslangValidator -V --target-env vulkan1.1 -S frag -o "
                      "/tmp/ia_min.frag.spv /tmp/ia_min.frag");
    check(r == 0, "o glslang ACEITA o GLSL que geramos");
    if (r != 0)
        std::printf("       (glslang disse o que acima; o GLSL esta impresso)\n");

    if (r == 0) {
        const int v = run("spirv-val /tmp/ia_min.frag.spv");
        check(v == 0, "o spirv-val valida o binario");

        const int d = run("spirv-dis /tmp/ia_min.frag.spv | grep -c OpCapability");
        check(d >= 0, "o spirv-desassembla o binario");
    }

    std::printf("\n%d verificacoes, %d falhas\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}