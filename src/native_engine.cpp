// EdenMaliIA — motor de execução nativa.
#include "ia/guest_placement.hpp"
#include "ia/native_engine.hpp"
#include "ia/guest_program.hpp"

#include <chrono>
#include <cstdio>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <ucontext.h>

namespace ia {

namespace {

// Os hooks são file-scope porque um handler de sinais não sabe que thread o
// levantou. Um motor single-thread torna isto seguro; com threads de guest
// isto passa a ser um array indexado pelo tid retrieved em tpidr_el0.
EngineHooks g_hooks{};

// ---------------------------------------------------------------------------
// TPIDR_EL0: o TLS da thread, lido e escrito pelo guest
// ---------------------------------------------------------------------------
//
// O guest vai buscar o seu TLS com `mrs x, tpidrro_el0`. Esse registo não
// existe para o guest neste kernel — medido quatro vias, ver PROGRESSO.md —
// por isso o patcher reescreve essa instrução para `mrs x, tpidr_el0`, e é
// este registo que o motor programa.
//
// `TPIDR_EL0` é o `fs` do AArch64 e é por thread de hardware, o que dá a
// multi-thread de graça: cada thread de host guarda o seu e instala o seu
// bloco de TLS de guest.
//
// Consequência que obriga a cuidado: enquanto o guest corre, `TPIDR_EL0` não é
// o TLS da libc. Qualquer código de host que corra nesse intervalo e use TLS
// rebenta — medido: `snprintf` com o `fs` apontado para o bloco do guest dá
// segfault. O único código de host que corre aí dentro é o handler de sinais,
// e é por isso que a troca vive aí e não só à volta de `run()`.

inline std::uint64_t read_thread_pointer() {
    std::uint64_t v = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

inline void write_thread_pointer(std::uint64_t v) {
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(v) : "memory");
}


// Emula `MRS <Xt>, TPIDRRO_EL0` quando o fault foi provoked por ela.
//
// Porque é preciso: ver o caso SIGSEGV em `handle_guest_signal`. O resumo é
// que neste host não há forma de PROGRAMAR o registo — só de o ler, e devolve
// sempre 0.
//
// Os encodings não são adivinhados. Foram montados com o assembler e lidos do
// object file:
//
//     mrs x0, tpidrro_el0   ->  d53bd060
//     mrs x8, tpidrro_el0   ->  d53bd068
//     mrs x3, tpidrro_el0   ->  d53bd063
//     msr tpidrro_el0, x0   ->  d51bd060     <- NÃO colide com o MRS
//
// O destino é o registo em `Xt`, que ocupa os bits [4:0]. A máscara tem de
// apagar SÓ esses cinco: `0xffffffe0`.
//
// A primeira versão usou `0xfffffc1f`, que também apaga os bits [5:6] — e aí
// `0xd53bd068` dava `0xd53bd040`, que não batia certo com o alvo e a
// emulação nunca disparava. A máscara foi escrita a olho, não medida; foi o
// Bits a olhar para os bytes que a apanhou.
inline bool emulate_tls_read(ucontext_t* uc, std::uint64_t pc) {
    if (pc < 4)
        return false;

    std::uint32_t insn = 0;

    constexpr std::uint32_t kMask = 0xFFFFFFE0u;
    constexpr std::uint32_t kMrsTpidrro = 0xD53BD060u;

    // Recuar mais do que uma instrução.
    //
    // Medido no `sdk` (offset 0x32f900):
    //
    //     mrs  x8, tpidrro_el0      ; pc-8
    //     ldr  w9, [x0]             ; pc-4   <- um spill
    //     ldr  x8, [x8, #0x1f8]     ; pc     <- fault
    //
    // O `mrs` não levanta trap — lê 0 e segue. O fault só chega duas
    // instruções depois. Por isso se olha para trás algumas instruções, e se
    // recomeça a execução no `mrs` já corrigido.
    //
    // A condição `registo == 0` é o que impede o ciclo: depois de corrigido, o
    // registo já não é zero e a procura falha. E limita o alcance: se o
    // registo tiver mudado de valor, a causa não era o TLS e o fault é
    // reportado como fault.
    constexpr int kMaxLookback = 4;
    for (int back = 1; back <= kMaxLookback; ++back) {
        const std::uint64_t at = pc - 4 * static_cast<std::uint64_t>(back);
        if (at < 4)
            break;

        std::memcpy(&insn, reinterpret_cast<const void*>(at), sizeof(insn));
        if ((insn & kMask) != kMrsTpidrro)
            continue;

        const std::size_t reg = insn & 0x1Fu;
        if (reg >= 32 || uc->uc_mcontext.regs[reg] != 0)
            continue;

        uc->uc_mcontext.regs[reg] = g_hooks.guest_tls;
        uc->uc_mcontext.pc = at + 4;
        return true;
    }
    return false;
}

// Handlers por omissão, guardados para repor quando um trap não for nosso.
struct sigaction g_old_trap{};
struct sigaction g_old_segv{};
struct sigaction g_old_bus{};

// BRK emitido por `ia_guest_returned` (src/ia_call_guest.S) quando o guest
// faz `ret` sem ter chamado ExitProcess. É uma trap para que este caso
// patológico passe pelo mesmo mecanismo de recuperação dos restantes — só o
// `siglongjmp` restaura o `sp` do host.
constexpr std::uint32_t kBrkGuestReturned = 0xFFFD;
// Paragem pedida pelo tracer: um BRK com este número também sai do guest.
constexpr std::uint32_t kBrkForceStop = 0xFFFC;

// Stack e rotina de recuperação de faults.
//
// Um SIGSEGV numa instrução do guest não pode retomar: repetir a mesma
// instrução entraria em ciclo até o processo ser morto, e o stack trace
// original desapareceria. Em vez disso o handler salta para
// `_fault_recovery`, que corre com esta stack e regressa ao host por
// siglongjmp.
alignas(16) std::uint8_t g_recovery_stack[64 * 1024];

// sigjmp_buf para regressar ao host a partir da recuperação de faults.
sigjmp_buf g_run_jmp;
volatile bool g_have_run_jmp = false;

// Estado do registo do guest no momento do fault, para o relatório.
volatile std::uint64_t g_fault_pc = 0;
volatile std::uint64_t g_fault_sp = 0;

// Rotinas de saída do guest.
//
// Tanto um fault de memória como um ExitProcess precisam de regressar ao host
// sem o guest voltar a executar. Ambas usam siglongjmp para `g_run_jmp`, o
// ponto de regresso posto por `run()` antes de chamar o guest.
//
// NOTA: `g_run_jmp` e não `g_fault_jmp` — há um único ponto de regresso, e
// é o `run()` que o põe. Ter dois obrigaria a sincronizar os dois.
[[noreturn]] void exit_recovery() {
    if (g_have_run_jmp)
        siglongjmp(g_run_jmp, 1);
    // Sem ponto de regresso: sair é melhor do que executar lixo.
    _exit(70);
}

} // namespace

NativeEngine::NativeEngine(tel::Telemetry& tel, SvcDispatcher& svc)
    : tel_(tel), svc_(svc) {}

NativeEngine::~NativeEngine() {
    if (!hooks_.armed)
        return;
    ::sigaction(SIGTRAP, &g_old_trap, nullptr);
    ::sigaction(SIGSEGV, &g_old_segv, nullptr);
    ::sigaction(SIGBUS, &g_old_bus, nullptr);
    ::sigaction(SIGILL, nullptr, nullptr);
    ::sigaction(SIGALRM, nullptr, nullptr);
    hooks_.armed = false;
    g_hooks.armed = false;
}

// Publica os hooks para o handler estático. Qualquer alteração depois de
// instalado tem de passar por aqui — o handler não vê a instância.
void NativeEngine::publish_hooks() {
    // Copia campo a campo em vez de `g_hooks = hooks_`.
    //
    // Só a CONFIGURAÇÃO é copiada: para onde despachar, quem observar, e as
    // rotinas de recuperação. O estado (contadores, flags de trap) NÃO é —
    // esse vive só em `g_hooks`, porque é o handler que o escreve e `run()` o
    // lê de volta no fim.
    //
    // A distinção não é preciosismo: com `g_hooks = hooks_`, o `run()`
    // reverteria o estado a cada chamada, e um observer instalado depois
    // desaparecia no `install_handlers` seguinte.
    g_hooks.svc              = hooks_.svc;
    g_hooks.tel              = hooks_.tel;
    g_hooks.observer         = hooks_.observer;
    g_hooks.observer_user    = hooks_.observer_user;
    g_hooks.trapper          = hooks_.trapper;
    g_hooks.trapper_user     = hooks_.trapper_user;
    g_hooks.platform_reg     = hooks_.platform_reg;
    g_hooks.recovery_pc      = hooks_.recovery_pc;
    g_hooks.recovery_sp      = hooks_.recovery_sp;
    g_hooks.armed            = hooks_.armed;
    g_hooks.trap_limit       = hooks_.trap_limit;
}

// Repõe os contadores na cópia que o handler escreve.
//
// Têm de ser repostos em `g_hooks` e NÃO em `hooks_`: o handler só conhece
// `g_hooks`. Repô-los só na instância e depois publicar não os limpava, e o
// segundo `run()` reportava os contadores do primeiro — que é o número que
// esconde bugs.
void NativeEngine::reset_counters() {
    g_hooks.svc_calls   = 0;
    g_hooks.traps       = 0;
    g_hooks.faults      = 0;
    g_hooks.host_faults = 0;
    g_hooks.svc_time_ns = 0;
    g_hooks.last        = TrapKind::None;
    g_hooks.faulted     = false;
    g_hooks.guest_returned = false;
    g_hooks.illegal_insn = false;
    g_hooks.limit_hit   = false;
    g_hooks.fault_pc    = 0;
    g_hooks.fault_addr  = 0;
    g_hooks.fault_sp    = 0;
}

bool NativeEngine::install_handlers(std::string& error) {
    ::sigaction(SIGTRAP, nullptr, &g_old_trap);
    ::sigaction(SIGSEGV, nullptr, &g_old_segv);
    ::sigaction(SIGBUS, nullptr, &g_old_bus);

    struct sigaction sa{};
    sa.sa_sigaction = &NativeEngine::signal_trampoline;
    // SA_SIGINFO: precisamos de siginfo_t para distinguir um BRK nosso de
    //              outro tipo de trap.
    // SA_NODEFER: uma trap dentro do handler não se auto-ignora. Um BRK
    //              aninhado é um bug e tem de ser visível.
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    if (::sigaction(SIGTRAP, &sa, nullptr) != 0) {
        error = "sigaction(SIGTRAP) falhou";
        return false;
    }
    if (::sigaction(SIGSEGV, &sa, nullptr) != 0) {
        error = "sigaction(SIGSEGV) falhou";
        return false;
    }
    if (::sigaction(SIGBUS, &sa, nullptr) != 0) {
        error = "sigaction(SIGBUS) falhou";
        return false;
    }
    // SIGILL nao e' uma trap de guest, mas sem handler o processo morre sem
    // diagnostico. Trata-se como host_fault para que o motor reporte onde.
    ::sigaction(SIGILL, &sa, nullptr);
    // SIGALRM nao e' uma trap de guest, mas sem handler um spin de codigo
    // nunca mais devolve o controlo ao host. O tracer usa-o como ultimo
    // recurso.
    ::sigaction(SIGALRM, &sa, nullptr);

    hooks_.svc = &svc_;
    hooks_.tel = &tel_;
    // Endereço e stack da rotina de recuperação de faults.
    hooks_.recovery_pc = reinterpret_cast<std::uint64_t>(&exit_recovery);
    // O topo da stack, alinhado a 16 bytes como a AAPCS64 exige.
    hooks_.recovery_sp = reinterpret_cast<std::uint64_t>(
        g_recovery_stack + sizeof(g_recovery_stack));
    hooks_.armed = true;
    publish_hooks();
    // NOTA: `publish_hooks` copia a estrutura inteira. Por isso os hooks que
    // o utilizador instala têm de ser postos ANTES de `install_handlers`, ou
    // republished depois — se não, esta cópia apaga-os.

    error.clear();
    return true;
}

RunResult NativeEngine::run(void (*code)(), std::uint64_t sp) {
    RunResult r;

    // O guest corre NESTA thread — o motor e' um loop de traps sincrono, e
    // nao ha `pthread_create` em lado nenhum. Sem isto o jogo herda o nucleo
    // onde o processo calhou, e o RK3588 pode calhar num A55.
    //
    // O Switch corre o jogo em 3 A57 a 1.020 MHz. Um A76 a 2.4 GHz e' outra
    // coisa, e e' por isso que se vai la. Ver `guest_placement.cpp`.
    fixar_guest_nos_nucleos_grandes();

    if (!hooks_.armed) {
        r.error = "handlers nao instalados";
        return r;
    }

    // Estado do dispatcher: um `exited_` que ficou a true contaminaria a
    // execução seguinte e faria um fault parecer uma saída limpa.
    svc_.reset_run_state();

    // Reinicia os contadores: um segundo arranque não deve herdar o primeiro.
    //
    // NÃO se faz `hooks_ = g_hooks` aqui. O handler de sinais vive em
    // `g_hooks`, e uma cópia de volta apagaria o `armed` e os hooks que o
    // utilizador instalou — foi exactamente o que aconteceu: o observer e o
    // trapper desapareciam no segundo arranque e o guest ficava mudo.
    reset_counters();

    // Ponto de regresso para a recuperação de traps.
    //
    // `sigsetjmp(buf, 1)` salva também a máscara de sinais: sem o segundo
    // argumento, o longjmp deixaria os handlers bloqueados e um segundo fault
    // mataria o processo em vez de ser reportado.
    //
    // Todas as saídas do guest passam por aqui. Nunca por um retorno normal
    // da chamada: se o guest fizesse `ret`, o epílogo de `run()` verificaria o
    // canário do stack protector sobre a stack do guest (medido — ver
    // src/ia_call_guest.S). `ia_guest_returned` transforma esse caso numa
    // trap, e o trap é tratado aqui como os restantes.
    // x18 é a stack canary do guest (o "platform register" da AAPCS64). Sem
    // isto o guest verifica a canary contra lixo, a verificação falha, e o
    // código de recuperação chama ReturnFromException. Foi o primeiro SVC que
    // o rtld pediu, com um x0 que não era um endereço de 39 bits — o sintoma
    // não apontava para a causa.
    //
    // O valor tem de ser o mesmo em todas as entradas, e o Eden usa
    // `Random64(0) | 1` (o bit 0 impede que uma canary termine em NUL).
    if (platform_reg_ == 0) {
        std::uint64_t seed = 0;
        std::FILE* f = std::fopen("/dev/urandom", "rb");
        if (f != nullptr) {
            if (std::fread(&seed, 1, sizeof(seed), f) != sizeof(seed))
                seed = 0x9E3779B97F4A7C15ull;
            std::fclose(f);
        } else {
            seed = 0x9E3779B97F4A7C15ull;
        }
        platform_reg_ = seed | 1u;
    }
    hooks_.platform_reg = platform_reg_;

    if (::getenv("IA_TRACE")) {
        char b[96];
        const int k = std::snprintf(b, sizeof b,
            "[run] platform_reg=0x%llx hooks=0x%llx\n",
            (unsigned long long)platform_reg_,
            (unsigned long long)hooks_.platform_reg);
        ssize_t w = ::write(2, b, (size_t)k); (void)w;
    }

    // A base do TLS do guest, decidida pelo boot (`set_guest_tls`). Zero quando
// não há programa — e nesse caso o `TPIDR_EL0` não é tocado.
g_hooks.guest_tls = hooks_.guest_tls;
g_hooks.host_tls  = read_thread_pointer();
guest_x1_         = hooks_.guest_x1;

const int jumped = sigsetjmp(g_run_jmp, 1);

    // O handler de sinais escreve em `g_hooks` (não tem instância). Trazer só
    // os CONTADORES de volta — nunca a estrutura inteira.
    //
// `hooks_ = g_hooks` seria o erro óbvio e ERRADO: `g_hooks` é uma cópia
    // que o handler actualiza campo a campo, e pode ter visto
    // `g_hooks.observer` ou `g_hooks.trapper` ainda a zero se o utilizador os
    // instalou depois de `install_handlers()`. Copiar a estrutura inteira
    // apagava-os, e o handler saía no `if (!g_hooks.armed) return;` — o guest
    // ficava mudo e sem diagnóstico. Foi o que aconteceu: o rtld levantava a
    // trap, o handler via-a, e não fazia nada.
    hooks_.svc_calls   = g_hooks.svc_calls;
    hooks_.traps       = g_hooks.traps;
    hooks_.faults      = g_hooks.faults;
    hooks_.host_faults = g_hooks.host_faults;
    hooks_.svc_time_ns = g_hooks.svc_time_ns;
    hooks_.last        = g_hooks.last;
    hooks_.faulted     = g_hooks.faulted;
    hooks_.guest_returned = g_hooks.guest_returned;
    hooks_.illegal_insn = g_hooks.illegal_insn;
    hooks_.limit_hit    = g_hooks.limit_hit;
    hooks_.fault_pc    = g_hooks.fault_pc;
    hooks_.fault_addr  = g_hooks.fault_addr;
    hooks_.fault_sp    = g_hooks.fault_sp;

    // Já estamos no host. O `siglongjmp` deixou o TLS do guest instalado, e o
    // resto de `run()` — telemetria, relatório — é código de host.
    if (g_hooks.guest_tls != 0)
        write_thread_pointer(g_hooks.host_tls);

    if (jumped == 0) {
        g_have_run_jmp = true;

        // Instalar o TLS do guest. Daqui até ao `sigreturn` que segue
        // qualquer trap, o `mrs x, tpidr_el0` do guest devolve a base do TLS
        // DELE — e não o da libc.
        if (g_hooks.guest_tls != 0)
            write_thread_pointer(g_hooks.guest_tls);

        ia_call_guest(code, sp, platform_reg_, guest_x1_);
        g_have_run_jmp = false;
        // inalcançável em operação normal: se o guest tivesse regressado,
        // a stack estaria corrompida e isto nem sequer seria seguro escrever.
        r.last_trap = TrapKind::Breakpoint;
        r.error = "o guest regressou sem chamar ExitProcess";
    } else if (g_hooks.limit_hit) {
        g_have_run_jmp = false;
        r.last_trap = TrapKind::Breakpoint;
        char msg[192];
        if (hooks_.trap_limit != 0)
            std::snprintf(msg, sizeof(msg),
                          "limite de traps atingido (%llu) — o guest esta em ciclo",
                          (unsigned long long)hooks_.trap_limit);
        else
            std::snprintf(msg, sizeof(msg),
                          "alarme de seguranca — o guest ficou preso em codigo "
                          "(sem levantar traps) durante tempo suficiente");
        r.error = msg;
    } else if (svc_.exited()) {
        g_have_run_jmp = false;
        // Saída limpa: o guest chamou ExitProcess.
        r.last_trap = TrapKind::SvcCall;
    } else if (g_hooks.guest_returned) {
        g_have_run_jmp = false;
        // O guest fez `ret` sem chamar ExitProcess: bug do guest, não saída.
        r.last_trap = TrapKind::Breakpoint;
        r.error = "o guest regressou sem chamar ExitProcess";
    } else if (g_hooks.illegal_insn) {
        g_have_run_jmp = false;
        // SIGILL no guest: instrução que o CPU não reconhece. Num jogo real
        // isto significa uma descodificação errada — ou uma trap nossa.
        r.last_trap = TrapKind::Breakpoint;
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "instrucao ilegal do guest em 0x%llx (sp=0x%llx)",
                      (unsigned long long)g_hooks.fault_pc,
                      (unsigned long long)g_hooks.fault_sp);
        r.error = msg;
    } else {
        g_have_run_jmp = false;
        // Fault de memória no guest.
        r.last_trap = TrapKind::GuestFault;
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "fault de memoria do guest em 0x%llx (sp=0x%llx)",
                      (unsigned long long)g_hooks.fault_pc,
                      (unsigned long long)g_hooks.fault_sp);
        r.error = msg;
    }

    r.exited    = svc_.exited();
    r.exit_code = svc_.exit_code();
    r.svc_calls   = hooks_.svc_calls;
    r.traps       = hooks_.traps;
    r.faults      = hooks_.faults;
    r.host_faults = hooks_.host_faults;
    r.fault_pc   = hooks_.fault_pc;
    r.fault_addr = hooks_.fault_addr;
    r.svc_time_ns = hooks_.svc_time_ns;
    return r;
}

// ---------------------------------------------------------------------------
// Handler de sinais
// ---------------------------------------------------------------------------

void NativeEngine::signal_trampoline(int sig, siginfo_t* info, void* raw) {
    // Duas trocas, e o motivo é o mesmo nos dois sentidos: quem executa
    // decide de quem é o registo.
    //
    //   - o handler é código do HOST (printf, telemetria, despacho de SVC) e
    //     precisa do TLS da libc.
    //   - o `sigreturn` que se segue põe o guest a correr, e o guest espera o
    //     SEU TLS em `TPIDR_EL0`.
    //
    // Se a troca do fim faltar, o guest retoma com o TLS da libc e lê o
    // ThreadLocalRegion no sítio errado — um fault que não parece um fault.
    if (g_hooks.guest_tls == 0) {
        // Sem programa carregado não há bloco de TLS de guest — e instalar
        // um endereço não mapeado rebenta a libc dentro do handler. Os testes
        // unitários correm um guest sintético sem programa, e ficavam a
        // bloquear por causa disto.
        handle_guest_signal(sig, info, raw);
        return;
    }
    write_thread_pointer(g_hooks.host_tls);
    handle_guest_signal(sig, info, raw);
    write_thread_pointer(g_hooks.guest_tls);
}

void NativeEngine::handle_guest_signal(int sig, siginfo_t* info, void* raw) {
    if (!g_hooks.armed)
        return; // nunca devia acontecer: handlers não instalados

    auto* uc = static_cast<ucontext_t*>(raw);
    const auto pc = static_cast<std::uint64_t>(uc->uc_mcontext.pc);
    ++g_hooks.traps;

    if (g_hooks.trapper != nullptr)
        g_hooks.trapper(sig, pc, static_cast<std::uint64_t>(uc->uc_mcontext.sp),
                        g_hooks.trapper_user);

    // Limite de traps. Um guest em ciclo não pode ficar a queimar CPU para
    // sempre: sai-se pela rotina de recuperação, que preserva o `sp` do host
    // e deixa o relatório explicar o que aconteceu.
    if (g_hooks.trap_limit != 0 && g_hooks.traps >= g_hooks.trap_limit) {
        g_hooks.limit_hit = true;
        g_hooks.last = TrapKind::Breakpoint;
        uc->uc_mcontext.sp = g_hooks.recovery_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    if (::getenv("IA_TRACE")) {
        char b[160];
        const int k = std::snprintf(b, sizeof b,
            "[h] sig=%d armed=%d obs=%p tel=%p svc=%p rec=%llx\n", sig,
            (int)g_hooks.armed, (void*)g_hooks.observer, (void*)g_hooks.tel,
            (void*)g_hooks.svc, (unsigned long long)g_hooks.recovery_pc);
        ssize_t w = ::write(2, b, (size_t)k); (void)w;
    }

    switch (sig) {
    case SIGTRAP: {
        // Descodificar o número de serviço do próprio BRK no PC.
        //
        // Não se usa si_value: numa trap de BRK o Linux põe si_addr = o
        // endereço da instrução, e o imediato só existe nas instruções. Ler o
        // código é o que garante que o número é o que o guest pediu.
        std::uint32_t insn = 0;
        if (pc != 0)
            std::memcpy(&insn, reinterpret_cast<const void*>(pc), 4);

        if ((insn & 0xFFE00000u) != 0xD4200000u) {
            // Não é um BRK: é outra trap (depuracao do host, ou um BRK de
            // instrumentação nosso). Contabiliza e devolve.
            g_hooks.last = TrapKind::Breakpoint;
            ++g_hooks.host_faults;
            return;
        }

        const std::uint32_t number = (insn >> 5) & 0xFFFF;

        if (number == kBrkForceStop) {
            // Paragem pedida pelo tracer (um spin de código que não levanta
            // traps). Sai pelo mesmo caminho dos restantes.
            g_hooks.last = TrapKind::Breakpoint;
            g_hooks.limit_hit = true;
            uc->uc_mcontext.sp = g_hooks.recovery_sp;
            uc->uc_mcontext.pc = g_hooks.recovery_pc;
            return;
        }

        if (number == kBrkGuestReturned) {
            // O guest fez `ret` sem ExitProcess. Sair pelo mesmo caminho dos
            // faults para que o `siglongjmp` restaure o `sp` do host: sem
            // isto, o epílogo de `run()` verificaria o canário do stack
            // protector sobre a stack do guest.
            g_hooks.last = TrapKind::Breakpoint;
            g_hooks.guest_returned = true;
            uc->uc_mcontext.sp = g_hooks.recovery_sp;
            uc->uc_mcontext.pc = g_hooks.recovery_pc;
            return;
        }

        // Despachar o serviço: argumentos em x0-x5, resultado em x0.
        SvcContext c{};
        for (int i = 0; i < 6; ++i)
            c.x[i] = uc->uc_mcontext.regs[i];
        for (int i = 0; i < 31; ++i)
            c.gpr[i] = uc->uc_mcontext.regs[i];
        c.pc = pc;
        c.sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);

        // O observer ve o contexto ANTES do dispatch, para poder registar o
        // que o guest pediu. Se registasse depois, veria o resultado em x0 e
        // não saberia o que foi pedido — que é a informação útil.
        if (g_hooks.observer != nullptr)
            g_hooks.observer(number, c, pc,
                             static_cast<std::uint64_t>(uc->uc_mcontext.sp),
                             g_hooks.observer_user);

        const auto t0 = g_hooks.tel->time_now();
        g_hooks.svc->dispatch(number, c);
        g_hooks.svc_time_ns +=
            static_cast<std::uint64_t>(g_hooks.tel->time_now() - t0);
        ++g_hooks.svc_calls;

        // A ORDA IMPORTA: x0 é o registo de RETORNO na ABI do Switch.
        //
        // Primeiro os argumentos (x1-x5), que o serviço pode ter mudado como
        // canal de saída. DEPOIS o resultado em x0.
        //
        // Ao contrário, `c.ret` era escrito primeiro e o ciclo seguinte
        // sobrescrevia-o com `c.x[0]` — que para a maioria dos serviços é o
        // endereço de saída, não o retorno. O guest via um endereço onde
        // esperava um Result, e o rtld tratava isso como erro:
        //
        //     4dc:  cbnz w0, 0x708     ; Result != 0 -> unreachable
        //
        // Por isso x0 NÃO é copiado de `c.x`. Quem devolve um valor escreve
        // em `ctx.ret`.
        for (int i = 1; i < 6; ++i)
            uc->uc_mcontext.regs[i] = c.x[i];
        uc->uc_mcontext.regs[0] = c.ret;

        // Repõe x18. O `ret` do guest clobbera o platform register, e o
        // próximo SVC que verificar a canary encontraria um valor diferente do
        // que foi posto no arranque.
        if (g_hooks.platform_reg != 0)
            uc->uc_mcontext.regs[18] = g_hooks.platform_reg;

        g_hooks.last = TrapKind::SvcCall;

        // ExitProcess: o guest pediu para terminar. Escrever pc+4 e regressar
        // NÃO chega — o guest continuaria a executar depois do ExitProcess.
        // A saída faz-se como a dos faults: saltar para uma rotina com stack
        // própria, que regressa ao host por siglongjmp.
        if (g_hooks.svc->exited()) {
            uc->uc_mcontext.sp = g_hooks.recovery_sp;
            uc->uc_mcontext.pc = g_hooks.recovery_pc;
            return;
        }

        // Saltar o BRK. O kernel honra esta escrita — medido em
        // tests/probe_ucontext.cpp.
        uc->uc_mcontext.pc = pc + 4;
        return;
    }

    case SIGSEGV:
    case SIGBUS: {
        // Antes de contar um fault, tentar o caso que NÃO é um fault: o guest
        // ter lido o seu TLS de um registo de sistema que o host também usa.
        //
        // O SDK do jogo faz `mrs x8, tpidrro_el0` para chegar ao bloco de TLS.
        // Numa execução nativa esse registo é o `fs` da thread do Linux. Neste
        // host vale 0 (medido), e a instrução seguinte — `ldr x8, [x8, #0x1F8]`
        // — acerta em 0x1f8, fora do espaço do guest: SIGSEGV.
        //
        // Trocar o registo seria a solução limpa, e foi a primeira tentativa:
        // neste kernel `msr tpidrro_el0, xn` levanta SIGILL, e
        // `arch_prctl(ARCH_SET_FS)` devolve -1. Medido nos dois, num programa
        // de 10 linhas. Não há caminho de escrita.
        //
        //resta a emular a LEITURA, que é uma instrução e não um caminho de
        //execução. Continua a ser nativo: o CPU não sabe que isto aconteceu.
        if (g_hooks.guest_tls != 0 && emulate_tls_read(uc, pc))
            return;   // retoma o guest no PC seguinte

        if (::getenv("IA_TRACE")) {
            char b[224];
            int n = std::snprintf(b, sizeof b, "[tls] falhou em pc=0x%llx\n",
                                  (unsigned long long)pc);
            if (pc >= 16) {
                for (int i = 4; i >= 1; --i) {
                    std::uint32_t w = 0;
                    std::memcpy(&w, reinterpret_cast<const void*>(pc - 4 * i), 4);
                    n += std::snprintf(b + n, sizeof(b) - n,
                                       "  pc-0x%x: %08x  x0=0x%llx x8=0x%llx "
                                       "tpidrro=0x%llx\n", 4 * i, w,
                                       (unsigned long long)uc->uc_mcontext.regs[0],
                                       (unsigned long long)uc->uc_mcontext.regs[8],
                                       (unsigned long long)read_thread_pointer());
                }
            }
            ssize_t w = ::write(2, b, (size_t)n); (void)w;
        }

        ++g_hooks.faults;
        g_hooks.last = TrapKind::GuestFault;
        g_hooks.tel->note_page_fault();

        // Um fault de memória não pode continuar: deixar o sigreturn
        // re-executar a mesma instrução entraria em ciclo até o processo ser
        // morto, engolindo o stack trace — o pior diagnóstico possível.
        g_hooks.fault_pc = pc;
        g_hooks.fault_sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);
        g_hooks.fault_addr = reinterpret_cast<std::uint64_t>(info->si_addr);
        g_hooks.faulted  = true;

        uc->uc_mcontext.sp = g_hooks.recovery_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    case SIGALRM: {
        // Alarme de segurança: o guest ficou preso em código (um spin que não
        // levanta traps) e o tracer quer que o programa volte ao host para
        // imprimir o relatório. Sem isto, um spin silencioso truncava a
        // saída e perdia-se o único rasto do que aconteceu.
        g_hooks.limit_hit = true;
        g_hooks.last = TrapKind::Breakpoint;
        uc->uc_mcontext.sp = g_hooks.recovery_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    case SIGILL: {
        // Uma instrucao que o CPU nao reconhece. Num guest isto significa uma
        // descodificacao errada ou um patch mal formado — e nao uma saida.
        // Registado a parte para o relatorio dizer qual dos dois.
        ++g_hooks.faults;
        ++g_hooks.host_faults;
        g_hooks.last = TrapKind::Breakpoint;
        g_hooks.illegal_insn = true;
        g_hooks.fault_pc = pc;
        g_hooks.fault_sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);
        uc->uc_mcontext.sp = g_hooks.recovery_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    default:
        return;
    }
}

} // namespace ia
