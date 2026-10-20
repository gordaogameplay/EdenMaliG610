/* Sonda do ciclo de traps do Linux em modo de utilizador (AArch64).
 *
 * Pergunta que responde: quando o guest levanta um BRK, o que é que o kernel
 * entrega ao signal handler, e o handler pode alterá-lo?
 *
 * Isto decide o desenho do motor. Duas hipóteses:
 *
 *   A) O handler altera o ucontext e regressa -> o kernel restaura o registo
 *      do PC depois do BRK e o guest continua. Simples e rápido, mas o
 *      contexto do host partilha registos com o guest.
 *
 *   B) O motor troca o contexto do hardware (x0-x30, sp, pc) para o do guest
 *      dentro do handler, executa o serviço em C++, e volta a trocar. Custa
 *      ~20 instrucções por SVC mas mantem host e guest independentes.
 *
 * O Eden usa (B). Verificar se (A) sequer funciona e quanto contexto o kernel
 * dá para manipular e o que decide a escolha.
 */
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ucontext.h>
#include <asm/sigcontext.h>

extern "C" void probe_brk(void);
extern "C" void probe_trap(void);

static volatile int g_saw_signal = 0;
static volatile int g_sig = 0;
static volatile int g_code = 0;
static volatile unsigned long g_addr = 0;
static volatile unsigned long g_pc = 0;
static volatile unsigned long g_x0 = 0;
static volatile unsigned long g_x1 = 0;
static volatile unsigned long g_sp = 0;
static volatile int g_signal_is_brk = 0;

static void handler(int sig, siginfo_t* info, void* raw) {
    auto* uc = static_cast<ucontext_t*>(raw);
    g_saw_signal = 1;
    g_sig  = sig;
    g_code = info->si_code;
    g_addr = reinterpret_cast<unsigned long>(info->si_addr);
    g_pc   = static_cast<unsigned long>(uc->uc_mcontext.pc);
    g_x0   = static_cast<unsigned long>(uc->uc_mcontext.regs[0]);
    g_x1   = static_cast<unsigned long>(uc->uc_mcontext.regs[1]);
    g_sp   = static_cast<unsigned long>(uc->uc_mcontext.sp);

    // A palavra no PC: e o BRK? tem o encoding 0xD4200000 nos bits altos?
    unsigned insn = 0;
    if (g_pc != 0)
        std::memcpy(&insn, reinterpret_cast<const void*>(g_pc), 4);
    g_signal_is_brk = ((insn & 0xFFE00000u) == 0xD4200000u) ? 1 : 0;

    // Tentar avançar o PC para depois do BRK: se o kernel respeitar, o guest
    // continua na instrucao seguinte em vez de repetir a trap.
    uc->uc_mcontext.pc += 4;
}

static void arm() {
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
}

static void disarm() {
    struct sigaction d{};
    d.sa_handler = SIG_DFL;
    sigemptyset(&d.sa_mask);
    sigaction(SIGTRAP, &d, nullptr);
    sigaction(SIGILL, &d, nullptr);
    sigaction(SIGSEGV, &d, nullptr);
}

int main() {
    printf("=== sonda de traps em modo de utilizador (AArch64) ===\n");
    arm();

    printf("\n[1] BRK #3 — que sinal o kernel levanta?\n");
    g_saw_signal = 0;
    probe_brk();
    if (g_saw_signal) {
        printf("    sinal        = %d (%s)\n", g_sig,
               g_sig == SIGTRAP ? "SIGTRAP" : g_sig == SIGILL ? "SIGILL" : "?");
        printf("    si_code      = %d\n", g_code);
        printf("    si_addr      = 0x%lx\n", g_addr);
        printf("    ucontext.pc  = 0x%lx\n", g_pc);
        printf("    x0 no trap   = %lu\n", g_x0);
        printf("    sp no trap   = 0x%lx\n", g_sp);
        printf("    x1 no trap   = %lu\n", g_x1);
        printf("    PC tem BRK   = %s\n", g_signal_is_brk ? "sim" : "NAO");
        // Se o handler avancar o PC, o kernel deve retomar DEPOIS do
        // BRK. Se g_x1 vale 7, a instrucao `mov x1, #7` executou.
        printf("    x1 depois    = %lu  (esperado 7 se o PC avancou)\n", g_x1);
    } else {
        printf("    nenhuma trap!\n");
    }

    printf("\n[2] SVC #0 cru — como o trata o kernel?\n");
    g_saw_signal = 0;
    probe_trap();
    if (g_saw_signal)
        printf("    sinal = %d (%s)\n", g_sig,
               g_sig == SIGILL ? "SIGILL" : g_sig == SIGTRAP ? "SIGTRAP" : "?");
    else
        printf("    nenhuma trap (o kernel tratou o SVC internamente)\n");

    disarm();
    printf("\n=== fim ===\n");
    return 0;
}
