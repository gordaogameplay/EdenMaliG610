// EdenMaliIA — traçador do arranque real.
//
// O rtld é código nativo AArch64 e está pronto a correr. A questão é: que
// serviços é que ele pede, e por que ordem? Esta é a forma de responder —
// letting o código real dizer, em vez de adivinhar a partir de documentação.
//
// Difícil de saber de memória. O Switch tem ~60 SVC e o rtld pode chamar
// qualquer um deles no arranque. Adivinhar o implementados primeiro e ver o
// que falha dá uma sequência de tentativas; traçar dá a lista.
//
// O que este programa faz:
//
//   1. carrega o ExeFS (rtld + main + sdk) e mapeia o espaço de 39 bits
//   2. corre o entry point do rtld com handlers de sinais instalados
//   3. em cada SVC, regista número + nome + argumentos e devolve um valor
//      plausível
//   4. para ao primeiro Unsupported, e reporta onde o guest estava
//
// O passo 3 é o ponto: devolver Success a tudo faz o rtld avançar para o
// serviço seguinte, e o relatório mostra a sequência completa até bloquear.
//
// isto NÃO é o emulador. É um instrumento de diagnóstico. A diferença é
// óbvia nos comentários: aqui não há semântica, há medições.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <csignal>
#include <sys/time.h>
#include <signal.h>
#include <ucontext.h>

#include "ia/guest_program.hpp"
#include "ia/guest_memory.hpp"
#include "ia/native_engine.hpp"
#include "ia/svc_dispatcher.hpp"
#include "ia/telemetry.hpp"

namespace {

// Registo de cada SVC pedido, na ordem.
struct SvcCall {
    std::uint32_t number;
    std::string   name;
    std::uint64_t x[6];
    std::uint32_t result;
    std::uint64_t pc;
    std::uint64_t sp;
};

std::vector<SvcCall> g_calls;

// Trace uma função de escrita async-signal-safe (write(2)), porque isto
// corre DENTRO de um handler de sinais.
void trace(const char* msg) {
    const std::size_t n = std::strlen(msg);
    ssize_t w = ::write(2, msg, n);
    (void)w;
}

// Interrompe o guest depois de N chamadas: um rtld que faz 10 000 pedidos
// não está a arrancar, está em ciclo, e esperar por ele não ajuda ninguém.
constexpr std::size_t kMaxCalls = 400;

} // namespace

// Amostra o PC do guest com um temporizador.
//
// Responde a uma pergunta que nenhuma trap responde: onde esta' o guest quando
// NAO esta' a levantar excepcoes? Um guest em ciclo de codigo puro nao passa
// pelo handler de sinais, e sem amostragem so' se sabe que "nao voltou" — que
// e' o sintoma mais opaco que existe.
//
// O sinal e' entregue ao processo e, como o guest corre na sua propria stack,
// o handler ve' o contexto do GUEST. E' o mesmo mecanismo dos SVC, visto por um
// caminho diferente.
using GuestKind = ia::GuestModule::Kind;

const ia::GuestModule* find(const ia::GuestProgram& p, GuestKind k) {
    for (const auto& m : p.modules())
        if (m.kind == k)
            return &m;
    return nullptr;
}

static bool g_hooks_dump_slots = false;

// Quando verdadeiro, o traçador cala-se: usado pelos testes de arranque,
// que só querem o resultado e não o despejo das tabelas.
static bool g_slots_quiet = false;
static std::vector<std::uint64_t> g_slot_offsets;
// Ponteiros que o observer usa para ler memória do guest. Postos pelo main.
static ia::GuestMemory* g_mem = nullptr;
static std::string g_scratch;

static volatile int g_samples = 0;
static volatile std::uint64_t g_sample_pc = 0;
// Salta para aqui para parar o guest: um BRK, que o motor reconhece.
extern "C" void force_stop() { __asm__ volatile("brk #0xFFFC"); }

void on_alarm(int, siginfo_t*, void* raw) {
    auto* uc = static_cast<ucontext_t*>(raw);
    const auto pc = static_cast<std::uint64_t>(uc->uc_mcontext.pc);
    g_sample_pc = pc;
    ++g_samples;

    // Na ultima amostragem, PARAR o guest. Um spin de codigo nao levanta
    // traps, e sem isto o programa nunca mais sai: o relatorio (que e' onde
    // esta' o unico rasto do que aconteceu) nunca e' impresso.
    if (g_samples >= 4) {
        // Parar o guest: salta para uma rotina que faz BRK, que o motor
        // trata como saida. Sem isto o programa nunca mais volta ao host e o
        // relatorio — o unico rasto do que aconteceu — nunca e' impresso.
        uc->uc_mcontext.pc = reinterpret_cast<std::uint64_t>(&force_stop);
    }
    if (g_samples <= 2) {
        char b[208];
        const int k = std::snprintf(
            b, sizeof b,
            "[sample %d] pc=0x%llx x0=0x%llx x1=0x%llx x19=0x%llx x29=0x%llx sp=0x%llx\n",
            g_samples, (unsigned long long)pc,
            (unsigned long long)uc->uc_mcontext.regs[0],
            (unsigned long long)uc->uc_mcontext.regs[1],
            (unsigned long long)uc->uc_mcontext.regs[19],
            (unsigned long long)uc->uc_mcontext.regs[29],
            (unsigned long long)uc->uc_mcontext.sp);
        ssize_t w = ::write(2, b, (size_t)k);
        (void)w;
    }
}

// Vê cada trap. Se o guest não levantar nenhuma, o PC nunca aparece — e sem
// o PC não se sabe se está parado ou em ciclo.
static volatile std::uint64_t g_last_pc = 0;
static volatile int g_traps = 0;

void on_trap(int sig, std::uint64_t pc, std::uint64_t sp, void* user) {
    g_last_pc = pc;
    ++g_traps;
    // Só imprime as primeiras: um guest em ciclo levanta milhares.
    if (g_traps <= 40) {
        // A instrução no PC é a informação que decide tudo: um BRK nosso é um
        // SVC em disguise (e o observer deveria ter registado), e outra coisa
        // é uma trap que o handler não sabe tratar.
        static std::uint32_t g_insn = 0;
        if (pc != 0)
            std::memcpy(&g_insn, reinterpret_cast<const void*>(pc), 4);

        char b[192];
        const int k = std::snprintf(
            b, sizeof b,
            "[trap #%d] sig=%d pc=0x%llx insn=0x%08x %s sp=0x%llx\n", g_traps,
            sig, (unsigned long long)pc, g_insn,
            ((g_insn & 0xFFE00000u) == 0xD4200000u) ? "(BRK = SVC patchado)" : "",
            (unsigned long long)sp);
        ssize_t w = ::write(2, b, (size_t)k);
        (void)w;
    }
    (void)user;
}

// Callback do motor. Assinatura async-signal-safe: só escreve em memória
// (o vector) e num buffer fixo. Nenhuma alocação, nenhum printf.
static volatile int g_svcs = 0;

void on_svc(std::uint32_t number, ia::SvcContext& ctx, std::uint64_t pc,
            std::uint64_t sp, void* user) {
    ++g_svcs;
    SvcCall c{};
    c.number = number;
    c.name   = ia::to_string(static_cast<ia::Svc>(number));
    for (int i = 0; i < 6; ++i)
        c.x[i] = ctx.x[i];
    c.pc = pc;
    c.sp = sp;
    g_calls.push_back(c);
    (void)user;
    // Confirma que o observer corre DENTRO do handler. Sem isto, e sem o
    // relatório (que só imprime quando o guest sai), um SVC despachado sem
    // guest parado não deixa rasto nenhum.
    // Dump da stack: fica para o dump textual em main(), onde o
    // GuestProgram está acessível. Aqui só se guarda o sp.
    if (g_svcs <= 1) {
        char b[32];
        const int k = std::snprintf(b, sizeof b, "[stk] sp=0x%llx\n",
                                    (unsigned long long)sp);
        ssize_t w = ::write(2, b, (size_t)k); (void)w;
    }

    if (number == 0x27 && ctx.x[1] > 0 && ctx.x[1] < 512) {
        // OutputDebugString: mostrar a string. É o registo mais próximo que o
        // guest dá sobre o que está a fazer.
        char b[600];
        char txt[520];
        const std::size_t n = ctx.x[1] < 500 ? ctx.x[1] : 500;
        bool ok = false;
        if (g_mem != nullptr)
            ok = g_mem->read(ctx.x[2], txt, n, g_scratch);
        if (ok) {
            txt[n] = 0;
            const int k = std::snprintf(b, sizeof b, "[dbg ] \"%s\"\n", txt);
            ssize_t w = ::write(2, b, (size_t)k);
            (void)w;
        }
    }

    if (g_svcs <= 4 && number == 0x06) {
        // O QueryMemory escreve 40 bytes no stack do guest. Mostrar o que
        // ficou lá é a forma de saber se a resposta é plausível — e um
        // endereço todo a zero é o sintoma de "não escrevemos nada".
        char b[192];
        const int k = std::snprintf(
            b, sizeof b,
            "[qm  ] out_info=0x%llx out_page=0x%llx query=0x%llx\n",
            (unsigned long long)ctx.x[0], (unsigned long long)ctx.x[1],
            (unsigned long long)ctx.x[2]);
        ssize_t w = ::write(2, b, (size_t)k);
        (void)w;
    }

    // gpr[] e não x[]: x[] tem 6 entradas e x[18] seria memória a mais.
    char b[320];
    const int k = std::snprintf(b, sizeof b,
        "[svc #%u] 0x%02x %-18s x0=0x%llx x1=0x%llx x2=0x%llx x18=0x%llx x30=0x%llx\n",
        g_svcs, number, c.name.c_str(),
        (unsigned long long)ctx.x[0], (unsigned long long)ctx.x[1],
        (unsigned long long)ctx.x[2], (unsigned long long)ctx.gpr[18],
        (unsigned long long)ctx.gpr[30]);
    ssize_t w = ::write(2, b, (size_t)k);
    (void)w;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("uso: %s <exefs_dir>\n", argv[0]);
        return 2;
    }
    const std::string exefs = argv[1];

    std::printf("══════ arranque real do rtld ══════\n");
    std::printf("  exefs: %s\n\n", exefs.c_str());

    ia::tel::Telemetry tel;
    tel.set_live_console(false);
    tel.start("0000000000000000", "boot-trace");

    // ── 1. carregar e mapear ──────────────────────────────────────────────
    ia::GuestProgram prog(tel);
    std::string err;

    // O rtld recebe argv[0] como primeiro argumento. Sem isto lê lixo.
    prog.set_program_args({"main"});

    if (!prog.load(exefs, err)) {
        std::printf("  carregamento: %s\n", err.c_str());
        return 1;
    }
    if (!prog.map_into(128ull * 1024 * 1024, err)) {
        std::printf("  mapeamento: %s\n", err.c_str());
        return 1;
    }
    const std::uint64_t entry = prog.entry_point();
    const std::uint64_t sp    = prog.stack_top();
    if (entry == 0) {
        std::printf("  sem entry point\n");
        return 1;
    }

    // Dump ANTES do patch. Sem isto não há como saber se uma instrução
    // "SVC" foi realmente um SVC no ficheiro, ou se o patcher a inventou.
    // Esta é a diferença entre o guest estar errado e nós o estarmos.
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--raw-orig") == 0 && i + 1 < argc) {
            std::vector<std::uint8_t> d(0x4000);
            if (prog.memory().read(entry, d.data(), d.size(), err)) {
                std::FILE* f = std::fopen(argv[i + 1], "wb");
                if (f != nullptr) {
                    std::fwrite(d.data(), 1, d.size(), f);
                    std::fclose(f);
                    std::printf("\n(entry ORIGINAL escrito em %s)\n", argv[i + 1]);
                }
            }
        }
    }

    if (!prog.patch_all(err)) {
        std::printf("  patch: %s\n", err.c_str());
        return 1;
    }
    std::printf("  entry do rtld : 0x%llx\n", (unsigned long long)entry);
    std::printf("  sp            : 0x%llx\n\n", (unsigned long long)sp);

    // ── 2. despachante ligado ao espaço do guest ──────────────────────────
    g_mem = &prog.memory();

    ia::SvcDispatcher svc(tel);
    svc.bind_memory(&prog.memory());
    svc.set_heap_base(ia::layout::kHeapBase);
    svc.set_alias_region(ia::layout::kAliasBase, ia::layout::kAliasSize);
    svc.set_memory_totals(prog.memory().base() + prog.memory().size(),
                          prog.modules().empty()
                              ? 0
                              : prog.modules().front().nso.image.size());
    svc.set_system_resource_size(prog.modules().empty()
                                     ? 0
                                     : prog.modules().front().nso.system_resource_size);
    svc.set_stack_region(ia::layout::kStackBase, ia::layout::kStackSize);
    // A prioridade da thread principal vem do NPDM (`main_thread_priority`).
    if (!prog.modules().empty())
        svc.set_main_thread_priority(prog.modules().front().nso.main_thread_priority);
    svc.set_regions(prog.memory_regions());

    // A tabela que o rtld vai=varrer. Dumpada porque uma região com o
    // `state`/`permission` errados não dá erro nenhum — o rtld simplesmente
    // não a reconhece, e o sintoma aparece muito mais tarde.
    if (!g_slots_quiet) {
        std::printf("\n══════ segmentos carregados ══════\n");
        for (const auto& m : prog.modules()) {
            std::printf("  %-5s base=0x%012llx  imagem=%zu bytes\n", m.name(),
                        (unsigned long long)m.vaddr_base, m.nso.image.size());
            for (int i = 0; i < 3; ++i) {
                std::printf("      seg%d vaddr=0x%08llx size=0x%08llx\n", i,
                            (unsigned long long)m.nso.segments[i].vaddr,
                            (unsigned long long)m.nso.segments[i].size);
            }
            std::printf("      bss=0x%llx\n", (unsigned long long)m.nso.bss_size);
        }
        std::printf("\n══════ regiões (svcQueryMemory) ══════\n");
        std::uint64_t prev_end = 0;
        for (const auto& r : svc.regions()) {
            const char* st = r.state == ia::GuestRegion::kCode   ? "Code"
                             : r.state == ia::GuestRegion::kNormal ? "Normal"
                             : r.state == ia::GuestRegion::kStatic ? "Static"
                                                                   : "?";
            const char* pm = r.permission == ia::GuestRegion::kReadExec ? "R-X"
                             : r.permission == ia::GuestRegion::kReadOnly  ? "R--"
                             : r.permission == ia::GuestRegion::kReadWrite ? "RW-"
                                                                          : "---";
            const bool gap = prev_end != 0 && r.base != prev_end;
            std::printf("  0x%012llx .. 0x%012llx  %-7s %s%s\n",
                        (unsigned long long)r.base,
                        (unsigned long long)(r.base + r.size), st, pm,
                        gap ? "   <<< BURACO" : "");
            prev_end = r.base + r.size;
        }
        std::printf("\n");
    }

    // ── 3. motor, com o traçador ligado ───────────────────────────────────
    ia::NativeEngine engine(tel, svc);
    // O bloco de TLS do guest só existe depois de `map_into()`.
    engine.set_guest_tls(ia::layout::kTlsBase);
    // Poucos traps: este tracer é para ver os PRIMEIROS serviços pedidos, e
    // um guest em spin de código não levanta traps nenhum. O limite não o
    // apanha, mas garante que o programa acaba e imprime relatório — em vez
    // de ficar a queimar CPU sem fim com a saída truncada.
    // 40 traps: o rtld faz uma dúzia de serviços no arranque antes de tocar em
    // código do jogo. Três era pouco para ver a sequência.
    engine.set_trap_limit(40);
    engine.set_observer(&on_svc, nullptr);
    engine.set_trapper(&on_trap, nullptr);
    if (!engine.install_handlers(err)) {
        std::printf("  handlers: %s\n", err.c_str());
        return 1;
    }

    // Despeja os primeiros bytes que o guest vai executar. Sem isto, um
    // guest que corre mas não faz nada dá zero informação — e "não faz
    // nada" é o sintoma mais opaco que existe.
    {
        std::vector<std::uint8_t> head(96);
        if (prog.memory().read(entry, head.data(), head.size(), err)) {
            std::printf("  primeiros bytes a executar:\n    ");
            for (std::size_t i = 0; i < head.size(); i += 4) {
                std::uint32_t w;
                std::memcpy(&w, head.data() + i, 4);
                std::printf(" %08x", w);
                if ((i / 4 + 1) % 6 == 0 && i + 4 < head.size())
                    std::printf("\n    ");
            }
            std::printf("\n\n");
        } else {
            std::printf("  NAO CONSEGUI ler o entry: %s\n\n", err.c_str());
        }
    }

    // Amostragem do PC enquanto o guest corre.
    {
        struct sigaction sa{};
        sa.sa_sigaction = &on_alarm;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGALRM, &sa, nullptr);

        struct itimerval tv{};
        tv.it_interval.tv_sec = 1;
        tv.it_value.tv_sec = 1;
        setitimer(ITIMER_REAL, &tv, nullptr);
    }

    // Despeja a região à volta de um offset pedido (--dump <offset>), para
    // se poder desassemblar sem um debugger Attached.
    // Escreve a imagem do entry num ficheiro, para o objdump a desassemblar.
    // Mais simples e mais fiável que decodificar AArch64 à mão.
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--slots") == 0) {
            g_hooks_dump_slots = true;
            g_slot_offsets = {0x3000, 0x3008, 0x3010, 0x3018,
                              0x3020, 0x3028, 0x3030, 0x3038,
                              0x30B0};
        }
    }

    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--raw") == 0 && i + 1 < argc) {
            std::vector<std::uint8_t> d(0x4000);
            if (prog.memory().read(entry, d.data(), d.size(), err)) {
                std::FILE* f = std::fopen(argv[i + 1], "wb");
                if (f != nullptr) {
                    std::fwrite(d.data(), 1, d.size(), f);
                    std::fclose(f);
                    std::printf("\nimagem do entry escrita em %s\n", argv[i + 1]);
                }
            }
        }
    }

    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            const std::uint64_t off = std::strtoull(argv[i + 1], nullptr, 0);
            const std::uint64_t addr = entry + off;
            std::vector<std::uint8_t> d(128);
            if (prog.memory().read(addr, d.data(), d.size(), err)) {
                std::printf("\n=== bytes em 0x%llx (entry+0x%llx) ===\n",
                            (unsigned long long)addr,
                            (unsigned long long)off);
                for (std::size_t k = 0; k < d.size(); k += 4) {
                    std::uint32_t w;
                    std::memcpy(&w, d.data() + k, 4);
                    std::printf("  +0x%02zx  %08x\n", off + k, w);
                }
            }
        }
    }

    // O padrão de hypercall que encontrámos carrega um ponteiro de função de
    // um slot da GOT e, se for nulo, chama o SVC directamente com um número
    // mágico. Se esse slot devesse estar preenchido, é a explicação mais
    // simples para o guest não avançar — e a mais fácil de falsificar.
    //
    // Lê-se o conteúdo do slot e compara-se com o que o ficheiro NSO traz
    // nesse offset. Se forem diferentes, alguém tinha de o reescrever.
    if (g_hooks_dump_slots) {
        const ia::GuestModule* rtld = nullptr;
        for (const auto& m : prog.modules())
            if (m.kind == ia::GuestModule::kRtld)
                rtld = &m;
        for (std::uint64_t off : g_slot_offsets) {
            std::uint8_t v[8];
            // O offset é relativo ao MÓDULO, não ao espaço do guest.
            if (rtld == nullptr) {
                std::printf("  (sem rtld)\n");
                break;
            }
            const std::uint64_t addr = rtld->vaddr_base + off;
            if (!prog.memory().read(addr, v, sizeof(v), err)) {
                std::printf("  slot 0x%llx: %s\n", (unsigned long long)off,
                            err.c_str());
                continue;
            }
            std::uint64_t val;
            std::memcpy(&val, v, sizeof(val));
            std::printf("  slot 0x%04llx @0x%llx = 0x%016llx %s\n",
                        (unsigned long long)off,
                        (unsigned long long)addr,
                        (unsigned long long)val,
                        val == 0 ? "  <-- NULO (cbz pega)" : "");
            std::fflush(stdout);
        }
        std::printf("\n");
    }

    // Os campos do cabeçalho NSO que dizem ao rtld onde estão as tabelas de
    // símbolos. Sem eles o rtld não sabe o que percorrer — e isso é a
    // diferença entre "os dados não foram carregados" e "os dados estão lá mas
    // ninguém sabe onde".
    std::printf("\n══════ metadata NSO (onde o rtld procura os símbolos) ══════\n");
    for (const auto& m : prog.modules()) {
        const ia::NsoHeader* h = m.nso.nso_header();
        if (h == nullptr) {
            std::printf("  %-6s sem cabeçalho NSO guardado\n", m.name());
            continue;
        }
        const std::uint64_t img = m.nso.image.size();
        auto dentro = [img](std::uint32_t off, std::uint32_t sz) {
            return sz == 0 || (static_cast<std::uint64_t>(off) + sz) <= img;
        };
        std::printf("  %-6s imagem=%zu bytes\n", m.name(), img);
        std::printf("      api_info  off=0x%08x size=0x%08x %s\n",
                    h->api_info_offset, h->api_info_size,
                    dentro(h->api_info_offset, h->api_info_size) ? "" : "<<< FORA");
        std::printf("      dynstr    off=0x%08x size=0x%08x %s\n",
                    h->dynstr_offset, h->dynstr_size,
                    dentro(h->dynstr_offset, h->dynstr_size) ? "" : "<<< FORA");
        std::printf("      dynsym    off=0x%08x size=0x%08x %s\n",
                    h->dynsym_offset, h->dynsym_size,
                    dentro(h->dynsym_offset, h->dynsym_size) ? "" : "<<< FORA");
        // O dynsym tem 24 bytes por entrada no ELF de 64 bits; o size tem de
        // ser multiplo disso. Se não for, o offset está mal lido.
        if (h->dynsym_size != 0) {
            const std::uint32_t n = h->dynsym_size / 24;
            std::printf("      -> %u entradas de simbolo%s\n", n,
                        (h->dynsym_size % 24) ? "  <<< size nao multiplo de 24" : "");
        }
    }
    std::printf("\n");

    // --scan <substring>: procura um símbolo em TODOS os módulos carregados.
    //
    // Responde a uma pergunta que o log não responde: quando o rtld diz
    // "Unresolved symbol: X", o símbolo existe em algum módulo e o runtime
    // não o viu, ou não existe de todo? A diferença é entre um bug de
    // base/relocação e um bug de carregamento — e leva a soluções opostas.
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--scan") == 0 && i + 1 < argc) {
            const std::string alvo = argv[i + 1];
            std::printf("\n══════ procurar \"%s\" em todos os módulos ══════\n",
                        alvo.c_str());
            for (const auto& m : prog.modules()) {
                const auto& img = m.nso.image;
                std::size_t hits = 0;
                std::size_t first = 0;
                for (std::size_t k = 0; k + alvo.size() <= img.size(); ++k) {
                    if (std::memcmp(img.data() + k, alvo.data(), alvo.size()) == 0) {
                        if (hits == 0)
                            first = k;
                        ++hits;
                    }
                }
                std::printf("  %-6s %8zu bytes  %zu occurencia(s)%s\n", m.name(),
                            img.size(), hits,
                            hits ? "" : "   <-- NAO ENCONTRADO");
                if (hits > 0)
                    std::printf("         primeira no offset 0x%zx do modulo"
                                " -> 0x%llx no guest\n",
                                first,
                                (unsigned long long)(m.vaddr_base + first));
            }
            std::printf("\n");
        }
    }

    trace("\n[boot] a entrar no rtld...\n");
    const ia::RunResult r =
        engine.run(reinterpret_cast<void (*)()>(entry), sp);

    {
        struct itimerval tv{};
        setitimer(ITIMER_REAL, &tv, nullptr);
    }

    // ── 4. relatório ──────────────────────────────────────────────────────
    std::printf("\n══════ resultado ══════\n");
    std::printf("  parou por   : %s\n",
                r.error.empty() ? "(sem erro)" : r.error.c_str());
    std::printf("  última trap : %s\n",
                r.last_trap == ia::TrapKind::GuestFault ? "fault de memória"
                : r.last_trap == ia::TrapKind::SvcCall   ? "SVC"
                : r.last_trap == ia::TrapKind::Breakpoint ? "breakpoint"
                : r.last_trap == ia::TrapKind::Yield     ? "yield"
                                                          : "nenhuma");
    std::printf("  svc_calls   : %llu\n", (unsigned long long)r.svc_calls);
    std::printf("  traps       : %llu\n", (unsigned long long)r.traps);
    std::printf("  faults      : %llu\n", (unsigned long long)r.faults);
    std::printf("  último PC   : 0x%llx\n", (unsigned long long)g_last_pc);

    // ONDE cai o endereço do fault. O PC não responde a isto, e a resposta é
    // que separa três bugs completamente diferentes:
    //
    //   (a) dentro da reserva do guest  -> conteúdo em falta
    //   (b) fora da reserva            -> mapeamento ou permissões
    //   (c) dentro, mas de uma região que não é do módulo que executa
    //
    // Sem esta linha, "fault em 0x82d2a910" não diz nada de útil.
    {
        const std::uint64_t a = r.fault_addr;
        // O bloco de TLS do guest, como o motor o entregou. A convenção
        // (0x200 bytes; magic em +0x1E0, handle +0x1E4, thread +0x1E8,
        // reent +0x1F0, tls_tp +0x1F8) diz que o kernel entrega isto ZERADO e
        // que o guest monta o resto. Se `tls_tp` estiver a zero, o
        // `ldr x8, [x8, #0x1f8]` do SDK devolve 0 e tudo o que segue
        // desreferencia o endereço zero.
        {
            std::uint8_t blk[0x200];
            std::string e2;
            if (prog.memory().read(ia::layout::kTlsBase, blk, sizeof blk, e2)) {
                std::printf("  bloco TLS em 0x%012llx:\n",
                            (unsigned long long)ia::layout::kTlsBase);
                for (int o = 0x1C0; o < 0x200; o += 16) {
                    std::string hexs;
                    for (int k = 0; k < 16; ++k) {
                        char t[8];
                        std::snprintf(t, sizeof t, "%02x ", blk[o + k]);
                        hexs += t;
                    }
                    std::printf("    +0x%03x  %s\n", o, hexs.c_str());
                }
                // magic e handle são u32 (u32 magic; Handle handle; — Handle é
                // u32). Ler 8 bytes aqui mostrava "0x121545624" e parecia um
                // bug de escrita que não existia.
                std::uint32_t magic, handle;
                std::memcpy(&magic,  blk + 0x1E0, 4);
                std::memcpy(&handle, blk + 0x1E4, 4);
                std::uint64_t tobj, reent, tp;
                std::memcpy(&tobj,   blk + 0x1E8, 8);
                std::memcpy(&reent,  blk + 0x1F0, 8);
                std::memcpy(&tp,     blk + 0x1F8, 8);
                std::printf("    magic=0x%x handle=0x%x obj=0x%llx "
                            "reent=0x%llx tls_tp=0x%llx\n",
                            magic, handle,
                            (unsigned long long)tobj, (unsigned long long)reent,
                            (unsigned long long)tp);
            }
        }
        std::printf("  addr do fault: 0x%llx -> ", (unsigned long long)a);

        if (prog.memory().owns(a)) {
            // Qual região o contém, e é de quem?
            const char* quem = "desconhecida";
            for (const auto& m : prog.modules()) {
                const std::uint64_t end = m.vaddr_base + m.nso.image.size() +
                                          m.nso.bss_size;
                if (a >= m.vaddr_base && a < end)
                    quem = m.name();
            }
            std::printf("(a) dentro da reserva, região de %s\n", quem);
        } else if (a == 0) {
            std::printf("(c) endereço NULO — o guest desreferenciou 0\n");
        } else {
            std::printf("(b) FORA da reserva (0x%llx..0x%llx) — mapeamento\n",
                        (unsigned long long)prog.memory().base(),
                        (unsigned long long)(prog.memory().base() +
                                             prog.memory().size()));
        }
    }
    std::printf("  traps vistas: %d\n", g_traps);
    std::printf("  svcs vistas : %d\n", g_svcs);
    std::printf("  amostras    : %d  (último PC 0x%llx)\n", g_samples,
                (unsigned long long)g_sample_pc);
    if (!svc.last_port().empty())
        std::printf("  ultima porta: \"%s\"  (STUB: handle fabricado, sem IPC)\n",
                    svc.last_port().c_str());
    std::printf("  MapMemory   : %llu chamadas\n",
                (unsigned long long)svc.map_memory_calls());

    // Sequência de serviços, na ordem em que foram pedidos.
    std::printf("\n══════ serviços pedidos, por ordem ══════\n");
    if (g_calls.empty()) {
        std::printf("  (nenhum — o rtld não chegou a chamar um serviço)\n");
    } else {
        std::size_t mostrar = g_calls.size();
        if (mostrar > 120) {
            mostrar = 120;
            std::printf("  (a mostrar os primeiros %zu de %zu)\n\n",
                        mostrar, g_calls.size());
        }
        std::printf("  %-4s %-24s %-18s %s\n", "#", "serviço", "x0", "x1");
        for (std::size_t i = 0; i < mostrar; ++i) {
            const auto& c = g_calls[i];
            std::printf("  %-4zu %-24s 0x%-16llx 0x%-16llx\n", i,
                        c.name.c_str(),
                        (unsigned long long)c.x[0],
                        (unsigned long long)c.x[1]);
        }
    }

    // Dump da stack do guest no momento do SVC. Se o rtld esperava
    // argc/argv e encontrou outra coisa, vê-se aqui.
    {
        std::vector<std::uint8_t> st(128);
        const std::uint64_t sp_now = prog.stack_top() - 16;
        if (prog.memory().read(sp_now - 96, st.data(), st.size(), err)) {
            std::printf("\n══════ stack do guest ══════\n");
            for (std::size_t i = 0; i < st.size(); i += 8) {
                std::uint64_t v;
                std::memcpy(&v, st.data() + i, 8);
                std::printf("  sp-%3zu  0x%016llx", 96 - i,
                            (unsigned long long)v);
                if (v != 0) {
                    char t[16];
                    std::memcpy(t, &v, 8);
                    std::printf("   %c%c%c%c%c%c%c%c",
                                t[0] ? t[0] : '.', t[1] ? t[1] : '.',
                                t[2] ? t[2] : '.', t[3] ? t[3] : '.',
                                t[4] ? t[4] : '.', t[5] ? t[5] : '.',
                                t[6] ? t[6] : '.', t[7] ? t[7] : '.');
                }
                std::printf("\n");
            }
        }
    }

    // Resumo por serviço: o que o rtld pede mais vezes é o que vale a pena
    // implementar primeiro.
    struct Agg { std::uint32_t n; std::size_t count; };
    std::vector<Agg> agg;
    for (const auto& c : g_calls) {
        bool found = false;
        for (auto& a : agg)
            if (a.n == c.number) { ++a.count; found = true; break; }
        if (!found)
            agg.push_back({c.number, 1});
    }
    std::printf("\n══════ resumo ══════\n");
    std::printf("  serviços distintos: %zu\n", agg.size());
    for (auto& a : agg)
        std::printf("    0x%02x %-24s %zu vez%s\n", a.n,
                    ia::to_string(static_cast<ia::Svc>(a.n)).c_str(),
                    a.count, a.count == 1 ? "" : "es");

    trace("\n[boot] fim\n");
    return 0;
}