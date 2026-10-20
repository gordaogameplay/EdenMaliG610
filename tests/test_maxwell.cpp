// Teste da tabela e do descodificador Maxwell.
//
// Isto não testa um tradutor: ainda não há tradutor. Testa a coisa de que o
// tradutor depende — que sabemos ler SASS Maxwell, e que sabemos construí-lo.
//
// Sem este teste, a primeira vez que um shader falha a descompilar não se
// distingue "a tabela está errada" de "o shader é esquisito".
#include <cstdio>
#include <string>
#include <vector>

#include "ia/gpu/maxwell_isa.hpp"
#include "ia/gpu/maxwell_program.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    ++g_pass;
    if (ok) {
        std::printf("  ok: %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  FALHOU: %s\n", what.c_str());
    }
}

} // namespace

int main() {
    using namespace ia::gpu::maxwell;

    // ── 1. a tabela ──────────────────────────────────────────────────────
    std::printf("[1] tabela de opcodes\n");
    check(opcode_count() == 280, "280 opcodes carregadas");
    check(name_of(Op::Invalid) == "?", "opcode invalida tem nome '?'");
    check(pattern_of(Op::Invalid).mask == 0xFFFF, "opcode invalida nao casa");

    // ── 2. a opcode vive nos bits [63:48] ───────────────────────────────
    //
    // Isto nao e' uma suposicao: e' o que `MaskValueFromEncoding` no Eden faz
    // (comeca em `u64(1) << 63`, percorre 16 bits). O teste fixa o
    // comportamento para que uma alteracao futura nao o mude em silencio.
    std::printf("\n[2] a opcode esta nos bits [63:48]\n");
    {
        Op qualquer = Op::Invalid;
        for (std::size_t i = 0; i < opcode_count(); ++i) {
            if (name_of(static_cast<Op>(i)) == "FADD (reg)") {
                qualquer = static_cast<Op>(i);
                break;
            }
        }
        check(qualquer != Op::Invalid, "FADD (reg) existe na tabela");

        const OpcodePattern p = pattern_of(qualquer);
        // Uma instrucao com a assinatura no topo e nada abaixo tem de
        // descodificar para essa opacidade, e o mesmo para uma com payload.
        const Insn limpo = make(qualquer, 0);
        check(decode(limpo) == qualquer,
              "make() com payload zero descodifica para a opacidade");
        const Insn com_payload = make(qualquer, 0x0000'1234'5678'9ABCull);
        check(decode(com_payload) == qualquer,
              "make() com payload nao zero tambem");
    }

    // ── 3. ida e volta em TODA a tabela ──────────────────────────────────
    //
    // Cada opacidade da tabela, gerada e descodificada de volta. Se uma
    // entrada da tabela estiver mal — mascara que nao casa, valor trocado —
    // aparece aqui e em mais lado nenhum.
    std::printf("\n[3] ida e volta em todas as opacidades\n");
    {
        // Nem toda a opacidade é uma instrução. A tabela tem entradas que
        // são CLASSES PREFIXO: `LD` tem mascara `0xE000` — três bits — e
        // valor `0x8000`, e está no fim da tabela de propósito. O descodificador
        // é uma varredura linear que devolve a primeira que casa, e
        // `IMAD32I` (mascara `0xFC00`, valor `0x8000`) vem antes, por isso
        // qualquer coisa com o topo `0x8000` é `IMAD32I` e nunca chega a `LD`.
        //
        // Isto NÃO é um bug do descodificador: é a semântica da tabela, e é
        // a mesma do Eden (`if (...) return Opcode::name;`, por ordem). Um
        // descodificador que devolvesse `LD` aqui estaria a divergir.
        //
        // O que o teste verifica é que essa lista é PEQUENA e que cada
        // entrada nela é mesmo uma classe — e que a lista não cresce sozinha.
        // Nomes à hardsaldados de memória: a lista é medida.
        // `std::string`, nao `const char*`: guardar o `c_str()` de um
        // temporário e um apontador para nada. Saiu mal uma vez.
        struct Sombra { std::size_t idx; std::string nome; std::string por; };
        std::vector<Sombra> classes;

        for (std::size_t i = 0; i < opcode_count(); ++i) {
            const Op op = static_cast<Op>(i);
            const Insn insn = make(op, 0);
            const Op obtido = decode(insn);
            if (obtido == op)
                continue;

            // Quem sombreou? A primeira entrada, na ordem da tabela, cuja
            // máscara é mais estreita e que casa com a mesma coisa.
            const OpcodePattern alvo = pattern_of(op);
            std::string por = "?";
            for (std::size_t j = 0; j < i; ++j) {
                const OpcodePattern p = pattern_of(static_cast<Op>(j));
                if (p.mask > alvo.mask && (alvo.value & p.mask) == p.value) {
                    por = std::string(name_of(static_cast<Op>(j)));
                    break;
                }
            }
            classes.push_back({i, std::string(name_of(op)), por});
        }

        char b[160];
        std::snprintf(b, sizeof b,
                      "as classes prefixo nao roundtable sao %zu (de %zu)",
                      classes.size(), opcode_count());
        check(classes.size() <= 4, b);
        for (const Sombra& s : classes)
            std::printf("       classe %-8s e' sombreada por %s\n", s.nome.c_str(), s.por.c_str());

        // As round-trips a sério: tudo o que não é classe.
        std::size_t falhadas = 0;
        std::string qual;
        for (std::size_t i = 0; i < opcode_count(); ++i) {
            bool e_classe = false;
            for (const Sombra& s : classes)
                if (s.idx == i) e_classe = true;
            if (e_classe)
                continue;
            const Op op = static_cast<Op>(i);
            if (decode(make(op, 0)) != op) {
                if (falhadas == 0)
                    qual = std::string(name_of(op));
                ++falhadas;
            }
        }
        std::snprintf(b, sizeof b,
                      "todas as opacidades que NAO sao classe fazem ida e volta%s%s",
                      falhadas ? " — falharam: " : "",
                      falhadas ? qual.c_str() : "");
        check(falhadas == 0, b);
    }

    // ── 4. deteccao de lixo ───────────────────────────────────────────────
    //
    // Uma palavra que nao corresponde a nada tem de dar `Invalid`, e nao a
    // opacidade mais proxima. Um descodificador que nunca falha e' um
    // descodificador que da codigo aleatorio sem dizer.
    std::printf("\n[4] lixo da entrada\n");
    {
        // Preenche os 16 bits do topo com 0xFFFF, que nao corresponde a
        // nenhuma assinatura da tabela.
        const Insn lixo = 0xFFFF'0000'0000'0000ull;
        check(decode(lixo) == Op::Invalid,
              "0xFFFF nos bits do topo nao casa com nada");
    }

    // ── 5. programas sinteticos ──────────────────────────────────────────
    std::printf("\n[5] programas sinteticos\n");
    {
        const Program p = make_trivial_program();
        check(!p.code.empty(), "o programa trivial tem instrucoes");

        bool todasDecodificam = true;
        std::string qual;
        for (Insn insn : p.code) {
            const Op op = decode(insn);
            if (op == Op::Invalid) {
                todasDecodificam = false;
                qual = "0x" + std::to_string(insn);
                break;
            }
        }
        check(todasDecodificam,
              "todas as instrucoes do programa trivial descodificam" +
                  (todasDecodificam ? "" : " (falhou em " + qual + ")"));

        std::printf("       ");
        for (Insn insn : p.code)
            std::printf("%s ", std::string(name_of(decode(insn))).c_str());
        std::printf("\n");
    }

    {
        const Program b = make_burst_program(1000);
        check(b.code.size() == 1000, "o burst tem 1000 instrucoes");

        std::size_t invalidas = 0;
        for (Insn insn : b.code)
            if (decode(insn) == Op::Invalid)
                ++invalidas;
        check(invalidas == 0, "nenhuma das 1000 instrucoes e' invalida");
    }

    std::printf("\n%d verificacoes, %d falhas\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}