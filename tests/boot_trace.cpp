// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — traçador do arranque real.
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
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <csignal>
#include <sys/time.h>
#include <signal.h>
#include <ucontext.h>

#include "g610/guest_program.hpp"
#include "g610/horizon_services_map.h"
#include "g610/services/binder.hpp"
#include "g610/guest_memory.hpp"
#include "g610/native_engine.hpp"
#include "g610/svc_dispatcher.hpp"
#include "g610/telemetry.hpp"

namespace {

// **O directorio dos dumps.** `G610_DUMP_DIR` aponta para um directorio de
// trabalho — por exemplo um disco externo, quando o `/tmp` do sistema nao
// tem espaco.
const char* dump_dir() {
    const char* d = std::getenv("G610_DUMP_DIR");
    return (d != nullptr && d[0] != 0) ? d : "/tmp";
}

// A medicao do arranque: o instante em que o guest comecou, o contador de
// SVCs, e o instante do `Break`.
long long g_inicio_ns = 0;


// Registo de cada SVC pedido, na ordem. **Sem heap**: o `on_svc` corre
// DENTRO de um handler de sinal em TODOS os threads do guest em
// paralelo, e um push_back de vector (que realoca e faz free do bloco
// velho) de dois threads ao mesmo tempo liberta o MESMO chunk duas
// vezes — medido: o core do abort de 23/10 ve' o chunk
// 0xaaaac8651840 (229392 bytes) LIVRE no unsorted bin (fd/bk iludidos)
// no instante do `free` do push_back, e os frames das threads 4/5/6
// todos em `svc_wait_process_wide_key_atomic` despachados dentro do
// handler. O nome deixa de ser `std::string` (uma alocacao e uma
// libertacao POR SVC) e passa a ponteiro de tabela pre-computada.
struct SvcCall {
    std::uint32_t number;
    const char*   name;
    std::uint64_t x[6];
    std::uint32_t result;
    std::uint64_t pc;
    std::uint64_t sp;
};

// Os primeiros SVCs, em ordem — a ordem e' o que o dump de fim usa
// ("servicos pedidos, por ordem"). Teto fixo: nada cresce em contexto
// de sinal.
constexpr std::size_t kSvcOrdenadoTam = 120;
SvcCall g_primeiros[kSvcOrdenadoTam] = {};
std::atomic<std::uint32_t> g_primeiros_cursor{0};
// A contagem por numero de SVC vale o agregado de fim (que era o laco
// O(N^2) sobre o vector). 0x100 cobre o enum visto; o resto conta no
// final.
constexpr std::size_t kSvcContagensTam = 0x100;
std::atomic<std::uint64_t> g_svc_contagens[kSvcContagensTam] = {};
// O nome por numero, produzido UMA vez no thread principal (o
// `to_string(Svc)` devolve std::string — alocacao — e no handler nao
// ha' alocamento).
constexpr std::size_t kSvcNomeTam = 16;
char g_nomes[kSvcContagensTam][kSvcNomeTam] = {};
const char* g_nomes_ptr[kSvcContagensTam] = {};
const char* nome_de_svc(std::uint32_t n) {
    if (n < kSvcContagensTam)
        return g_nomes_ptr[n];
    return "Svc?";
}

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
using GuestKind = g610::GuestModule::Kind;

const g610::GuestModule* find(const g610::GuestProgram& p, GuestKind k) {
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
static g610::GuestMemory* g_mem = nullptr;

// O programa, para o observer poder percorrer os modulos e varrer o .text
// quando o guest aborta. Sem simbolos, achar quem chamou a funcao que
// abortou e a unica forma de saber o que falhou.
static const g610::GuestProgram* g_prog = nullptr;
// Vista do despachante para o print de nome por handle no block #33.
static g610::SvcDispatcher* g_svc = nullptr;
static std::string g_scratch;

static volatile int g_samples = 0;
static volatile std::uint64_t g_sample_pc = 0;

// O dump por thread (definido em native_engine.cpp).
extern "C" void g610_dump_threads();

// O dump do quadro de arbitração (definido em svc_dispatcher.cpp).
extern "C" void g610_dump_arbiter();

// O numero de paragem e' parametrizavel (`IA_STOP_SAMPLES`, default 4):
// o boot de um jogo chega a fases que so' existem depois do 4 sample
// (o laco de render do titulo, LOG34), e sem a janela aberta o tracer
// corta o run no mesmo morro. 0 desarma a paragem por completo (o
// `timeout` do shell e o relatorio continuam como rede de seguranca).
//
// A leitura corre em contexto normal (inicializacao estatica, antes do
// main instalar qualquer handler) e nao dentro do on_alarm: getenv/
// strtoul nao sao async-signal-safe e o primeiro disparo do alarme
// (~1 s) fzia o strtoul morrer dentro do contexto de thread de guest
// (LOG37/39/40/42: SIGSEGV em 0x2841060000000a; 0 samples impressos).
static const unsigned long g_stop_samples =
    [] { const char* s = ::getenv("IA_STOP_SAMPLES");
         return s == nullptr ? 4ul : ::strtoul(s, nullptr, 0); }();

// **O batimento do steady-state** (`IA_HEARTBEAT=1`, mesma regra da env:
// leitura UMA vez aqui, nunca dentro do on_alarm — a lição do LOG42).
//
// Por que existe: o run padrão morre aos ~4 s PELO sampler (o
// `force_stop` do 4.º tick — medido LOGE1: "parou por: alarme de
// seguranca", pc=0x83b0bb3c) e NÃO por decisão do guest — o
// `ExitProcess` nunca chega a ser chamado. Com `IA_STOP_SAMPLES=0` o
// guest corre até ao `timeout` do shell; o steady-state do jogo é então
// o que este batimento descreve: por segundo, o delta do histograma de
// SVC — quem ainda trabalha, quem calou, e qual o novo serviço que
// entra. A impressão e' write(2) (contexto de sinal) e leitura dos
// contadores atomicos do observer.
static const bool g_batimenta = ::getenv("IA_HEARTBEAT") != nullptr;
static std::uint64_t g_batimento_antes[kSvcContagensTam] = {};

// **O retato do WaitSynchronization** (`IA_WAIT_TRACE=1`, mesma regra da
// env: leitura UMA vez aqui, nunca dentro do handler).
static const bool g_wait_trace = ::getenv("IA_WAIT_TRACE") != nullptr;
static std::atomic<int> g_foto_wait{0};

// **O dump por thread** (`IA_THREAD_TRACE=1`).
static const bool g_thread_trace = ::getenv("IA_THREAD_TRACE") != nullptr;

// O ultimo `SendSyncRequest`: o buffer do pedido (x1) e o da resposta (x3).
//
// A resposta do servico vai para x3, no espaco do guest. E' ai que o SDK le
// o Result. Se o nosso motor escrever a resposta no sitio errado, o SDK le
// lixo — e um Result invalido e' exactamente o que mata um jogo.
static std::uint64_t g_ultimo_req = 0;
static std::uint64_t g_ultimo_resposta = 0;
static std::uint64_t g_ultimo_handle = 0;
static bool g_havia_sync = false;
// Salta para aqui para parar o guest: um BRK, que o motor reconhece.
extern "C" void force_stop() { __asm__ volatile("brk #0xFFFC"); }

void on_alarm(int, siginfo_t*, void* raw) {
    auto* uc = static_cast<ucontext_t*>(raw);
    const auto pc = static_cast<std::uint64_t>(uc->uc_mcontext.pc);
    g_sample_pc = pc;
    ++g_samples;

    // **O batimento** (por segundo; a nota do g_batimenta acima explica
    // por que existe): o delta do histograma de SVC desde o batimento
    // anterior. Duas passadas: a primeira so' soma, a segunda imprime e
    // actualiza o instantaneo. Os nomes vêm da tabela pré-computada
    // (async-signal-safe, sem alocacao) e a impressao e' write(2).
    if (g_batimenta) {
        std::uint64_t delta = 0;
        for (std::size_t n = 0; n < kSvcContagensTam; ++n)
            delta += g_svc_contagens[n].load(std::memory_order_relaxed) -
                     g_batimento_antes[n];
        char b0[80];
        int k = std::snprintf(b0, sizeof b0,
                              "[bat %d] pc=0x%llx (delta +%llu)\n",
                              g_samples, (unsigned long long)pc,
                              (unsigned long long)delta);
        ::write(2, b0, static_cast<std::size_t>(k));
        unsigned linhas = 0;
        for (std::size_t n = 0; n < kSvcContagensTam && linhas < 12; ++n) {
            const std::uint64_t d =
                g_svc_contagens[n].load(std::memory_order_relaxed) -
                g_batimento_antes[n];
            if (d == 0)
                continue;
            char bb[80];
            const int kb = std::snprintf(
                bb, sizeof bb, "    0x%02zx %-24s %llu\n", n,
                nome_de_svc(static_cast<std::uint32_t>(n)),
                (unsigned long long)d);
            ::write(2, bb, static_cast<std::size_t>(kb));
            ++linhas;
        }
        if (linhas >= 12) {
            static const char mais[] = "    (mais omitidas)\n";
            ::write(2, mais, sizeof mais - 1);
        }
        for (std::size_t n = 0; n < kSvcContagensTam; ++n)
            g_batimento_antes[n] =
                g_svc_contagens[n].load(std::memory_order_relaxed);
    }

    // **O dump por thread** (gated por IA_THREAD_TRACE=1).
    if (g_thread_trace) {
        char cab[32];
        int kc = std::snprintf(cab, sizeof cab, "[thr %d]\n", g_samples);
        ::write(2, cab, static_cast<std::size_t>(kc));
        g610_dump_threads();
    }

    // **O quadro de arbitração** por tick — quem está em fila (mutex), quem
// está em cv e as PALAVRAS guest: o estado real da fila é o relológico
// do congelamento (a fila do run mistura as threads e os per-chamadas).
    if (g_thread_trace) {
        g610_dump_arbiter();
    }


    // O g_stop_samples e' lido uma vez em contexto normal (acima): a sua
    // leitura inicializante (getenv+strtoul) NAO pode correr dentro do
    // handler — e' async-signal-unsafe. Medida LOG42: o 1o on_alarm do run
    // (aos ~1 s) entrava no strtoul com o contexto de thread de guest e
    // fazia morrer o processo com SIGSEGV em 0x2841060000000a dentro do
    // __isoc23_strtoul (por isso os runs que faultam tinham 0 samples).
    if (g_stop_samples != 0 && g_samples >= g_stop_samples) {
        // Parar o guest: salta para uma rotina que faz BRK, que o motor
        // trata como saida. Sem isto o programa nunca mais volta ao host e o
        // relatorio — o unico rasto do que aconteceu — nunca e' impresso.
        uc->uc_mcontext.pc = reinterpret_cast<std::uint64_t>(&force_stop);
    }
    if (g_samples <= 2 || g_stop_samples == 0) {
        char b[416];
        // Caminhada de frames a partir do x29: *(x29+8) = o sitio de
        // retorno do chamador de cima (prologo stp x29, x30). So' derefs
        // directos (nada de biblioteca em contexto de sinal).
        std::uint64_t lr1 = 0, lr2 = 0, lr3 = 0, lr4 = 0;
        std::uint64_t fp = uc->uc_mcontext.regs[29];
        for (int nivel = 0; nivel < 4 && fp != 0; ++nivel) {
            const auto* f = reinterpret_cast<const volatile std::uint64_t*>(fp);
            const std::uint64_t ret = f[1];
            if (nivel == 0) lr1 = ret;
            else if (nivel == 1) lr2 = ret;
            else if (nivel == 2) lr3 = ret;
            else { lr4 = ret; break; }
            fp = f[0];
            if (fp == 0 || (fp & 7) != 0) break;
        }
        const int k = std::snprintf(
            b, sizeof b,
            "[sample %d] pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x19=0x%llx "
            "x29=0x%llx x30=0x%llx sp=0x%llx lr1=0x%llx lr2=0x%llx "
            "lr3=0x%llx lr4=0x%llx\n",
            g_samples, (unsigned long long)pc,
            (unsigned long long)uc->uc_mcontext.regs[0],
            (unsigned long long)uc->uc_mcontext.regs[1],
            (unsigned long long)uc->uc_mcontext.regs[2],
            (unsigned long long)uc->uc_mcontext.regs[19],
            (unsigned long long)uc->uc_mcontext.regs[29],
            (unsigned long long)uc->uc_mcontext.regs[30],
            (unsigned long long)uc->uc_mcontext.sp,
            (unsigned long long)lr1, (unsigned long long)lr2,
            (unsigned long long)lr3, (unsigned long long)lr4);
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

}

// Callback do motor. Assinatura async-signal-safe: só escreve em memória
// (o vector) e num buffer fixo. Nenhuma alocação, nenhum printf.
static std::atomic<std::uint32_t> g_svcs{0};

void on_svc(std::uint32_t number, g610::SvcContext& ctx, std::uint64_t pc,
            std::uint64_t sp, void* user) {
    // O contador total: atomico (era `volatile int ++` — a soma de dois
    // threads perde updates; o numero impresso `[svc #N]` ficava
    // duplicado e a conta do fim subestimada).
    const std::uint32_t n_svc = static_cast<std::uint32_t>(
        g_svcs.fetch_add(1, std::memory_order_relaxed)) + 1;
    // O cursor "sem rolar": cada thread ganha o SEU index (o
    // `fetch_add` e' atomico, dois em paralelo nunca caem no mesmo
    // slot) — e o escritor de cada slot e' um so'. Sem heap por
    // invocacao (o push_back das linhas velhas realocava o vector e
    // librava o bloco velho; dois threads no mesmo instante = free
    // duplo, medido no core 23/10).
    const std::uint64_t idx = static_cast<std::uint64_t>(
        g_primeiros_cursor.fetch_add(1, std::memory_order_relaxed));
    if (number < kSvcContagensTam) {
        g_svc_contagens[number].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_svc_contagens[kSvcContagensTam - 1].fetch_add(1,
                                                        std::memory_order_relaxed);
    }
    SvcCall c{};
    c.number = number;
    c.name   = nome_de_svc(number);
    for (int i = 0; i < 6; ++i)
        c.x[i] = ctx.x[i];
    c.pc = pc;
    c.sp = sp;
    if (idx < kSvcOrdenadoTam)
        g_primeiros[idx] = c;

    // O pedido de IPC, em bruto.
    //
    // `SendSyncRequest(handle, buffer, size)` — o buffer é a MessageBuffer do
    // ThreadLocalRegion (os primeiros 0x100 bytes), que é NOSSA memória e que
    // podemos ler. Antes de responder a um único comando de serviço é preciso
    // ver o que o guest perguntou: um pedido de `sm:` que se responde às
    // cegas é um palpite.
    //
    // A estrutura é a de todos os serviços IPC do Horizon
    // (switchbrew/IPC_services):
    //
    //     u32 type;                 0 = pedido, 1 = resposta
    //     u32 unknown;
    //     ServiceCmdArg args[8];    { char name[0x20]; u32 pad; u64 data; }
    //
    // total 8 + 8*0x28 = 0x148 bytes de cabeçalho.
    if ((number == 0x21 || number == 0x20) && g_mem != nullptr) {
        std::uint8_t m[0x148];
        std::string e;
        if (g_mem->read(ctx.x[1], m, sizeof m, e)) {
            std::uint32_t type, unk;
            std::memcpy(&type, m, 4);
            std::memcpy(&unk, m + 4, 4);
            std::printf("[ipc ] pedido em %llu (handle %llx) type=%u unk=0x%08x\n",
                        (unsigned long long)ctx.x[1],
                        (unsigned long long)ctx.x[0], type, unk);
            // Hexdump bruto: a estrutura assumida pode estar errada, e ver os
            // bytes é mais barato do queinferir a partir de uma leitura.
            for (int off = 0; off < 0x60; off += 16) {
                std::string hexs, txt;
                for (int k = 0; k < 16; ++k) {
                    char t[8];
                    std::snprintf(t, sizeof t, "%02x ", m[off + k]);
                    hexs += t;
                    const unsigned char ch = m[off + k];
                    txt += (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '.';
                }
                std::printf("        +%02x  %s  %s\n", off, hexs.c_str(), txt.c_str());
            }
            for (int a = 0; a < 8; ++a) {
                const std::uint8_t* p = m + 8 + a * 0x28;
                char nome[0x21] = {0};
                std::memcpy(nome, p, 0x20);
                std::uint64_t data;
                std::memcpy(&data, p + 0x20 + 4, 8);
                bool vazio = true;
                for (int k = 0; k < 0x20; ++k)
                    if (nome[k] != 0) vazio = false;
                if (vazio && data == 0)
                    continue;
                std::printf("        arg[%d] \"%s\"  data=0x%llx\n", a, nome,
                            (unsigned long long)data);
            }
        }
    }
    // As barras de SendSyncRequest: servico em x0, mensagem em x1.
    // LOG36: 911 `SendSyncRequest` (30/s), sempre deste stub
    // (0x83b0bb64), com os handles 0x981f1 e 0xa01f0. Os primeiros u32
    // da mensagem dizem o comando do servico — com isso o laco de 30 Hz
    // deixa de ser um palpite.
    if (number == 0x21 && g_mem != nullptr) {
        // 96 bytes: os 24 de cima, nos corpos com raw oni grande, nao
        // alcancam nem o MethodId. 24 words cobrem o CmifInHeader e a
        // carga nos shapes com Pointer+Send a 0x8.
        std::uint8_t mh[96];
        std::string e2;
        if (g_mem->read(ctx.x[1], mh, sizeof mh, e2)) {
            std::uint32_t mh32[24];
            std::memcpy(mh32, mh, sizeof mh32);
            // O nome mora na mesa do despachante, e construir std::string
            // DENTRO do handler por envio e' o caminho de corrupcao que
            // IA_NOOB existia para cortar (doc acima). Fica so' na primeira
            // vez que um handle aparece — uma alocacao por sessao nova.
            const std::uint32_t hnd = static_cast<std::uint32_t>(ctx.x[0]);
            static std::uint32_t vistos[64] = {0};
            constexpr std::size_t kVistosTam = sizeof vistos / sizeof vistos[0];
            bool novo = hnd != 0;
            std::size_t furo = kVistosTam;
            for (std::size_t i = 0; i < kVistosTam; ++i) {
                if (vistos[i] == hnd) { novo = false; break; }
                if (vistos[i] == 0) { furo = i; break; }
            }
            if (novo && furo < kVistosTam) {
                vistos[furo] = hnd;
                if (g_svc != nullptr) {
                    const std::string nome =
                        g_svc->service_name_for_tracer(hnd);
                    std::printf(
                        "[ipc ] sessao nova handle=%llx nome=%s\n",
                        (unsigned long long)hnd, nome.c_str());
                }
            }
            // **A etiqueta por tabela** (`g610::hsvc`): o par (handle, W0)
            // cruza kSessoes[] e o envio ganha o nome do servico + o
            // MethodId medido, sem nenhum caminho de despacho novo. A
            // posicao do MethodId no corpo depende do shape (medido,
            // doc do header): raw com descritores empurra o CMIF.
            {
                const char* tag = nullptr;
                std::uint32_t pos_metodo = ~0u;
                for (std::int32_t i = 0; i < g610::hsvc::kNumSessoes; ++i) {
                    const auto& s = g610::hsvc::kSessoes[i];
                    if (hnd == s.handle && mh32[0] == s.w0) {
                        tag = s.servico;
                        // raw 0x8.. (descritores 0 ou Ptr+Send par): CMIF
                        // a 0x20; com C-buf (Receive=1 MapData 12 bytes):
                        // CMIF a 0x30.
                        switch (s.w0) {
                        case 0x01000006u:
                            pos_metodo = 14;  // W14 = @0x30 + 8
                            break;
                        default:
                            pos_metodo = 10;  // W10 = @0x20 + 8
                            break;
                        }
                        break;
                    }
                }
                if (tag != nullptr && pos_metodo < 24u) {
                    std::uint32_t metodo = mh32[pos_metodo];
                    // O MethodId do CMIF: na rotina (b) o CmifIn fica
                    // depois do dominio (InRawSize 32-largo): W14; na
                    // rotina (a)/nvdrv: W10.
                    const char* nome_m = nullptr;
                    if (hnd == g610::hsvc::kSessoes[0].handle) {
                        for (std::int32_t j = 0;
                             j < g610::hsvc::kNumMetodosFspSrv; ++j)
                            if (g610::hsvc::kMetodosFspSrv[j].id == metodo)
                                nome_m = g610::hsvc::kMetodosFspSrv[j].nome;
                    } else {
                        for (std::int32_t j = 0;
                             j < g610::hsvc::kNumMetodosNvDrv; ++j)
                            if (g610::hsvc::kMetodosNvDrv[j].id == metodo)
                                nome_m = g610::hsvc::kMetodosNvDrv[j].nome;
                    }
                    std::printf(
                        "[ipc ] #33 handle=%llx etq=%s method=0x%x%s\n",
                        (unsigned long long)ctx.x[0], tag, metodo,
                        nome_m != nullptr ? "" : " (novo no mapa)");
                }
            }
            std::printf(
                "[ipc ] #33 handle=%llx msg=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                (unsigned long long)ctx.x[0],
                mh32[0], mh32[1], mh32[2], mh32[3], mh32[4], mh32[5],
                mh32[6], mh32[7], mh32[8], mh32[9], mh32[10], mh32[11],
                mh32[12], mh32[13], mh32[14], mh32[15], mh32[16],
                mh32[17], mh32[18], mh32[19], mh32[20], mh32[21],
                mh32[22], mh32[23]);
        }
    }
    // **O portão do wait** (`IA_WAIT_TRACE=1`, env lida fora do handler,
    // à maneira do `g_batimenta`).
    //
    // Nos 120 s de LOGE2 o steady-state resume-se a dois padrões: a bomba
    // (Wait/Signal na cv 0xd9c2c228, ~630/s) e este tríptico de
    // `WaitSynchronization` 0x18 a 30/s com x0=0x30fffcfac,
    // x1=0x30fffd1c4, x2=1, x5=0x28, até ao fim do run. O x0 e' da pilha do
    // guest — não é array de handles conhecido: o portão mora na variável
    // no sitio. O retrato lê a janela dos dois ponteiros (16 bytes cada)
    // e marca os handles não-nulos UMA VEZ por handle novo (o mesmo
    // padrão "vistos" do bloco 0x21, sem alloc repetido). Ritmo: as 3
    // primeiras chamadas e depois uma em 600 (30/s medido → ~1 foto a
    // cada 20 s).
    if (number == 0x18 && g_wait_trace && g_mem != nullptr) {
        const int n_foto = g_foto_wait.fetch_add(1, std::memory_order_relaxed);
        if (n_foto < 3 || (n_foto % 600) == 0) {
            std::uint8_t w0[16] = {};
            std::uint8_t w1[16] = {};
            std::string e3;
            const bool ok0 = g_mem->read(ctx.x[0], w0, sizeof w0, e3);
            const bool ok1 = g_mem->read(ctx.x[1], w1, sizeof w1, e3);
            std::printf(
                "[wait] n=%d x0=0x%llx x1=0x%llx x2=%llu x3=0x%llx "
                "x4=0x%llx x5=0x%llx pc=0x%llx\n",
                n_foto, (unsigned long long)ctx.x[0],
                (unsigned long long)ctx.x[1], (unsigned long long)ctx.x[2],
                (unsigned long long)ctx.x[3], (unsigned long long)ctx.x[4],
                (unsigned long long)ctx.x[5], (unsigned long long)pc);
            if (ok0) {
                std::printf("    [@x0] %02x %02x %02x %02x  %02x %02x %02x "
                            "%02x  %02x %02x %02x %02x  %02x %02x %02x %02x\n",
                            w0[0], w0[1], w0[2], w0[3], w0[4], w0[5], w0[6],
                            w0[7], w0[8], w0[9], w0[10], w0[11], w0[12],
                            w0[13], w0[14], w0[15]);
            } else {
                std::printf("    [@x0] leitura falhou (%s)\n", e3.c_str());
            }
            if (ok1) {
                std::printf("    [@x1] %02x %02x %02x %02x  %02x %02x %02x "
                            "%02x  %02x %02x %02x %02x  %02x %02x %02x %02x\n",
                            w1[0], w1[1], w1[2], w1[3], w1[4], w1[5], w1[6],
                            w1[7], w1[8], w1[9], w1[10], w1[11], w1[12],
                            w1[13], w1[14], w1[15]);
            } else {
                std::printf("    [@x1] leitura falhou (%s)\n", e3.c_str());
            }
            // Os u32 nas duas janelas, tomados como handles. UMA vez por
            // handle novo: a mesa do dsp atrás do nome (o mesmo esquema
            // do bloco 0x21).
            {
                static std::uint32_t vistos_wait[64] = {0};
                constexpr std::size_t kWvistosTam =
                    sizeof vistos_wait / sizeof vistos_wait[0];
                const std::uint32_t cand0 = *reinterpret_cast<const std::uint32_t*>(w0);
                const std::uint32_t cand1 = *reinterpret_cast<const std::uint32_t*>(w1);
                const std::uint32_t cands[2] = {cand0, cand1};
                for (const std::uint32_t h : cands) {
                    if (h == 0)
                        continue;
                    bool novo = true;
                    std::size_t furo = kWvistosTam;
                    for (std::size_t i = 0; i < kWvistosTam; ++i) {
                        if (vistos_wait[i] == h) { novo = false; break; }
                        if (vistos_wait[i] == 0) { furo = i; break; }
                    }
                    if (novo && furo < kWvistosTam) {
                        vistos_wait[furo] = h;
                        if (g_svc != nullptr) {
                            const std::string nome =
                                g_svc->service_name_for_tracer(h);
                            std::printf(
                                "[wait] handle=%llx nome=%s\n",
                                (unsigned long long)h, nome.c_str());
                        }
                    }
                }
            }
        }
    }
   // Endereços de retorno do guest na stack, para seguir de onde veio o
    // abort. Um `Break(x0=0)` diz QUE valor estava a zero; a stack diz
    // QUEM o tinha.
    // ── a janela de CODIGO a volta do abort ────────────────────────────
    //
    // `Break(x0=0)` diz que um valor era zero. A stack diz quem o tinha. E o
    // que descobre o PORQUE e' o codigo a volta da chamada — e la que se ve
    // que comparacao falhou.
    //
    // Sem simbolos, o `x30` aponta para a instrucao a seguir ao `bl Break`, e
    // os bytes a volta dizem o que a funcao estava a testar. E a diferenca entre
    // "o SDK abortou" e "o SDK abortou porque X era zero".
    //
    // gpr[] e nao x[]: x[] so tem 6 entradas e x[30] seria memoria a mais.
    if (number == 0x26 && ctx.gpr[30] >= 0x80000000ull &&
        ctx.gpr[30] < 0x98000000ull && g_mem != nullptr) {
        const std::uint64_t inicio = ctx.gpr[30] - 0x80;
        unsigned char janela[0x100];
        std::string e;
        if (g_mem->read(inicio, janela, sizeof janela, e)) {
            char caminho[64];
            std::snprintf(caminho, sizeof caminho, "/tmp/g610_break_%llx.bin",
                          (unsigned long long)inicio);
            std::FILE* f = std::fopen(caminho, "wb");
            if (f != nullptr) {
                std::fwrite(janela, 1, sizeof janela, f);
                std::fclose(f);
                std::printf("[cod] janela 0x%llx..0x%llx -> %s\n",
                            (unsigned long long)inicio,
                            (unsigned long long)(inicio + sizeof janela), caminho);
            }
        } else {
            std::printf("[cod] nao consegui ler 0x%llx: %s\n",
                        (unsigned long long)inicio, e.c_str());
        }
    }

    // ── a MENSAGEM do abort ─────────────────────────────────────────────
    //
    // O codigo desassemblado do sitio da chamada diz o que se passa:
    //
    //     stp x29, x30, [sp, #-16]!
    //     mov x1, x0            ; x1 = o argumento recebido
    //     orr w2, wzr, #4
    //     mov w0, wzr           ; x0 = 0
    //     bl  Break
    //
    // Ou seja: a funcao passa o seu argumento para x1. Se esse argumento for
    // uma string, entao a mensagem do abort esta na memoria do guest e
    // pode ler-se. Nao e preciso seguir a cadeia de chamadas: a mensagem diz o
    // que falhou, em portugues do SDK, com o nome da funcao e o ficheiro.
    //
    // E uma string a readable que vale mais do que vinte palpites sobre o
    // formato da resposta IPC.
    if (number == 0x26 && g_mem != nullptr) {
        for (int arg = 0; arg <= 7; ++arg) {
            const std::uint64_t p = ctx.gpr[arg];
            if (p < 0x80000000ull || p >= 0x98000000ull) {
                if (std::getenv("IA_IPC_TRACE") != nullptr) {
                    std::printf("[msg] x%d = 0x%llx (fora da gama do guest)\n",
                                arg, (unsigned long long)p);
                }
                continue;
            }
            char buf[256];
            std::string e;
            if (!g_mem->read(p, buf, sizeof buf - 1, e)) {
                std::printf("[msg] x%d = 0x%llx: a leitura falhou (%s)\n", arg,
                            (unsigned long long)p, e.c_str());
                continue;
            }
            buf[sizeof buf - 1] = 0;
            // So interessa se for mesmo texto.
            int legiveis = 0;
            for (const char* q = buf; *q; ++q)
                if (*q >= 0x20 && *q < 0x7F)
                    ++legiveis;
                else
                    break;
            std::printf("[msg] x%d = 0x%llx -> \"%s\" (%d legiveis)\n", arg,
                        (unsigned long long)p, buf, legiveis);
            if (legiveis < 4)
                continue;
        }
        std::printf("[msg] w2 = %u\n",
                    (unsigned)(ctx.gpr[2] & 0xFFFFFFFFu));

        // A ZONA: a mensagem do assert nao esta no ponteiro, esta num
        // buffer perto. Despeja a janela a volta de cada ponteiro, para
        // ver a string que o SDK esta a apontar.
        //
        // No abort deste jogo, `x1` aponta para um buffer com 1 byte
        // legivel ("@"), e o `x3` (o comprimento) e' 0xe. A string esta
        // algures nas vizinhas. Uma janela de 0x100 em volta de cada
        // ponteiro mostra a string, e o offset dela diz o que o SDK
        // codificou.
        for (int arg = 0; arg <= 7; ++arg) {
            const std::uint64_t p = ctx.gpr[arg];
            if (p < 0x80000000ull || p >= 0x98000000ull)
                continue;
            const std::uint64_t inicio = p - 0x40;
            unsigned char janela[0x100];
            std::string e2;
            if (!g_mem->read(inicio, janela, sizeof janela, e2))
                continue;
            // As strings legiveis na janela, com o seu offset.
            std::uint64_t off = 0;
            while (off < sizeof janela) {
                std::uint64_t n = 0;
                while (off + n < sizeof janela && janela[off + n] >= 0x20 &&
                       janela[off + n] < 0x7F)
                    ++n;
                if (n >= 6) {
                    std::printf("[zona] +0x%04llx (ptr+0x%llx): \"%.*s\"\n",
                                (unsigned long long)(inicio - p + off),
                                (unsigned long long)(off - (p - inicio)), (int)n,
                                (const char*)janela + off);
                }
                ++off;
                while (off < sizeof janela && janela[off] != 0)
                    ++off;
                ++off;
            }
        }
    }

    // ── o CHAMADOR, pela stack ──────────────────────────────────────────
    //
    // A função que abortou e' `stp x29, x30, [sp, #-16]!` seguida de
    // `mov x1, x0` / `orr w2, wzr, #4` / `mov w0, wzr` / `bl Break`. Logo o
    // x30 dela esta no topo da stack — e o x30 e' o endereco de RETORNO, que
    // e' o chamador.
    //
    // Nao ha `bl` nenhum para essa função em nenhum dos três módulos: a
    // chamada e' indirecta, por um stub tipo PLT. Varrer 25 MB de `.text` a
    // procurar o alvo deu "nenhuma chamada directa encontrada", e a stack
    // deu a resposta à primeira. A stack e' o caminho que nao depende de haver
    // símbolos, e é muito mais barato.
    // ── as STRINGS de um modulo ──────────────────────────────────────────
    //
    // `IA_STRINGS=main` extrai todo o texto ASCII imprimivel dos segmentos de
    // dados de um modulo e imprime-o.
    //
    // Existe por causa de uma descoberta: o `main` que estamos a correr
    // chama-se `piDroid_Test_ARM` e NAO tem banco de shaders — `GshD` nao
    // aparece no ficheiro. Nao e' um jogo comercial: e' um programa de teste
    // em libnx. Isso muda a estrategia toda, e a forma de o confirmar foi ver
    // o que o programa diz de si proprio. E, por acaso, a melhor coisa que
    // pode ser: codigo de nicho, e o texto esta no sitio.
    //
    // `IA_STR=0x...` le texto num endereco avulso, para quando a string e'
    // num `.bss` que a varrimento nao pega.
    if (number == 0x26 && g_mem != nullptr && g_prog != nullptr) {
        const char* qual = std::getenv("IA_STRINGS");
        if (qual != nullptr) {
            for (const auto& m : g_prog->modules()) {
                if (std::string(m.name()) != qual)
                    continue;
                // Os segmentos 1 e 2 sao a rodata e a data. O texto vive la.
                for (int sg = 1; sg <= 2; ++sg) {
                    const std::uint64_t ini = m.vaddr_base + m.nso.segments[sg].vaddr;
                    const std::size_t tam = static_cast<std::size_t>(m.nso.segments[sg].size);
                    if (tam == 0 || tam > (16u << 20))
                        continue;
                    std::vector<char> buf(tam);
                    std::string e5;
                    if (!g_mem->read(ini, buf.data(), tam, e5)) {
                        std::printf("[txt] %s seg%d: nao li: %s\n", m.name(), sg,
                                    e5.c_str());
                        continue;
                    }
                    std::size_t i2 = 0;
                    while (i2 < buf.size()) {
                        std::size_t j = i2;
                        while (j < buf.size() && buf[j] >= 0x20 && buf[j] < 0x7F)
                            ++j;
                        if (j - i2 >= 8)
                            std::printf("[txt] %s 0x%llx: %.*s\n", m.name(),
                                        (unsigned long long)(ini + i2),
                                        static_cast<int>(j - i2), buf.data() + i2);
                        i2 = (j == i2) ? i2 + 1 : j + 1;
                    }
                }
            }
        }

        // `IA_FIND=GshD` procura um magic na imagem JA DESCOMPRIMIDA de
        // todos os modulos.
        //
        // Existe porque o `grep` no ficheiro NSO nao encontra o banco de
        // shaders, e a primeira conclusao — "este programa nao tem shaders" —
        // era errada: o programa foi ligado com `NX-NXFP2-a64` e com
        // `glslc.nss`, que e' o compilador Maxwell da NVIDIA. O banco nao
        // esta no ficheiro porque os segmentos estao comprimidos, e o que se
        // procurou foi o ficheiro.
        //
        // O mesmo magic em dois sitios ditos de formas diferentes da a
        // resposta correcta: um e' a coisa, o outro e' a mesma coisa.
        const char* vf = std::getenv("IA_FIND");
        if (vf != nullptr && std::strlen(vf) == 4) {
            for (const auto& m : g_prog->modules()) {
                const std::size_t tam = m.nso.image.size();
                if (tam < 4)
                    continue;
                std::vector<char> img(tam);
                std::string e6;
                if (!g_mem->read(m.vaddr_base, img.data(), tam, e6))
                    continue;
                for (std::size_t k = 0; k + 4 <= tam; ++k) {
                    if (std::memcmp(img.data() + k, vf, 4) != 0)
                        continue;
                    std::printf("[find] %-5s \"%s\" no offset 0x%zx "
                                "(endereco 0x%llx)\n",
                                m.name(), vf, k,
                                (unsigned long long)(m.vaddr_base + k));
                    // Os 64 bytes a seguir: um cabecalho de banco de shaders
                    // diz quantos shaders tem e onde estao.
                    std::printf("        ");
                    for (int b = 0; b < 64 && k + 4 + b < tam; ++b)
                        std::printf("%02x%s",
                                    static_cast<unsigned char>(img[k + 4 + b]),
                                    (b % 16 == 15) ? " " : " ");
                    std::printf("\n");
                }
            }
        }

        const char* vs = std::getenv("IA_STR");
        if (vs != nullptr) {
            const std::uint64_t p0 = std::strtoull(vs, nullptr, 0);
            for (int n = 0; n < 4; ++n) {
                char txt[256];
                std::string e3;
                if (!g_mem->read(p0 + n * 256, txt, sizeof txt - 1, e3)) {
                    std::printf("[str] 0x%llx: nao li: %s\n",
                                (unsigned long long)(p0 + n * 256), e3.c_str());
                    continue;
                }
                txt[sizeof txt - 1] = 0;
                std::printf("[str] 0x%llx: \"%s\"\n",
                            (unsigned long long)(p0 + n * 256), txt);
                std::printf("      hex:");
                for (int b = 0; b < 64; ++b)
                    std::printf("%02x%s", static_cast<unsigned char>(txt[b]),
                                (b % 16 == 15) ? "\n          " : " ");
                std::printf("\n");
            }
        }
    }

    // A assercao do SDK: `OnAssertionFailure(type, file, line, msg, ...)`.
    //
    // A mensagem esta no x5 (o quinto argumento), e o ficheiro no x2/x3.
    // E' a unica forma de saber **o que** falhou: o SDK aborta por uma
    // assercao, e a assercao tem o ficheiro e a linha.
    // **O instante do `Break`** — o guest abortou. E o que separa o
    // tempo do **arranque** (o que o nosso motor custou) do tempo do
    // **loop infinito do `Abort`** (que nao e' nosso).
    if (number == 0x26) {
        struct timespec agora {};
        clock_gettime(CLOCK_MONOTONIC, &agora);
        const auto ns = static_cast<long long>(agora.tv_sec) * 1000000000LL + agora.tv_nsec;
        if (g_inicio_ns != 0) {
            std::printf("[arranque] o guest abortou em %.3f ms, apos %llu SVC(s)\n",
                        (ns - g_inicio_ns) / 1e6,
                        (unsigned long long)g_svcs.load());
        }
    }

    if (number == 0x26 && g_mem != nullptr) {
        // Todos os registos: a assercao do SDK passa (type, file, line,
        // msg, ...) e nao sabemos qual o que interessa.
        // A string da assercao: o SDK passa um ponteiro para uma
        // string **do proprio SDK** (o `.rodata` dele), que nao esta no
        // espaco do guest. Por isso o `g_mem->read` nao a ve — e a
        // unica forma e' o dump do modulo.
        std::printf("[brk] registos:");
        for (int arg = 0; arg <= 7; ++arg) {
            std::printf(" x%d=0x%llx", arg,
                        (unsigned long long)ctx.gpr[arg]);
        }
        std::printf("  pc=0x%llx\n", (unsigned long long)ctx.pc);
        // **Todos os registos.** A cadeia de chamadas do abort passa pelo
        // `x29` (o frame pointer); sem os registos todos, o `x22` — que no
        // `nn::fs` e' o caminho do ficheiro — fica a ser uma adivinhaca.
        std::printf("[brk] gpr:");
        for (int arg = 0; arg <= 30; ++arg) {
            std::printf(" x%d=0x%llx", arg,
                        (unsigned long long)ctx.gpr[arg]);
        }
        std::printf("\n");
        for (int arg = 1; arg <= 6; ++arg) {
            const std::uint64_t p = ctx.gpr[arg];
            if (p == 0 || p >= 0x98000000ull)
                continue;
            char buf[512];
            std::string e;
            if (!g_mem->read(p, buf, sizeof buf - 1, e))
                continue;
            buf[sizeof buf - 1] = 0;
            int legiveis = 0;
            for (const char* q = buf; *q; ++q)
                if (*q >= 0x20 && *q < 0x7F)
                    ++legiveis;
                else
                    break;
            if (legiveis < 4)
                continue;
            std::printf("[ass] x%d = 0x%llx -> \"%s\"\n", arg,
                        (unsigned long long)p, buf);
        }
    }

    // **A pilha inteira da thread que aborta.** O `[quem]` abaixo le 4
    // palavras e salta as que nao sao enderecos de codigo — o que esconde
    // a cadeia de chamadas. O abort do SDK (`nn::diag::detail::AbortImpl`)
    // tem uma frame de 0x190 bytes, logo o chamador esta' muito acima do
    // `sp`: sem a pilha toda nao se chega ao chamador.
    if (number == 0x26 && g_ultimo_req != 0 && g_mem != nullptr) {
        // **O buffer de IPC do guest, no momento do abort.**
        //
        // E' o que o SDK leu por ultimo — a resposta que ele rejeitou.
        // Sem isto, so se sabe que a resposta "parece certa"; com isto,
        // ve-se o que o guest tem de facto.
        std::uint8_t buf[0x60]{};
        std::string e7;
        if (g_mem->read(g_ultimo_req, buf, sizeof buf, e7)) {
            std::printf("[ipcB] buffer do guest em %llu:\n",
                        (unsigned long long)g_ultimo_req);
            for (std::size_t i = 0; i < sizeof buf; i += 16) {
                std::printf("[ipcB]  +%02zx ", i);
                for (std::size_t j = 0; j < 16; ++j)
                    std::printf("%02x ", buf[i + j]);
                std::printf(" |");
                for (std::size_t j = 0; j < 16; ++j) {
                    const char c = (char)buf[i + j];
                    std::printf("%c", (c >= ' ' && c <= '~') ? c : '.');
                }
                std::printf("|\n");
            }
        }
    }

    if (number == 0x26 && sp != 0 && g_mem != nullptr) {
        std::uint64_t palavras[128] = {0};
        std::string e;
        if (g_mem->read(sp, palavras, sizeof palavras, e)) {
            std::printf("[pilha] sp=0x%llx\n", (unsigned long long)sp);
            for (int k = 0; k < 128; ++k) {
                const std::uint64_t v = palavras[k];
                const char* marca = (v >= 0x80000000ull && v < 0x98000000ull) ? " <- codigo"
                                                                           : "";
                if (v == 0 && k > 8)
                    continue;
                std::printf("[pilha] +0x%03x = 0x%016llx%s\n", k * 8,
                            (unsigned long long)v, marca);
            }
            // **A pilha toda, em binario.** A cadeia de chamadas passa por
            // varias frames (o `AbortImpl` do SDK tem uma frame de 0x190
            // bytes), e a cadeia so' se ve seguindo o `x29`. O ficheiro
            // permite analyses locais sem voltar a correr o guest.
            {
                const std::uint64_t inicio = sp - 0x100;
                std::vector<unsigned char> janela(0x2000);
                if (g_mem->read(inicio, janela.data(), janela.size(), e)) {
                    char caminho[64];
                    std::snprintf(caminho, sizeof caminho, "/tmp/g610_pilha.bin");
                    std::FILE* f = std::fopen(caminho, "wb");
                    if (f != nullptr) {
                        std::fwrite(janela.data(), 1, janela.size(), f);
                        std::fclose(f);
                        std::printf("[pilha] despejada: %s (base 0x%llx, %zu bytes)\n",
                                    caminho, (unsigned long long)inicio, janela.size());
                    }
                }
            }
        }
    }

    if (number == 0x26 && sp != 0 && g_mem != nullptr) {
        std::uint64_t words[4] = {0, 0, 0, 0};
        std::string e;
        if (g_mem->read(sp, words, sizeof words, e)) {
            for (int k = 0; k < 4; ++k) {
                const std::uint64_t v = words[k];
                if (v < 0x80000000ull || v >= 0x98000000ull)
                    continue;
                // Centrada no endereco de retorno, e larga. A primeira
                // janela media 0x100 e terminava exactamente no endereço de
                // retorno; o objdump mostrava um `...` que parecia um buraco
                // de zeros e era só o fim dos dados.
                const std::uint64_t inicio = v - 0x80;
                unsigned char janela[0x200];
                if (!g_mem->read(inicio, janela, sizeof janela, e))
                    continue;
                char caminho[64];
                std::snprintf(caminho, sizeof caminho, "/tmp/g610_chamador_%llx.bin",
                              (unsigned long long)inicio);
                std::FILE* f = std::fopen(caminho, "wb");
                if (f == nullptr)
                    continue;
                std::fwrite(janela, 1, sizeof janela, f);
                std::fclose(f);
                std::printf("[quem] stack+%d = 0x%llx (retorno) -> %s\n",
                            k * 8, (unsigned long long)v, caminho);
            }
        }
    }

    if (number == 0x26 && sp != 0 && g_mem != nullptr) {
        std::uint64_t words[4] = {0, 0, 0, 0};
        std::string e;
        if (g_mem->read(sp, words, sizeof words, e)) {
            for (int k = 0; k < 4; ++k) {
                const std::uint64_t v = words[k];
                if (v < 0x80000000ull || v >= 0x98000000ull)
                    continue;
                // O endereco de retorno aponta para a instrucao a seguir
                // ao `bl`; a janela e' centrada nele.
                // Centrada no endereco de retorno, e larga: a janela
                // anterior terminava exactamente nele, e o objdump mostrava
                // um `...` que parecia um buraco de zeros e nao era o fim dos
                // dados. Um ficheiro de despejo que acaba onde o olho esta a
                // olhar e' pior do que um ficheiro menor mas completo.
                const std::uint64_t inicio = v - 0x80;
                unsigned char janela[0x200];
                if (!g_mem->read(inicio, janela, sizeof janela, e))
                    continue;
                char caminho[64];
                std::snprintf(caminho, sizeof caminho, "/tmp/g610_chamador_%llx.bin",
                              (unsigned long long)inicio);
                auto cod_de = [&](std::size_t i) {
                    return static_cast<std::uint32_t>(janela[i * 4]) |
                           (static_cast<std::uint32_t>(janela[i * 4 + 1]) << 8) |
                           (static_cast<std::uint32_t>(janela[i * 4 + 2]) << 16) |
                           (static_cast<std::uint32_t>(janela[i * 4 + 3]) << 24);
                };
                std::FILE* f = std::fopen(caminho, "wb");
                if (f == nullptr)
                    continue;
                std::fwrite(janela, 1, sizeof janela, f);
                std::fclose(f);
                std::printf("[quem] stack+%d = 0x%llx (retorno) -> %s\n",
                            k * 8, (unsigned long long)v, caminho);

                // As STRINGS que a função usa.
                //
                // Num SDK sem símbolos, o nome de uma função não existe — mas
                // o texto que ela imprime existe, e está na `.rodata`. O
                // código forma esses endereços com o par classico
                //
                //     adrp xN, pagina
                //     add  xN, xN, #offset
                //
                // Varrendo o par e lendo o que está lá, sai o nome da
                // função sem uma tabela de símbolos. E o nome diz o que
                // falhou melhor do que vinte palpites sobre o formato de uma
                // resposta IPC.
                for (std::size_t j = 0; j + 1 < sizeof janela / 4; ++j) {
                    const std::uint32_t w0 = cod_de(j);
                    if ((w0 & 0x9F000000u) != 0x90000000u)   // nao e' um ADRP
                        continue;
                    const std::uint32_t w1 = cod_de(j + 1);
                    if ((w1 & 0xFFC00000u) != 0x91000000u)   // nao e' um ADD imm
                        continue;
                    // Rd do ADRP tem de ser o Rn do ADD — é o mesmo
                    // registo. A primeira versão comparava `(w1 >> 5)` sem
                    // máscara, que includes os 12 bits do imediato, com o
                    // número do registo: nunca dava certo e o bloco
                    // silenciosamente não encontrava nada.
                    const std::uint32_t rd_adrp = w0 & 0x1Fu;
                    const std::uint32_t rn_add = (w1 >> 5) & 0x1Fu;
                    if (rd_adrp != rn_add)
                        continue;

                    // ADRP: immlo em [30:29], immhi em [23:5], 21 bits no
                    // total, com sinal, deslocados 12 — e o alvo e' a pagina
                    // do PC mais esse deslocamento. Sem sinal, quase todos os
                    // enderecos saem errados e a lista fica vazia sem dizer
                    // porque.
                    const std::int64_t immlo = static_cast<std::int64_t>((w0 >> 29) & 3u);
                    const std::int64_t immhi = static_cast<std::int64_t>((w0 >> 5) & 0x7FFFFu);
                    std::int64_t imm = ((immhi << 2) | immlo) << 12;
                    if (imm & (1ll << 32))
                        imm |= ~((1ll << 33) - 1);          // sinal a 21 bits
                    const std::int64_t off = static_cast<std::int64_t>(w1 & 0x000FFFFFu);
                    const std::uint64_t pc = inicio + j * 4;
                    const std::uint64_t alvo =
                        static_cast<std::uint64_t>(static_cast<std::int64_t>(pc & ~0xFFFull) +
                                                   imm + off);
                    if (alvo < 0x80000000ull || alvo >= 0x98000000ull)
                        continue;
                    char txt[200];
                    std::string e2;
                    if (!g_mem->read(alvo, txt, sizeof txt - 1, e2))
                        continue;
                    txt[sizeof txt - 1] = 0;
                    int n = 0;
                    while (txt[n] >= 0x20 && txt[n] < 0x7F)
                        ++n;
                    if (n < 4)
                        continue;
                    txt[n] = 0;
                    std::printf("[str] 0x%llx -> \"%s\"\n",
                                (unsigned long long)alvo, txt);
                }                std::printf("[quem] stack+%d = 0x%llx (retorno) -> %s\n",
                            k * 8, (unsigned long long)v, caminho);
            }
        }
    }

    if (number == 0x26 && sp != 0 && g_mem != nullptr) {
        std::uint64_t slots[96];
        std::string e;
        if (g_mem->read(sp, slots, sizeof slots, e)) {
            std::printf("[stk] retorno a partir de Break:\n");
            for (int i = 0; i < 96; ++i) {
                const std::uint64_t v = slots[i];
                if (v >= 0x80000000ull && v < 0x98000000ull)
                    std::printf("      sp+%4d  0x%016llx\n", i * 8,
                                (unsigned long long)v);
            }
        }
    }

    // A mensagem do abort, em texto.
    //
    // O SDK chama o abort assim (medido no `sdk` desassemblado, offset
    // 0x1801d0):
    //
    //     mov  x1, x0        ; x1 = a mensagem
    //     orr  w2, wzr, #4
    //     mov  w0, wzr
    //     bl   Break
    //
    // Logo o `x1` do `Break` e' o ponteiro para a mensagem que o SDK
    // construiu — e ela diz o que falhou, em texto. E' a unica pista que
    // nao passa por adivinhar codigo a desassemblar.
    // A resposta do IPC anterior: o observer corre ANTES do SVC ser
    // despachado, portanto quando chega um SVC novo o buffer de mensagem
    // ja tem a resposta do anterior. E' o unico sitio onde se pode ver o
    // Result que o guest vai ler.
    if (g_havia_sync && g_mem != nullptr) {
        std::uint8_t buf[96]{};
        std::string e5;
        if (g_mem->read(g_ultimo_req, buf, sizeof buf, e5)) {
            std::printf("[ipcR] handle=0x%llx  resposta: ", (unsigned long long)g_ultimo_handle);
            for (std::size_t i = 0; i < sizeof buf; i += 16) {
                std::printf("  ");
                for (std::size_t j = 0; j < 16; ++j)
                    std::printf("%02x ", buf[i + j]);
                std::printf(" |");
                for (std::size_t j = 0; j < 16; ++j) {
                    const char c = (char)buf[i + j];
                    std::printf("%c", (c >= ' ' && c <= '~') ? c : '.');
                }
                std::printf("|\n");
            }
            const std::uint32_t result = (std::uint32_t)buf[0x30] |
                                         ((std::uint32_t)buf[0x31] << 8) |
                                         ((std::uint32_t)buf[0x32] << 16) |
                                         ((std::uint32_t)buf[0x33] << 24);
            std::printf("[ipcR]   Result em +0x30 = 0x%08x\n", result);
        }
    }

    // O `CreateThread`: despejar o codigo do entry point, **tal como o
    // motor o tem em memoria**. E' a forma de comparar a nossa
    // descompressao com a do `hactool`.
    if (number == 0x08 && ctx.x[1] != 0 && g_mem != nullptr) {
        std::uint8_t buf[64]{};
        std::string e6;
        if (g_mem->read(ctx.x[1], buf, sizeof buf, e6)) {
            std::printf("[thrd] entry point 0x%llx:", (unsigned long long)ctx.x[1]);
            for (auto b : buf)
                std::printf(" %02x", b);
            std::printf("\n");
        }
    }

    if (number == 0x21) {
        g_ultimo_req     = ctx.x[1];
        g_ultimo_resposta = ctx.x[3];
        g_ultimo_handle  = ctx.x[0];
        g_havia_sync     = true;
    } else {
        g_havia_sync = false;
    }

    if (number == 0x26 && ctx.x[1] != 0 && g_mem != nullptr) {
        char txt[600];
        const std::size_t n = 560;
        std::string e2;
        if (g_mem->read(ctx.x[1], txt, n, e2)) {
            txt[n] = 0;
            std::size_t fim = 0;
            while (fim < n && txt[fim] != 0 && txt[fim] >= ' ' && txt[fim] <= '~')
                ++fim;
            std::printf("[msg ] Break: x1=0x%llx  \"%s\"\n",
                        (unsigned long long)ctx.x[1], std::string(txt, fim).c_str());
            // Hexdump: a mensagem pode nao comecar no x1 (pode ter um
            // cabecalho), e o que interessa e' ver os bytes.
            std::printf("[msg ] bytes em x1:\n");
            for (std::size_t i = 0; i < 128; i += 16) {
                std::printf("      ");
                for (std::size_t j = 0; j < 16; ++j)
                    std::printf("%02x ", (unsigned char)txt[i + j]);
                std::printf(" |");
                for (std::size_t j = 0; j < 16; ++j) {
                    const char c = txt[i + j];
                    std::printf("%c", (c >= ' ' && c <= '~') ? c : '.');
                }
                std::printf("|\n");
            }
            // As strings da stack do guest. A mensagem do SDK e' uma
            // string construida na stack; e' a unica forma de saber o que
            // falhou sem adivinhar codigo a desassemblar.
            {
                const std::uint64_t base = 0x847f0000ull;
                const std::size_t   n    = 0x10000;
                std::vector<char> buf(n);
                std::string e4;
                if (g_mem->read(base, buf.data(), n, e4)) {
                    std::printf("[msg ] strings na stack do guest:\n");
                    std::size_t i = 0;
                    while (i < n) {
                        if (buf[i] >= ' ' && buf[i] <= '~') {
                            std::size_t j = i;
                            while (j < n && buf[j] >= ' ' && buf[j] <= '~')
                                ++j;
                            if (j - i >= 8) {
                                std::printf("      0x%llx  \"%.*s\"\n",
                                            (unsigned long long)(base + i),
                                            (int)(j - i), buf.data() + i);
                            }
                            i = j;
                        } else {
                            ++i;
                        }
                    }
                }
            }
            // O pedido e a resposta do ultimo IPC.
            for (int k = 0; k < 2; ++k) {
                const std::uint64_t end = k == 0 ? g_ultimo_req : g_ultimo_resposta;
                if (end == 0)
                    continue;
                std::printf("[msg ] %s em 0x%llx:\n", k == 0 ? "pedido" : "resposta",
                            (unsigned long long)end);
                std::uint8_t buf[128]{};
                std::string e3;
                if (!g_mem->read(end, buf, sizeof buf, e3)) {
                    std::printf("      (leitura falhou: %s)\n", e3.c_str());
                    continue;
                }
                for (std::size_t i = 0; i < sizeof buf; i += 16) {
                    std::printf("      ");
                    for (std::size_t j = 0; j < 16; ++j)
                        std::printf("%02x ", buf[i + j]);
                    std::printf(" |");
                    for (std::size_t j = 0; j < 16; ++j) {
                        const char c = (char)buf[i + j];
                        std::printf("%c", (c >= ' ' && c <= '~') ? c : '.');
                    }
                    std::printf("|\n");
                }
            }
        }
    }
    (void)user;
    // Confirma que o observer corre DENTRO do handler. Sem isto, e sem o
    // relatório (que só imprime quando o guest sai), um SVC despachado sem
    // guest parado não deixa rasto nenhum.
    // Dump da stack: fica para o dump textual em main(), onde o
    // GuestProgram está acessível. Aqui só se guarda o sp.
    if (n_svc <= 1) {
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

    if (n_svc <= 4 && number == 0x06) {
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

    // **O rasto de palavras** — o mutex 0x10a5096d8 e a sua cv-key
    // 0x10a5096e4 muda de dono ENTRE os SVCs. O valor por linha é a
    // descendência exata do `0x20000` do runCAS0/1: a linha onde ele
    // aparece pela primeira vez é o serviço que o deixou passar.
    static const bool rasto_palavras =
        std::getenv("IA_WORD_TRACE") != nullptr;
    if (rasto_palavras && g_mem != nullptr) {
        std::uint32_t dw = 0, e0w = 0, e4w = 0;
        if (g_mem->read(0x10a5096d8, &dw, sizeof dw, g_scratch) &&
            g_mem->read(0x10a5096e0, &e0w, sizeof e0w, g_scratch) &&
            g_mem->read(0x10a5096e4, &e4w, sizeof e4w, g_scratch)) {
            char wb[96];
            const int wk =
                std::snprintf(wb, sizeof wb,
                              "[mwr ] d8=0x%08x e0=0x%08x e4=0x%08x\n",
                              dw, e0w, e4w);
            ssize_t wq = ::write(2, wb, (size_t)wk);
            (void)wq;
        }
    }

    // gpr[] e não x[]: x[] tem 6 entradas e x[18] seria memória a mais.
    char b[320];
    const int k = std::snprintf(b, sizeof b,
        "[svc #%u] 0x%02x %-18s x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx "
        "x4=0x%llx x5=0x%llx x18=0x%llx x30=0x%llx\n",
        n_svc, number, c.name,
        (unsigned long long)ctx.x[0], (unsigned long long)ctx.x[1],
        (unsigned long long)ctx.x[2],
        (unsigned long long)ctx.gpr[3], (unsigned long long)ctx.gpr[4],
        (unsigned long long)ctx.gpr[5],
        (unsigned long long)ctx.gpr[18], (unsigned long long)ctx.gpr[30]);
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

    // A telemetria e' a **global** do motor (ver `telemetry.hpp`): e' a
    // mesma que o `nvdrv` usa para medir a traducao Maxwell -> Vulkan.
    g610::tel::Telemetry& tel = g610::tel::Telemetria();
    tel.set_live_console(false);
    tel.start("0000000000000000", "boot-trace");

    // O **diagnóstico final**: o que há na native window? Se o jogo
    // desenhou, é aqui que se vê. É a pergunta que decide se o caminho
    // da GPU é o caminho do desenho.
    // ── 1. carregar e mapear ──────────────────────────────────────────────
    g610::GuestProgram prog(tel);
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

    // ── onde o rtld le o handle ────────────────────────────────────────────
    //
    // `IA_FIND_TLS=0x110` procura, no `.text` do `rtld`, os pares
    // `mrs xN, tpidr_el0` seguidos de um `ldr` com o offset pedido.
    //
    // É a pergunta que decide o resto: o rtld é o que constrói os parâmetros
    // de arranque que o crt0 do SDK consome, e o único canal que o kernel
    // lhe dá é a ThreadLocalRegion.
    if (const char* tls_alvo = std::getenv("IA_FIND_TLS")) {
        const auto want = std::strtoull(tls_alvo, nullptr, 0);
        std::printf("\n[varrimento] procuram-se mrs tpidr_el0 + ldr #%#llx\n",
                    static_cast<unsigned long long>(want));
        for (const auto& module : prog.modules()) {
            const auto& img = module.nso.image;
            const std::size_t ini = module.nso.seg_offset[0];
            const std::size_t tam = static_cast<std::size_t>(module.nso.segments[0].size);
            if (tam == 0 || ini + tam > img.size())
                continue;
            for (std::size_t i = 0; i + 8 <= tam; i += 4) {
                std::uint32_t w;
                std::memcpy(&w, img.data() + ini + i, 4);
                // MRS Xt, TPIDR_EL0 = 0xD53BD040 | Rt
                if ((w & 0xFFFFE0FFu) != 0xD53BD040u)
                    continue;
                // Procura um LDR (32 ou 64 bits) com o offset pedido nas 6
                // instrucoes seguintes.
                for (std::size_t k = 4; k <= 24 && i + k + 4 <= tam; k += 4) {
                    std::uint32_t w2;
                    std::memcpy(&w2, img.data() + ini + i + k, 4);
                    std::size_t off = 0;
                    bool casa = false;
                    if ((w2 & 0xFFC00000u) == 0xB9400000u) {
                        off = static_cast<std::size_t>(((w2 >> 10) & 0xFFF) * 4);
                        casa = true;
                    } else if ((w2 & 0xFFC00000u) == 0xF9400000u) {
                        off = static_cast<std::size_t>((w2 >> 10) & 0xFFF);
                        casa = true;
                    }
                    if (!casa || off != want)
                        continue;
                    const std::uint64_t pc = module.vaddr_base +
                                             module.nso.segments[0].vaddr + i;
                    std::printf("[varrimento] %s: mrs tpidr_el0 em 0x%llx, ldr #%#llx em +%zu\n",
                                module.name(), static_cast<unsigned long long>(pc),
                                static_cast<unsigned long long>(want), k);
                }
            }
        }
    }

    // ── qualquer ldr com um offset ─────────────────────────────────────────
    //
    // `IA_FIND_LDR=0x110` lista todos os `ldr` com esse offset em cada
    // modulo. E o complemento do scan anterior, que exigia um `mrs tpidr_el0`
    // imediatamente antes — pressuposicao que o codigo pode nao cumprir (o
    // registo pode vir de outro sitio, ou o offset pode estar num `ldp`).
    if (const char* ldr_alvo = std::getenv("IA_FIND_LDR")) {
        const auto want = std::strtoull(ldr_alvo, nullptr, 0);
        std::printf("\n[varrimento] procuram-se ldr #%#llx\n",
                    static_cast<unsigned long long>(want));
        for (const auto& module : prog.modules()) {
            const auto& img = module.nso.image;
            const std::size_t ini = module.nso.seg_offset[0];
            const std::size_t tam = static_cast<std::size_t>(module.nso.segments[0].size);
            if (tam == 0 || ini + tam > img.size())
                continue;
            int n = 0;
            for (std::size_t i = 0; i + 4 <= tam && n < 12; i += 4) {
                std::uint32_t w;
                std::memcpy(&w, img.data() + ini + i, 4);
                std::size_t off = 0;
                bool casa = false;
                if ((w & 0xFFC00000u) == 0xB9400000u) {
                    off = static_cast<std::size_t>(((w >> 10) & 0xFFF) * 4);
                    casa = true;
                } else if ((w & 0xFFC00000u) == 0xF9400000u) {
                    off = static_cast<std::size_t>((w >> 10) & 0xFFF);
                    casa = true;
                }
                if (!casa || off != want)
                    continue;
                const std::uint64_t pc = module.vaddr_base +
                                         module.nso.segments[0].vaddr + i;
                std::printf("[varrimento] %s: ldr #%#llx em 0x%llx\n", module.name(),
                            static_cast<unsigned long long>(want),
                            static_cast<unsigned long long>(pc));
                ++n;
            }
        }
    }

    // ── despejar o .text de um modulo ──────────────────────────────────────
    //
    // `IA_DUMP_TEXT=rtld` escreve o `.text` do modulo em `/tmp/g610_<nome>.text`,
    // para desassemblar com o objdump do host. É a forma de trabalhar com
    // 15 KB de código em vez de 64 bytes de cada vez.
    if (const char* dump_alvo = std::getenv("IA_DUMP_TEXT")) {
        for (const auto& module : prog.modules()) {
            if (dump_alvo[0] != 0 && std::strcmp(dump_alvo, module.name()) != 0)
                continue;
            const auto& img = module.nso.image;
            const std::size_t ini = module.nso.seg_offset[0];
            const std::size_t tam = static_cast<std::size_t>(module.nso.segments[0].size);
            if (tam == 0 || ini + tam > img.size())
                continue;
            char caminho[128];
            std::snprintf(caminho, sizeof caminho, "%s/g610_%s.text", dump_dir(), module.name());
            std::FILE* f = std::fopen(caminho, "wb");
            if (f == nullptr)
                continue;
            std::fwrite(img.data() + ini, 1, tam, f);
            std::fclose(f);
            std::printf("[dump] %s: %zu bytes em %s (base 0x%llx)\n", module.name(), tam,
                        caminho, static_cast<unsigned long long>(module.vaddr_base +
                                                             module.nso.segments[0].vaddr));
        }
    }

    // ── o modulo `main`, em binario ─────────────────────────────────────────
    //
    // `IA_DUMP_MAIN` escreve o `.text` do modulo `main` em
    // `/tmp/g610_main.text`. E' o **proprio jogo**, e a unica forma de ver o
    // que ele chama sem um debugger.
    if (const char* dump_main = std::getenv("IA_DUMP_MAIN")) {
        for (const auto& module : prog.modules()) {
            if (std::strcmp(module.name(), "main") != 0)
                continue;
            const auto& img = module.nso.image;
            const std::size_t ini = module.nso.seg_offset[0];
            const std::size_t tam = static_cast<std::size_t>(module.nso.segments[0].size);
            if (tam == 0 || ini + tam > img.size())
                continue;
            std::FILE* f = std::fopen("/tmp/g610_main.text", "wb");
            if (f == nullptr)
                continue;
            std::fwrite(img.data() + ini, 1, tam, f);
            std::fclose(f);
            std::printf("[dump] main: %zu bytes em /tmp/g610_main.text (base 0x%llx)\n", tam,
                        static_cast<unsigned long long>(module.vaddr_base +
                                                     module.nso.segments[0].vaddr));
        }
    }

    // ── despejar a imagem completa de um modulo ────────────────────────────
    //
    // `IA_DUMP_MODULE=sdk` escreve a imagem inteira do modulo (cabeçalho
    // NSO, segmentos e tudo) em `/tmp/g610_<nome>.nso`, para se poder analysar
    // a tabela de simbolos dinamicos sem um debugger.
    if (const char* dump_modulo = std::getenv("IA_DUMP_MODULE")) {
        for (const auto& module : prog.modules()) {
            if (dump_modulo[0] != 0 && std::strcmp(dump_modulo, module.name()) != 0 &&
                std::strcmp(dump_modulo, "todos") != 0)
                continue;
            const auto& img = module.nso.image;
            char caminho[128];
            // **Onde os dumps vao.** `G610_DUMP_DIR` aponta para um
            // directorio de trabalho — por exemplo um disco externo, quando
            // o `/tmp` do sistema nao tem espaco. Um dump do `main` sao
            // 40 MB, e com `IA_DUMP_MODULE=todos` sao tres.
            std::snprintf(caminho, sizeof caminho, "%s/g610_%s.nso", dump_dir(), module.name());
            std::FILE* f = std::fopen(caminho, "wb");
            if (f == nullptr)
                continue;
            std::fwrite(img.data(), 1, img.size(), f);
            std::fclose(f);
            std::printf("[dump] %s: %zu bytes em %s (base 0x%llx)\n", module.name(), img.size(),
                        caminho,
                        static_cast<unsigned long long>(module.vaddr_base));

            // Um ficheiro ao lado com a geometria: sem os offsets dos
            // segmentos e da dynsym, a imagem dumpada nao se pode analysar.
            char geom[128];
            std::snprintf(geom, sizeof geom, "%s/g610_%s.geom", dump_dir(), module.name());
            std::FILE* g = std::fopen(geom, "w");
            if (g != nullptr) {
                std::fprintf(g, "base 0x%llx\n", static_cast<unsigned long long>(module.vaddr_base));
                for (int k = 0; k < 3; ++k) {
                    std::fprintf(g, "seg %d vaddr 0x%llx size 0x%llx offset %zu\n", k,
                                 static_cast<unsigned long long>(module.nso.segments[k].vaddr),
                                 static_cast<unsigned long long>(module.nso.segments[k].size),
                                 static_cast<std::size_t>(module.nso.seg_offset[k]));
                }
                const auto* h = module.nso.nso_header();
                if (h != nullptr) {
                    std::fprintf(g, "dynsym %u %u\ndynstr %u %u\n", h->dynsym_offset,
                                 h->dynsym_size, h->dynstr_offset, h->dynstr_size);
                }
                std::fclose(g);
                std::printf("[dump] %s: geometria em %s\n", module.name(), geom);
            }
        }
    }

    // ── quem escreve num endereco concreto ──────────────────────────────────
    //
    // `IA_FIND_STORE=0x836a1038` procura, no `.text` de cada módulo, o par
    // ADRP+ADD que forma o endereço pedido, e imprime o código à volta.
    //
    // É a alternativa a desassemblar PLTs e a ler entry points: o par
    // ADRP/ADD é como o AArch64 forma um endereço de 32 bits, e o par é
    // reconhecível numa passage.
    if (const char* store_alvo = std::getenv("IA_FIND_STORE")) {
        const auto alvo = std::strtoull(store_alvo, nullptr, 0);
        const std::uint64_t pagina = alvo & ~0xFFFull;
        const std::uint32_t off = static_cast<std::uint32_t>(alvo & 0xFFF);
        std::printf("\n[varrimento] procuram-se ADRP+ADD para %#llx\n",
                    static_cast<unsigned long long>(alvo));
        for (const auto& module : prog.modules()) {
            const auto& img = module.nso.image;
            const std::size_t ini = module.nso.seg_offset[0];
            const std::size_t tam = static_cast<std::size_t>(module.nso.segments[0].size);
            if (tam == 0 || ini + tam > img.size())
                continue;
            for (std::size_t i = 0; i + 8 <= tam; i += 4) {
                std::uint32_t w;
                std::memcpy(&w, img.data() + ini + i, 4);
                // ADRP: 1 immlo 10000 immhi Rd
                if ((w & 0x9F000000u) != 0x90000000u)
                    continue;
                const std::uint64_t pc = module.vaddr_base +
                                         module.nso.segments[0].vaddr + i;
                const std::int64_t imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2);
                const std::uint64_t pg = (pc & ~0xFFFull) + (static_cast<std::uint64_t>(imm) << 12);
                if (pg != pagina)
                    continue;
                // ADD (immediate): 1 0 0 100010 shift imm12 Rn Rd
                std::uint32_t w2;
                std::memcpy(&w2, img.data() + ini + i + 4, 4);
                if ((w2 & 0xFF800000u) != 0x91000000u)
                    continue;
                if ((w2 >> 10) & 0xFFF != off)
                    continue;
                std::printf("[varrimento] %s: ADRP+ADD para %#llx em 0x%llx\n",
                            module.name(), static_cast<unsigned long long>(alvo),
                            static_cast<unsigned long long>(pc));
            }
        }
    }

    // ── 2. despachante ligado ao espaço do guest ──────────────────────────
    g_mem = &prog.memory();
    g_prog = &prog;

    // O entry de cada modulo, lido do cabecalho `MOD0` que abre o segmento de
    // texto. O `LoadedModule::entry_point` e' a BASE do texto, nao o entry —
    // e essa distincao custou uma volta.
    for (const auto& m : prog.modules()) {
        // O `MOD0` abre o segmento de texto, e e' a primeira coisa da imagem
        // do modulo (o texto esta no vaddr 0). Le-se da IMAGEM, que esta no
        // host, e nao da memoria do guest — assim nao se depende de onde o
        // modulo foi colocado.
        const auto& img = m.nso.image;
        if (img.size() < 8 || std::strncmp(reinterpret_cast<const char*>(img.data()), "MOD0", 4) != 0) {
            std::printf("  entry do %-5s: (sem MOD0)\n", m.name());
            continue;
        }
        std::uint32_t mod0 = 0;
        std::memcpy(&mod0, img.data() + 4, sizeof mod0);
        const std::uint64_t texto = m.vaddr_base + m.nso.segments[0].vaddr;
        std::printf("  entry do %-5s: 0x%llx  (MOD0 em 0x%llx)\n", m.name(),
                    static_cast<unsigned long long>(texto + mod0),
                    static_cast<unsigned long long>(texto));
    }

    g610::SvcDispatcher svc(tel);
    g_svc = &svc;
    svc.bind_memory(&prog.memory());
    svc.set_heap_base(g610::layout::kHeapBase);
    svc.set_heap_size(g610::layout::kHeapSize);
    svc.set_alias_region(g610::layout::kAliasBase, g610::layout::kAliasSize);
    // A memoria do guest: o **tamanho** da reserva, e nao o fim dela.
    //
    // O Eden devolve o *tamanho* (`GetTotalNonSystemUserPhysicalMemorySize`),
    // e o SDK subtrai o usado para saber quanto ainda ha. Com o *fim*
    // da reserva, a subtracao dava um numero sem sentido — e o SDK
    // pediu um heap de 2,4 GB, e o assert de crescimento de vector
    // (`!safe_add(&new_capacity, new_size, (new_size / 2))`) transbordou.
    svc.set_memory_totals(prog.memory().size(),
                          prog.modules().empty()
                              ? 0
                              : prog.modules().front().nso.image.size());
    svc.set_system_resource_size(prog.modules().empty()
                                     ? 0
                                     : prog.modules().front().nso.system_resource_size);
    // A regiao da stack tem de ser a do PROGRAMA: o SDK pergunta o endereco
    // pelo SVC 0x29 (tipo 14) e usa-o. Se o motor responder com um
    // endereco diferente do que o guest esta a usar, a stack do guest fica
    // noutro sitio.
    svc.set_stack_region(prog.stack_base(), g610::layout::kStackSize);
    // A prioridade da thread principal vem do NPDM (`main_thread_priority`).
    if (!prog.modules().empty())
        svc.set_main_thread_priority(prog.modules().front().nso.main_thread_priority);
    svc.set_regions(prog.memory_regions());

    // A região ASLR: onde o código do processo está mapeado.
    //
    // No Eden é `process->GetPageTable().GetAliasCodeRegionStart()` e
    // `GetAliasCodeRegionSize()` (`svc_info.cpp:76-82`), e é o que o SDK da
    // Nintendo usa para instalar o seu heap e o seu estado. Ver a nota em
    // `SvcDispatcher::set_aslr_region`.
    //
    // **A região tem de ser a RESERVA inteira, não o alcance dos módulos.**
    //
    // A primeira versão punha `[base, fim_dos_textos)`. Medido
    // (`boot_p6e.log`): o `nn::os::detail::AddressSpaceAllocator::
    // AllocateSpace` do SDK (que aloca o endereço do mapeamento do
    // `GetSharedMemoryNativeHandle`) escolhe endereços dentro da região,
    // pergunta o `svcQueryMemory` e **exige um bloco `Free`** — e o
    // alcance dos textos está, por construção, **inteiro tapado** pelas
    // doze regiões dos módulos: 1030 sondas, zero blocos livres, e o
    // SDK abortou com `SetTerminateResult: modulo=162 desc=1`
    // (o assert da libnn) depois de percorrer o mapa todo.
    //
    // No Eden, o `KAddressSpaceInfo` (39-bit, `k_address_space_info.cpp`)
    // põe a região de código em `[128 MiB, +512 GiB)` — centenas de GiB —
    // e a maior parte é livre. Aqui a mesma ideia, capada à **reserva**
    // do processo (o que este motor consegue de facto responder: o
    // `svc_map_memory`/`svc_map_shared_memory` só aceitam endereços de
    // lá). O heap (0x9000_0000) e a stack (0x2900_0000_0) vivem no mesmo
    // intervalo e têm as suas entradas de região próprias.
    {
        std::uint64_t base = 0;
        for (const auto& m : prog.modules()) {
            const std::uint64_t ini = m.vaddr_base + m.nso.segments[0].vaddr;
            if (base == 0 || ini < base)
                base = ini;
        }
        const auto& reserva = prog.memory();
        const std::uint64_t fim =
            reserva.base() != 0 ? reserva.base() + reserva.size() : 0x310000000ull;
        if (base != 0 && fim > base)
            svc.set_aslr_region(base, fim - base);
    }

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
            const char* st = r.state == g610::GuestRegion::kCode   ? "Code"
                             : r.state == g610::GuestRegion::kNormal ? "Normal"
                             : r.state == g610::GuestRegion::kStatic ? "Static"
                                                                   : "?";
            const char* pm = r.permission == g610::GuestRegion::kReadExec ? "R-X"
                             : r.permission == g610::GuestRegion::kReadOnly  ? "R--"
                             : r.permission == g610::GuestRegion::kReadWrite ? "RW-"
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
    g610::NativeEngine engine(tel, svc);

    // ── a camada de IPC do Eden ───────────────────────────────────────────
    //
    // A partir daqui, `ConnectToNamedPort` e `SendSyncRequest` deixam de ser
    // nossos: passam a ser o `KObjectName` e o `KClientSession` do Eden, e o
    // `sm:` é o serviço a sério. Ver `vendor/eden/README.md`.
    //
    // **A ORDEM DESTA PARTE IMPORTA**, e foi o que custou a volta final:
    //
    //   1. `attach_eden` monta o sistema do Eden e regista a thread principal
    //      na tabela de handles — é daqui que sai o handle verdadeiro.
    //   2. `set_eden_tls` escreve esse handle em TLS+0x110, que é onde o Eden
    //      o põe (`k_process.cpp:1014`).
    //   3. `set_guest_x1` põe o MESMO handle no x1 do entry point, que é onde
    //      o Eden o põe (`k_process.cpp:1006-1010`).
    //
    // Com o `set_guest_x1` antes do `attach_eden`, o handle era zero — e zero
    // é o valor de um mutex livre, o que fazia o SDK abortar.
    if (svc.attach_eden(1, 1)) {
        std::printf("  IPC do Eden   : ligado (sm: a serio)\n");
    } else {
        std::printf("  IPC do Eden   : NAO ligado -- fica o caminho antigo\n");
    }

    // O bloco de TLS do guest só existe depois de `map_into()`.
    engine.set_guest_tls(g610::layout::kTlsBase);
    svc.set_eden_tls(g610::layout::kTlsBase);

    // O x1 do entry point: o handle da thread principal. Ver `g610_call_guest.S`.
    engine.set_guest_x1(svc.main_thread_handle());
    std::printf("  handle da thread principal: 0x%x\n", svc.main_thread_handle());

    // O bloco de TLS do guest só existe depois de `map_into()`.
    engine.set_guest_tls(g610::layout::kTlsBase);
    // Poucos traps: este tracer é para ver os PRIMEIROS serviços pedidos, e
    // um guest em spin de código não levanta traps nenhum. O limite não o
    // apanha, mas garante que o programa acaba e imprime relatório — em vez
    // de ficar a queimar CPU sem fim com a saída truncada.
    // 40 traps: o rtld faz uma dúzia de serviços no arranque antes de tocar em
    // código do jogo. Três era pouco para ver a sequência.
    // O limite de traps. Subiu de 40 para 4000 porque o SDK **faz polling**:
    // depois de pedir um serviço ao `sm:`, ele repete `GetInfo` e
    // `SendSyncRequest` a espera que o evento de notificacao dispare. Com
    // 40 traps, o motor parava a meio dessa espera — e o sintoma era
    // "o guest esta em ciclo", que é o que o polling **parece** e não é.
    // **O limite de traps.** Medido: o SDK faz polling — 74
    // `SignalProcessWideKey` e 53 `SendSyncRequest` antes de o jogo
    // chegar ao primeiro assert. Com 4000 traps, o motor parava a meio
    // dessa espera.
    //
    // Este valor e' **parametrizavel** (`G610_TRAP_LIMIT`), porque o
    // limite e' uma ferramenta de diagnostico e nao uma propriedade do
    // motor: para medir o desempenho do arranque do jogo, o que interessa
    // e' ate onde o guest chega, e nao onde o motor desiste.
    {
        struct timespec agora {};
        clock_gettime(CLOCK_MONOTONIC, &agora);
        g_inicio_ns = static_cast<long long>(agora.tv_sec) * 1000000000LL + agora.tv_nsec;
    }
    if (const char* limite = std::getenv("G610_TRAP_LIMIT")) {
        engine.set_trap_limit(std::strtoull(limite, nullptr, 0));
    } else {
        engine.set_trap_limit(4000);
    }
    // IA_NOOB=1: o observer é desligado para a medição por subtração — se a
    // heap deixa de corromper sem o observer, o corruptor é o caminho de
    // diagnóstico (printf/string/dump) a correr de dentro do handler em
    // várias threads. Com IA_TRACE=0 os prints do [h]/[run] desaparecem, mas
    // o observer (printf/„string") mantém-se — [svc] sempre printa.
    if (::getenv("IA_NOOB") == nullptr) {
        // O nome por numero de SVC: e' produzido UMA vez aqui, no
        // thread principal, ANTES dos guest existirem. O `to_string`
        // devolve std::string (alocacao!) — no handler, `on_svc`
        // deixou de chama-lo e le' da tabela (medido: 1 malloc+1 free
        // POR SVC no caminho velho).
        for (std::uint32_t n = 0; n < kSvcContagensTam; ++n) {
            std::string nome = g610::to_string(static_cast<g610::Svc>(n));
            std::snprintf(g_nomes[n], kSvcNomeTam, "%s", nome.c_str());
            g_nomes_ptr[n] = g_nomes[n];
        }
        engine.set_observer(&on_svc, nullptr);
        engine.set_trapper(&on_trap, nullptr);
    }
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
        const g610::GuestModule* rtld = nullptr;
        for (const auto& m : prog.modules())
            if (m.kind == g610::GuestModule::kRtld)
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
        const g610::NsoHeader* h = m.nso.nso_header();
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
    const g610::RunResult r =
        engine.run(reinterpret_cast<void (*)()>(entry), sp);

    {
        struct itimerval tv{};
        setitimer(ITIMER_REAL, &tv, nullptr);
    }

    // ── 3a. manter vivo ───────────────────────────────────────────────────
    //
    // Nos runs padrão o run nem passa daqui de forma limpa: o sampler do
    // tracer força a saida aos ~4 s (4.º tick do itimer, `force_stop` —
    // medido LOGE1) e o run devolve com `limit_hit`. a' saída limpa
    // (`ExitProcess` do guest — `r.exited`), este bloco mantém o processo
    // vivo MAIS <seg> segundos (`G610_KEEP_ALIVE=<seg>`): os handlers
    // continuam armados, as threads de guest continuam vivas (a paragem
    // so' e' pedida pelo ~NativeEngine), e por segundo imprime o DELTA do
    // histograma de SVC — o steady-state visto de quem sobra sem o main.
    // Com `IA_STOP_SAMPLES=0` este bloco nunca acorda (o run vive ate' ao
    // timeout do shell); o batimento (`IA_HEARTBEAT=1`) e' quem mede essa
    // janela nesses runs.
    if (const char* env = std::getenv("G610_KEEP_ALIVE")) {
        const long segs = std::strtol(env, nullptr, 10);
        if (segs > 0 && !r.exited) {
            std::printf("[alive] o run NAO saiu pelo ExitProcess (%s) — "
                        "manter vivo saltado\n",
                        r.error.empty() ? "(sem erro)" : r.error.c_str());
        } else if (segs > 0) {
            std::printf("[alive] o guest saiu (code=0x%x, %llu svc) — manter "
                        "o processo vivo %ld s com as threads de guest\n",
                        r.exit_code, (unsigned long long)g_svcs.load(), segs);
            std::uint64_t antes[kSvcContagensTam] = {};
            for (std::size_t n = 0; n < kSvcContagensTam; ++n)
                antes[n] = g_svc_contagens[n].load();
            for (long s = 1; s <= segs; ++s) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::uint64_t delta = 0;
                for (std::size_t n = 0; n < kSvcContagensTam; ++n)
                    delta += g_svc_contagens[n].load() - antes[n];
                std::printf("[alive %02ld/%ld] total=%u (delta +%llu nesta "
                            "segunda)\n",
                            s, segs, (unsigned)g_svcs.load(),
                            (unsigned long long)delta);
                unsigned linhas = 0;
                for (std::size_t n = 0; n < kSvcContagensTam; ++n) {
                    const std::uint64_t v =
                        g_svc_contagens[n].load() - antes[n];
                    antes[n] += v;
                    if (v != 0) {
                        std::printf("    0x%02zx %-24s %llu\n", n,
                                    nome_de_svc(static_cast<std::uint32_t>(n)),
                                    (unsigned long long)v);
                        if (++linhas >= 16) {
                            std::printf("    (mais omitidas)\n");
                            break;
                        }
                    }
                }
            }
            std::printf("[alive] fim; o desligamento segue pelo caminho de "
                        "sempre\n");
        }
    }

    // ── 4. relatório ──────────────────────────────────────────────────────
    // O que há na native window? É a pergunta que decide se o caminho
    // da GPU é o caminho do desenho.
    g610::services::DespejarJanelaDoCompositor();
    g610::services::VarrerRegiaoDoCanalDoCompositor();
    if (g_thread_trace) {
        std::printf("\n[arb] quadro final do congelamento:\n");
        g610_dump_arbiter();
    }
    std::printf("\n══════ resultado ══════\n");
    std::printf("  parou por   : %s\n",
                r.error.empty() ? "(sem erro)" : r.error.c_str());
    std::printf("  última trap : %s\n",
                r.last_trap == g610::TrapKind::GuestFault ? "fault de memória"
                : r.last_trap == g610::TrapKind::SvcCall   ? "SVC"
                : r.last_trap == g610::TrapKind::Breakpoint ? "breakpoint"
                : r.last_trap == g610::TrapKind::Yield     ? "yield"
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
            if (prog.memory().read(g610::layout::kTlsBase, blk, sizeof blk, e2)) {
                std::printf("  bloco TLS em 0x%012llx:\n",
                            (unsigned long long)g610::layout::kTlsBase);
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
    std::printf("  svcs vistas : %u\n", g_svcs.load());
    std::printf("  amostras    : %d  (último PC 0x%llx)\n", g_samples,
                (unsigned long long)g_sample_pc);
    if (!svc.last_ipc().empty())
        std::printf("  ultima IPC  : %s\n", svc.last_ipc().c_str());
    std::printf("  SendSyncReq : %llu\n",
                (unsigned long long)svc.sync_request_calls());
    if (!svc.last_port().empty())
        std::printf("  ultima porta: \"%s\"  (STUB: handle fabricado, sem IPC)\n",
                    svc.last_port().c_str());
    std::printf("  MapMemory   : %llu chamadas\n",
                (unsigned long long)svc.map_memory_calls());

    // Sequência de serviços, na ordem em que foram pedidos. O anel
    // `g_primeiros` guarda os primeiros 120 (teto fixo, sem heap no
    // handler); o cursor e' o total de SVCs.
    std::printf("\n══════ serviços pedidos, por ordem ══════\n");
    {
        const std::uint32_t total = g_primeiros_cursor.load();
        if (total == 0) {
            std::printf("  (nenhum — o rtld não chegou a chamar um serviço)\n");
        } else {
            const std::uint32_t mostrar = total < kSvcOrdenadoTam
                                              ? total
                                              : kSvcOrdenadoTam;
            if (mostrar < total)
                std::printf("  (a mostrar os primeiros %u de %u)\n\n", mostrar,
                            total);
            std::printf("  %-4s %-24s %-18s %s\n", "#", "serviço", "x0", "x1");
            for (std::uint32_t i = 0; i < mostrar; ++i) {
                const SvcCall& c = g_primeiros[i];
                std::printf("  %-4u %-24s 0x%-16llx 0x%-16llx\n", i, c.name,
                            (unsigned long long)c.x[0],
                            (unsigned long long)c.x[1]);
            }
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

    // ── o GOT do `main` foi corrigido? ────────────────────────────────────
    //
    // O `main` tem 821 stubs de PLT no fim do `.text`, e cada um salta
    // atraves de um slot do GOT. No ficheiro, os slots valem `0x17e8f90` — o
    // endereco *relativo ao modulo* do trampolim `PLT0`. Se o guest nao os
    // corrigir, a primeira chamada a uma funcao importada salta para o sitio
    // errado.
    //
    // Esta e' a medicao que diz quem resolve as importacoes: o nosso motor,
    // ou o `rtld` do proprio jogo.
    for (const auto& module : prog.modules()) {
        if (module.kind != GuestKind::kMain)
            continue;

        // Onde esta o GOT? Os stubs de PLT do `main` estao no fim do
        // `.text`, e cada um salta atraves de um slot do GOT. A sequencia e':
        //
        //     adrp x16, <pagina>
        //     ldr  x17, [x16, #off]
        //     add  x16, x16, #off
        //     br   x17
        //
        // O `off` e' o mesmo nos dois (`ldr` escala por 8, `add` nao), o que
        // da a posicao do slot. Os stubs sao contiguos, e por isso procuramos
        // a *maior* serie, e nao o primeiro padrao que apareca (que pode ser
        // uma coincidencia noutro sitio do `.text`).
        const std::vector<std::uint8_t>& img = module.nso.image;
        const std::size_t ini = module.nso.seg_offset[0];
        const std::size_t tam = static_cast<std::size_t>(module.nso.segments[0].size);

        auto is_stub = [&](std::size_t o, std::uint64_t& slot) {
            if (o + 16 > tam)
                return false;
            std::uint32_t adrp = 0, ldr = 0, add = 0, br = 0;
            std::memcpy(&adrp, img.data() + ini + o, 4);
            std::memcpy(&ldr, img.data() + ini + o + 4, 4);
            std::memcpy(&add, img.data() + ini + o + 8, 4);
            std::memcpy(&br, img.data() + ini + o + 12, 4);
            if ((adrp & 0x9F000000u) != 0x90000000u)
                return false;
            if ((ldr & 0xFFC00000u) != 0xF9400000u)
                return false;
            if ((add & 0xFFC00000u) != 0x91000000u)
                return false;
            if (br != 0xD61F0220u)
                return false;

            // A pagina do `adrp` e' relativa ao PC, e o imm21 tem duas
            // metades: `immlo` (bits 30:29) e `immhi` (bits 23:5).
            const std::int64_t immlo = (adrp >> 29) & 3;
            const std::int64_t immhi = (adrp >> 5) & 0x7FFFF;
            std::int64_t imm = immlo | (immhi << 2);
            if (imm & 0x100000)
                imm -= 0x200000;
            const std::uint64_t pc = module.vaddr_base + module.nso.segments[0].vaddr + o;
            const std::uint64_t page =
                (pc & ~static_cast<std::uint64_t>(0xFFF)) +
                static_cast<std::uint64_t>(imm * 4096);
            const std::uint64_t ldr_off =
                static_cast<std::uint64_t>((ldr >> 10) & 0xFFF) * 8;
            const std::uint64_t add_off = (add >> 10) & 0xFFF;
            if (page + ldr_off != page + add_off)
                return false;  // nao e' um stub
            slot = page + ldr_off;
            return true;
        };

        std::size_t melhor_n = 0;
        std::uint64_t got = 0;
        for (std::size_t o = 0; o + 16 <= tam; o += 4) {
            std::uint64_t slot = 0;
            if (!is_stub(o, slot))
                continue;
            std::size_t n = 0;
            std::uint64_t s2 = 0;
            while (is_stub(o + n * 16, s2))
                n++;
            if (n > melhor_n) {
                melhor_n = n;
                got = slot;
            }
        }
        if (!got) {
            std::printf("  GOT do main: nao encontrei stubs de PLT\n");
            break;
        }
        std::printf("  GOT do main: %zu stubs de PLT, primeiro slot em 0x%llx\n",
                    melhor_n, (unsigned long long)got);

        std::uint64_t slots[8] = {};
        std::string e2;
        if (!prog.memory().read(got, slots, sizeof slots, e2)) {
            std::printf("  GOT do main: nao legivel em 0x%llx (%s)\n",
                        (unsigned long long)got, e2.c_str());
            break;
        }
        std::printf("  GOT do main @0x%llx (8 primeiros slots):\n",
                    (unsigned long long)got);
        for (int i = 0; i < 8; i++) {
            std::printf("    [%d] 0x%016llx\n", i, (unsigned long long)slots[i]);
        }

        // Todos os slots, para correlacionar com as chamadas do jogo.
        //
        // Cada stub de PLT salta atraves de um slot do GOT, e o GOT e' a
        // saida do `rtld` do proprio jogo: os slots que ainda valem o
        // endereco do `PLT0` (0x17e8f90) sao os que ainda nao foram
        // chamados; os outros ja tem o endereco da funcao do SDK.
        {
            std::vector<std::uint64_t> todos(melhor_n);
            std::string e3;
            if (g_mem->read(got, todos.data(), todos.size() * 8, e3)) {
                char caminho[64];
                std::snprintf(caminho, sizeof caminho, "/tmp/g610_got_main.txt");
                std::FILE* f = std::fopen(caminho, "w");
                if (f != nullptr) {
                    for (std::size_t i = 0; i < todos.size(); i++)
                        std::fprintf(f, "%zu 0x%llx\n", i,
                                     (unsigned long long)todos[i]);
                    std::fclose(f);
                    std::printf("  GOT do main: %zu slots em %s\n", todos.size(),
                                caminho);
                }
            }
        }
        break;
    }

    // Resumo por serviço: o que o rtld pede mais vezes é o que vale a pena
    // implementar primeiro. Contagem por numero, acumulada no handler
    // por atómicos — sem o laço O(N^2) sobre o vector velho.
    std::printf("\n══════ resumo ══════\n");
    std::uint32_t distintos = 0;
    for (std::uint32_t n = 0; n < kSvcContagensTam; ++n) {
        const std::uint64_t v = g_svc_contagens[n].load();
        if (v > 0) {
            ++distintos;
            std::printf("    0x%02x %-24s %llu vez%s\n", n,
                        g_nomes_ptr[n] != nullptr ? g_nomes_ptr[n] : "Svc?",
                        (unsigned long long)v, v == 1 ? "" : "es");
        }
    }
    std::printf("  serviços distintos: %u\n", distintos);

    trace("\n[boot] fim\n");
    return 0;
}