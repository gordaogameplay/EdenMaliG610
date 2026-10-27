// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação do despachante de SVC.
#include "g610/svc_dispatcher.hpp"

#include "g610/eden_bridge.hpp"
#include "g610/native_engine.hpp"
#include "g610/services/shared_font.hpp"

#include "vendor/eden/core/hle/kernel/k_handle_table.h"
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"
#include "vendor/eden/core/hle/kernel/k_readable_event.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <sched.h>
#include <thread>
#include <vector>

namespace g610 {

// A pagina do watchpoint — o motor usa-a para distinguir "o guest
// escreveu aqui" de "o guest leu fora do mapa".
volatile std::uint64_t g_watch_pagina = 0;

// **O watchpoint ainda nao foi armado** — porque o rtld
// escreve na pagina (as relocacoes).
volatile bool g_esperar_pelo_rtld = false;

// O endereco a vigiar.
std::uint64_t g_endereco_do_watchpoint = 0;

// ── Troca coopérativa de threads (definido em native_engine.cpp) ────────
extern ucontext_t* g_dispatch_uc;
extern std::atomic<std::uint64_t> g_switch_to_tls;
extern std::unordered_map<std::uint64_t, std::uint64_t> g_cv_waiter_thread;
extern GuestThreadState* find_ready_guest_thread(std::uint64_t current_tls);
extern void mark_ready_threads_on_cv(std::uint64_t cv_key);
extern std::uint64_t read_thread_pointer();
extern GuestThreadState* find_guest_thread_state(std::uint64_t tp);

// Mapeia o token de espera (dispatch_owner_itself) para o GuestThreadState,
// para que SignalProcessWideKey saiba que thread despertar.
extern std::unordered_map<std::uint64_t, GuestThreadState*> g_token_to_thread;

// Mapeia cv_key para a thread de guest que esta a espera nessa cv.
extern std::unordered_map<std::uint64_t, std::uint64_t> g_cv_waiter_thread;

// **Ler o global do jogo** — o handler de sinais e' estatico
// e nao alcanca o dispatcher.
bool LerGlobalDoJogo(std::uint64_t* valor) {
    if (g_endereco_do_watchpoint == 0)
        return false;
    // A memoria do guest e' do proprio processo (o `mmap` com
    // `MAP_FIXED_NOREPLACE`) — e o handler e' estatico.
    std::memcpy(valor, reinterpret_cast<const void*>(g_endereco_do_watchpoint),
                sizeof(std::uint64_t));
    return true;
}

// **Repor a pagina do watchpoint** — o handler de sinais e' estatico
//, e por isso nao alcanca o dispatcher. O `mprotect` e' feito
// aqui mesmo — a pagina e' do guest e o motor sabe onde ela esta'.

// **Voltar a armar depois da escrita** — o `mprotect` para so' leitura
// trava a escrita; o guest repete a instrucao e a pagina tem de
// ficar gravavel. Re-armar no proximo trap e' o que apanha a
// escrita **seguinte**.
volatile bool g_rearmar_watchpoint = false;
void ReporWatchpoint() {
    if (g_watch_pagina == 0)
        return;
    ::mprotect(reinterpret_cast<void*>(g_watch_pagina), 0x1000,
               PROT_READ | PROT_WRITE);
}

// **Voltar a armar** — a pagina fica em so' leitura, para a
// escrita seguinte ser apanhada.
void ReArmarWatchpoint() {
    if (g_watch_pagina == 0)
        return;
    ::mprotect(reinterpret_cast<void*>(g_watch_pagina), 0x1000, PROT_READ);
}

// **Armar no proximo trap** — e' aqui que a escrita ja' passou.
void ArmarWatchpointNoProximoTrap() {
    if (g_watch_pagina != 0)
        g_rearmar_watchpoint = true;
}

namespace {
std::uint64_t host_tick_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// O guest valida strings pela zona terminador. Esta leitura é defensiva para
// não rebentar o processo do host com um apontador do guest sem NUL.
//
// Nota: `addr` é um endereço do espaço de 39 bits do guest. Numa configuração
// real isto tem de passar por um tradutor de memória do guest em vez de um
// dereference directo — ver a nota sobre GuestMemory em nso_loader.hpp. Está
// assim para manter este ficheiro legível; o tradutor entra em fase posterior.
} // namespace

bool SvcDispatcher::read_guest(std::uint64_t addr, void* dst, std::size_t size) const {
    if (mem_ != nullptr)
        return mem_->read(addr, dst, size, g_err_scratch);
    // Sem espaço ligado: não se pode garantir que o endereço é válido. Um
    // dereference directo aqui seria um SIGSEGV dentro de um handler de
    // sinais, que é o pior sítio possível para um crash.
    return false;
}

// **O watchpoint** — ver a nota em `svc_dispatcher.hpp`.
//
// A pagina fica em so' leitura; a escrita do guest nela levanta um
// SIGSEGV, que o motor reconhece (o endereco esta' dentro do espaco
// do guest) e reporta.
// **Armar o watchpoint depois do rtld**.
void SvcDispatcher::ArmarWatchpointSePedir(std::uint64_t endereco, std::uint64_t tamanho) {
    g_endereco_do_watchpoint = endereco;
    g_esperar_pelo_rtld = true;
}

void SvcDispatcher::ArmarWatchpoint(std::uint64_t endereco, std::uint64_t tamanho) {
    if (mem_ == nullptr || tamanho == 0)
        return;
    // A pagina que contem o endereco.
    const std::uint64_t pagina = endereco & ~static_cast<std::uint64_t>(0xFFF);
    std::string erro;
    if (!mem_->protect(pagina, 0x1000, Prot::Read, erro)) {
        std::fprintf(stderr, "[watch] mprotect falhou: %s\n", erro.c_str());
        return;
    }
    g_watch_pagina = pagina;
    std::fprintf(stderr, "[watch] pagina 0x%llx em so' leitura\n",
                 (unsigned long long)pagina);
}

bool SvcDispatcher::write_guest(std::uint64_t addr, const void* src,
                                std::size_t size) {
    if (mem_ == nullptr || size == 0)
        return false;
    return mem_->write(addr, src, size, g_err_scratch);
}

bool SvcDispatcher::cas_word_guest(std::uint64_t addr, std::uint32_t esperado,
                                   std::uint32_t novo) {
    // **Rasto de palavras**: cada troca na memória do guest fica nos
    // registros — o `0x00020000` da word do mutex 0xd8 (runCAS0/1-5)
    // não impresso por NINGUÉM, então o estado só se vê por aqui.
    static const bool rasto = std::getenv("IA_WORD_TRACE") != nullptr;
    static std::size_t cas_n = 0;
    //
    // **PADRÃO CEGO** — o valor (`novo`) entra INCONDICIONALMENTE na
    // memória, como o `write_guest` sempre fez. Medido (runBLIND vs
    // runCAS0-5, 25/10): com o CAS ativo o runs ESTANCAM sempre no
    // svc #402-407 com o word do mutex 0x10a5096d8 órfão (`0x00020000`
    // e depois `0x40020000` — dono-fantasma que nenhuma fila conhece),
    // escrito por código do próprio guest entre a nossa liberação do
    // parque e o próximo SVC. Com a escrita cega o fluxo corre sã:
    // svc #933+, DLOCK/ACORDEI ciclando, a família de palavras sã
    // (0x0/0x00020003/0x000081ff/0x400081ff/0x40020003). O permitido
    // de escrita cega é matar é o estado medido mais estávo — o braço
    // CAS fica atrás de `IA_CAS=1` para a experiência futura.
    static const bool cas_ativo = std::getenv("IA_CAS") != nullptr;
    if (!cas_ativo) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const bool ok = write_guest(addr, &novo, sizeof novo);
        if (rasto)
            std::fprintf(stderr,
                         "[word] BLIND n=%zu addr=0x%llx esp=0x%x novo=0x%x %s\n",
                         cas_n, static_cast<unsigned long long>(addr), esperado,
                         novo, ok ? "ok" : "falhou");
        ++cas_n;
        if (!ok)
            return false;
        return true;
    }
    if (mem_ == nullptr)
        return false;
    std::uint8_t* p = mem_->host_pointer(addr, sizeof esperado);
    if (p == nullptr)
        return false;
    std::uint32_t e = esperado;  // mutável: em falha o CAS escreve o atual
    const bool ok = __atomic_compare_exchange_n(p, &e, novo, false,
                                                __ATOMIC_SEQ_CST,
                                                __ATOMIC_SEQ_CST);
    if (rasto && (cas_n < 6144 || cas_n % 1000 == 0))
        std::fprintf(stderr,
                     "[word] CAS n=%zu addr=0x%llx esp=0x%x novo=0x%x %s\n",
                     cas_n, static_cast<unsigned long long>(addr), esperado,
                     novo, ok ? "ok" : (e == esperado ? "retry" : "erased"));
    ++cas_n;
    return ok;
}

bool SvcDispatcher::escrita_atomica_word(std::uint64_t addr,
                                         std::uint32_t novo) {
    // Com o braço CAS desligado (o padrão, ver `cas_word_guest`) isto é
    // EXACTAMENTE a escrita cega anterior: fence seq_cst + `write_guest`.
    // Com `IA_CAS=1` volta o ciclo comparar-e-trocar com re-leituras.
    static const bool cas_ativo = std::getenv("IA_CAS") != nullptr;
    if (!cas_ativo)
        return cas_word_guest(addr, 0, novo) ? true : false;
    std::uint32_t observado = 0;
    if (!read_guest(addr, &observado, sizeof observado))
        return false;
    for (int tenta = 0; tenta < 16; ++tenta) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (cas_word_guest(addr, observado, novo))
            return true;
        // A palavra mudou no meio (o guest escreveu): re-ler e trocar de
        // novo — o `novo` é do quadro, não da leitura repetida.
        if (!read_guest(addr, &observado, sizeof observado))
            return false;
    }
    // Esgotadas as trocas (um circuito rival azarado a longo): a escrita
    // cega é o comportamento de reserva — mantém liveness.
    static std::size_t fall_n = 0;
    if (std::getenv("IA_WORD_TRACE") != nullptr &&
        (fall_n < 512 || fall_n % 100 == 0))
        std::fprintf(stderr,
                     "[word] FALLBACK n=%zu addr=0x%llx velho=0x%x novo=0x%x\n",
                     fall_n, static_cast<unsigned long long>(addr), observado,
                     novo);
    ++fall_n;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return write_guest(addr, &novo, sizeof novo);
}

namespace {

bool safe_copy_string(SvcDispatcher& d, std::uint64_t guest_addr, std::string& out,
                      std::size_t max) {
    if (guest_addr == 0)
        return false;
    out.assign(max, '\0');
    if (!d.read_guest(guest_addr, out.data(), max))
        return false;
    // O comprimento vem do guest e é exactamente o número de bytes a ler. O
    // terminador não precisa de estar dentro desse intervalo: o driver do
    // Switch passa o comprimento, não um strlen.
    return true;
}

// Copia uma string terminada em NUL do espaço do guest.
//
// Para as SVCs cuja assinatura é só um ponteiro — `ConnectToNamedPort` não
// leva comprimento (Eden, svc.cpp:1470). Ler `max` bytes e parar no primeiro
// zero é o que o kernel faz.
bool safe_copy_cstring(SvcDispatcher& d, std::uint64_t guest_addr,
                       std::string& out, std::size_t max) {
    if (guest_addr == 0)
        return false;
    out.clear();
    for (std::size_t i = 0; i < max; ++i) {
        char c = 0;
        if (!d.read_guest(guest_addr + i, &c, 1))
            return false;
        if (c == '\0')
            return true;
        out.push_back(c);
    }
    return false;   // sem terminador dentro do limite
}
} // namespace

std::string to_string(Svc s) {
    switch (s) {
    case Svc::SetHeapSize:             return "SetHeapSize";
    case Svc::SetMemoryPermission:     return "SetMemoryPermission";
    case Svc::SetMemoryAttribute:      return "SetMemoryAttribute";
    case Svc::MapMemory:               return "MapMemory";
    case Svc::UnmapMemory:             return "UnmapMemory";
    case Svc::QueryMemory:             return "QueryMemory";
    case Svc::ExitProcess:             return "ExitProcess";
    case Svc::ExitThread:              return "ExitThread";
    case Svc::CreateThread:            return "CreateThread";
    case Svc::StartThread:             return "StartThread";
    case Svc::SleepThread:             return "SleepThread";
    case Svc::GetThreadPriority:       return "GetThreadPriority";
    case Svc::SetThreadPriority:       return "SetThreadPriority";
    case Svc::GetThreadCoreMask:       return "GetThreadCoreMask";
    case Svc::SetThreadCoreMask:       return "SetThreadCoreMask";
    case Svc::GetCurrentProcessorNumber: return "GetCurrentProcessorNumber";
    case Svc::SignalEvent:             return "SignalEvent";
    case Svc::ClearEvent:              return "ClearEvent";
    case Svc::MapSharedMemory:         return "MapSharedMemory";
    case Svc::UnmapSharedMemory:       return "UnmapSharedMemory";
    case Svc::CreateTransferMemory:    return "CreateTransferMemory";
    case Svc::CloseHandle:             return "CloseHandle";
    case Svc::GetSystemTick:           return "GetSystemTick";
    case Svc::ConnectToNamedPort:      return "ConnectToNamedPort";
    case Svc::SendSyncRequestLight:    return "SendSyncRequestLight";
    case Svc::SendSyncRequest:         return "SendSyncRequest";
    case Svc::GetProcessId:            return "GetProcessId";
    case Svc::GetThreadId:             return "GetThreadId";
    case Svc::SignalProcessWideKey: return "SignalProcessWideKey";
    case Svc::Break:                   return "Break";
    case Svc::OutputDebugString:       return "OutputDebugString";
    case Svc::GetInfo:                 return "GetInfo";
    case Svc::FlushEntireDataCache:    return "FlushEntireDataCache";
    case Svc::FlushDataCache:          return "FlushDataCache";
    case Svc::MapPhysicalMemory:       return "MapPhysicalMemory";
    case Svc::UnmapPhysicalMemory:     return "UnmapPhysicalMemory";
    case Svc::ArbitrateLock:           return "ArbitrateLock";
    case Svc::ArbitrateUnlock:         return "ArbitrateUnlock";
    case Svc::WaitProcessWideKeyAtomic: return "WaitProcessWideKeyAtomic";
    case Svc::WaitForAddress:          return "WaitForAddress";
    case Svc::SignalToAddress:         return "SignalToAddress";
    }
    return "Svc#" + std::to_string(static_cast<std::uint32_t>(s));
}

std::string to_string(Result r) {
    switch (r) {
    case Result::Success:           return "Success";
    case Result::InvalidCurrentMemory: return "InvalidCurrentMemory";
    case Result::InvalidCombination: return "InvalidCombination";
    case Result::InvalidState:      return "InvalidState";
    case Result::InvalidPointer:    return "InvalidPointer";
    case Result::NotImplemented:    return "NotImplemented";
    case Result::OutOfRange:        return "OutOfRange";
    case Result::LimitReached:      return "LimitReached";
    case Result::InvalidHandle:     return "InvalidHandle";
    case Result::OutOfMemory:       return "OutOfMemory";
    case Result::NotFound:          return "NotFound";
    case Result::InvalidAddress:    return "InvalidAddress";
    case Result::OutOfResource:     return "OutOfResource";
    case Result::Unsupported:       return "Unsupported";
    case Result::InvalidCoreId:    return "InvalidCoreId";
    case Result::TimedOut:         return "TimedOut";
    default:                        return "Result#" + std::to_string((std::uint32_t)r);
    }
}

// ---------------------------------------------------------------------------

// `ConnectToNamedPort` cria uma PORTA e uma SESSÃO, e devolve o handle da
// SESSÃO.
//
// De `svc_port.cpp:19-55` no Eden:
//
//     // Find the client port.
//     auto port = KObjectName::Find<KClientPort>(system.Kernel(), name.data());
//     R_UNLESS(port.IsNotNull(), ResultNotFound);
//
//     // Reserve a handle for the port.
//     R_TRY(handle_table.Reserve(system.Kernel(), out));
//
//     // Create a session.
//     KClientSession* session;
//     R_TRY(port->CreateSession(system.Kernel(), std::addressof(session)));
//
//     // Register the session in the table, close the extra reference.
//     handle_table.Register(system.Kernel(), *out, session);
//
// Duas coisas que a versão anterior não tinha, e as duas importam:
//
//   1. O handle devolvido é o de uma SESSÃO, não o de uma porta. É por isso
//      que o guest vai directo do `ConnectToNamedPort` para o
//      `SendSyncRequest` sem `OpenSession` nenhum — e é por isso que o
//      handle tem de servir de sessão.
//
//   2. São DOIS objectos e DOIS handles. Antes havia um contador que
//      fabricava os dois ao mesmo tempo, e um handle de porta acabava a ser
//      usado como sessão sem ninguém dar por isso.
//
// A tabela é a do Eden: índice de 15 bits e geração de 15 bits
// (`kobject.hpp`, com a fonte de cada campo). Os handles de antes
// (`0x1000`, `0x1001`, ...) tinham `linear_id == 0`, que é INVÁLIDO pela
// regra do Eden — nenhum passaria um `IsValidHandle`.
std::uint32_t SvcDispatcher::connect_named_port(const std::string& name) {
    if (name.empty())
        return 0;

    // A porta. Uma porta por nome, como `KObjectName::Find<KClientPort>`.
    Handle port_h{};
    {
        auto it = port_by_name_.find(name);
        if (it != port_by_name_.end()) {
            port_h = Handle{it->second};
        } else {
            auto* po = new KObject();
            po->kind = KObject::Kind::Port;
            po->name = name;
            const Handle h = handles_.reservar();
            if (!handles_.registar(h, po))
                return 0;
            port_by_name_.emplace(name, h);
            port_h = h;
        }
    }

    // A sessão. Nova a cada `ConnectToNamedPort`, como no Eden.
    auto* so = new KObject();
    so->kind   = KObject::Kind::Session;
    so->name   = name;
    so->parent = port_h;

    const Handle h = handles_.reservar();
    if (!handles_.registar(h, so)) {
        delete so;
        return 0;
    }
    ++vivos_;
    return h.raw;
}

void SvcDispatcher::close_handle(std::uint32_t raw_handle) {
    // `svc_close_handle` — é o `CloseHandle` do Eden
    // (`svc_synchronization.cpp:19-28`): `Remove` na mesa do processo e
    // falha com `ResultInvalidHandle` quando o handle não existe.
    //
    // **A medição que mandou escrever isto** (`/tmp/boot_long.log`): o
    // guest abriu o `pl:u` e nunca conseguiu fechar nada — o fecho antigo
    // consultava um mapa que ninguém preenche, e as sessões de
    // `KClientPort::CreateSession` aglomeraram-se até à porta esgotar e
    // devolver `ResultOutOfSessions` (0x0CE01), com o jogo a abortar no
    // `SetTerminateResult` (modulo 1, desc 103). O fecho em duas tabelas
    // (a da casa para as suas threads/memórias, a do Eden para as sessões
    // de serviço) é o que mantém as portas com sessões livres.

    // Handles com forma inválida (0, pseudo-handles) não têm entrada em
    // nenhuma das duas mesas — o `Remove` do Eden também os rejeita
    // (`k_handle_table.cpp:31`).
    const Handle h{raw_handle};
    if (!h.shape_valido())
        return;

    // 1. A mesa da casa. Threads do `CreateThread` e memórias de
    //    transferência vivem aqui; livrar a entrada e largar a referência
    //    é o paralelo do `FreeEntry` + `obj->Close()` do Eden.
    if (handles_.remover(h)) {
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            std::fprintf(stderr, "[svc] CloseHandle 0x%x: remover da mesa casa\n",
                         raw_handle);
        }
        return;
    }

    // 2. A mesa do Eden. As sessões de serviço (o `GetService` do `sm:`),
    //    os eventos e as memórias partilhadas dos serviços — tudo o que
    //    dois `svc_*` handlers entregaram ao guest por handle. `Remove`
    //    fecha o objecto e, quando a última referência da sessão se vai,
    //    `KClientPort::OnSessionFinalized` decrementa `m_num_sessions` —
    //    a porta volta a aceitar.
    if (eden_ != nullptr) {
        auto& kernel = eden_->Kernel();
        if (Kernel::GetCurrentProcess(kernel).GetHandleTable()
                .Remove(kernel, Kernel::Handle{raw_handle})) {
            if (std::getenv("IA_IPC_TRACE") != nullptr) {
                std::fprintf(stderr,
                             "[svc] CloseHandle 0x%x: remover da mesa Eden\n",
                             raw_handle);
            }
            return;
        }
        // O handle pode existir nas DUAS mesas (valores independentes).
        // Casa primeiro, Eden depois — e se nenhum o conhece, é um fecho
        // de handle inexistente: o Eden devia devolver erro, mas os
        // SDKs chamam sem verificar e o wrapper não escreve x0; ignorar.
    }
}

// ---------------------------------------------------------------------------
// dispatch — o caminho quente.
// ---------------------------------------------------------------------------

void SvcDispatcher::dispatch(std::uint32_t number, SvcContext& ctx) {
    // **O watchpoint** — armado no **primeiro SVC**, que e'
    // depois do rtld (o rtld nao faz SVC).
    if (g_esperar_pelo_rtld) {
        // **O rtld ja' passou?** — o rtld vive em `0x8446xxxx` e
        // o jogo em `0x80xxxxxx`. armar antes disso apanhava as
        // **relocacoes** (que escrevem na mesma pagina) — e a
        // pagina em so' leitura travaria o jogo.
        //
        // Medido: sem este cuidado, as relocacoes do rtld
        // (`str x11, [x12, x8]`, em `0x84461980`) levantavam
        // `SIGSEGV` em loop.
        // **Arma ja'** — o valor estragado aparece **durante o rtld**
        // (medido: no momento em que o watchpoint arma, o
        // global ja' valia `0xcb9000000000808e`).
        //
        // A pagina volta a ser gravavel no handler (o `ReporWatchpoint`
        //), por isso o rtld consegue escrever — e a escrita e'
        // apanhada.
        g_esperar_pelo_rtld = false;
        ArmarWatchpoint(g_endereco_do_watchpoint, 8);
    }

    const tel::Nanos t0 = tel_.time_now();
    tel_.note_svc();
    tel_.exec().svc_calls.fetch_add(1, std::memory_order_relaxed);

    // **Todos os SVCs, em ordem.** Sem isto, "o guest esta' em ciclo" nao
    // diz *onde*: com a lista, diz qual e' o ultimo SVC antes do ciclo —
    // e o nome do SVC que falta e' a pista.
    //
    // **O `sp` diz de que thread o SVC veio.** O `sp` do guest e' a pilha
    // real da thread; as pilhas do guest sao disjuntas, logo o `sp`
    // identifica a thread sem ambiguidade (agrupando por `sp >> 24`).
    // Sem isto, "quem fez o que" era inferencia a partir das linhas do
    // log — e a inferencia deu wrong answers.
    if (std::getenv("IA_SVC_TRACE") != nullptr) {
        std::fprintf(stderr, "[svc] #0x%02x %s sp=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
                     number, to_string(static_cast<Svc>(number)).c_str(),
                     (unsigned long long)ctx.sp,
                     (unsigned long long)ctx.x[0], (unsigned long long)ctx.x[1],
                     (unsigned long long)ctx.x[2], (unsigned long long)ctx.x[3]);
    }

    Result r;
    switch (static_cast<Svc>(number)) {
    case Svc::SetHeapSize:          r = svc_set_heap_size(ctx); break;
    case Svc::ExitProcess:          r = svc_exit_process(ctx);  break;
    case Svc::CloseHandle:          r = svc_close_handle(ctx);  break;
    case Svc::SetMemoryPermission:  r = svc_control_memory(ctx); break;
    case Svc::SetMemoryAttribute:   r = svc_set_memory_attribute(ctx); break;
    case Svc::GetSystemTick:        r = svc_get_system_tick(ctx); break;
    case Svc::GetThreadId:          r = svc_get_thread_id(ctx); break;
    case Svc::GetProcessId:         r = svc_get_process_id(ctx); break;
    case Svc::ConnectToNamedPort:   r = svc_connect_port(ctx);   break;
    case Svc::MapMemory:            r = svc_map_memory(ctx);     break;
    case Svc::UnmapMemory:          r = svc_unmap_memory(ctx);   break;
    case Svc::MapSharedMemory:      r = svc_map_shared_memory(ctx); break;
    case Svc::CreateTransferMemory: r = svc_create_transfer_memory(ctx); break;
    case Svc::UnmapSharedMemory:    r = svc_unmap_memory(ctx);   break;
    case Svc::QueryMemory:          r = svc_query_memory(ctx);   break;
    case Svc::GetInfo:              r = svc_get_info(ctx);       break;
    case Svc::WaitSynchronization:  r = svc_wait_synchronization(ctx); break;
    case Svc::SendSyncRequest:      r = svc_send_sync_request(ctx); break;
    case Svc::SendSyncRequestLight: r = svc_send_sync_request_light(ctx); break;
    case Svc::GetThreadPriority:    r = svc_get_thread_priority(ctx); break;
    case Svc::SetThreadPriority:    r = svc_set_thread_priority(ctx); break;
    case Svc::GetThreadCoreMask:    r = svc_get_thread_core_mask(ctx); break;
    case Svc::SetThreadCoreMask:    r = svc_set_thread_core_mask(ctx); break;
    case Svc::GetCurrentProcessorNumber: r = svc_get_current_processor_number(ctx); break;
    case Svc::CreateThread:         r = svc_create_thread(ctx); break;
    case Svc::StartThread:          r = svc_start_thread(ctx); break;
    case Svc::ArbitrateLock:        r = svc_arbitrate_lock(ctx); break;
    case Svc::ArbitrateUnlock:      r = svc_arbitrate_unlock(ctx); break;
    case Svc::WaitProcessWideKeyAtomic: r = svc_wait_process_wide_key_atomic(ctx); break;
    case Svc::SignalProcessWideKey: r = svc_signal_process_wide_key(ctx); break;
    case Svc::WaitForAddress:       r = svc_wait_for_address(ctx); break;
    case Svc::SignalToAddress:      r = svc_signal_to_address(ctx); break;
    default:                        r = svc_unsupported(ctx);    break;
    }

    // Só escreve o Result se o serviço não tiver devolvido um valor.
    // Ver `SvcContext::has_ret`.
    if (!ctx.has_ret)
        ctx.ret = static_cast<std::uint64_t>(r);

    // **O Result que o guest recebe** — e' o que o SDK verifica depois de
    // cada SVC. Medido: zero SVC devolvem erro durante o arranque (o
    // unico e' o `Break` final), o que mostra que o jogo nao aborta por
    // causa de um Result — aborta por o timeout do watchdog.
    if (std::getenv("IA_SVC_TRACE") != nullptr && r != Result::Success) {
        std::fprintf(stderr, "[svc] devolveu ERRO %u (0x%x)\n",
                     static_cast<unsigned>(r), static_cast<unsigned>(r));
    }

    // **O x0 que o guest recebe** — o SDK le-o.

    // **Um SVC que devolve erro mata o jogo.** O SDK trata um Result
    // inesperado como fatal: chama `SetTerminateResult` ao `am` e aborta.
    // Foi o que aconteceu com o SVC 0x0f (`SetThreadCoreMask`), que nao
    // estava implementado e devolvia `Unsupported` (64 = 0x40).
    //
    // Por isso o Result de cada SVC que falha fica no log: e' a unica
    // pista que diz *qual* chamada matou o jogo.
    if (r != Result::Success && std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] #0x%02x %-24s devolveu ERRO %s (0x%x)\n", number,
                     to_string(static_cast<Svc>(number)).c_str(), to_string(r).c_str(),
                     static_cast<unsigned>(r));
    }

    // A medição é o produto principal do motor: cada SVC entra no histograma
    // com o seu custo, e o resumo aponta o mais caro.
    tel_.services().record(number, tel_.time_now() - t0);
}

Result SvcDispatcher::svc_set_heap_size(SvcContext& ctx) {
    // **A ABI é a do Eden**, e foi medida no SDK.
    //
    // O Eden (`src/core/hle/kernel/svc.cpp:1250-1255`):
    //
    //     static void SvcWrap_SetHeapSize64(Core::System& system, std::span<uint64_t, 8> args) {
    //         uint64_t size = Convert<uint64_t>(GetArg64(args, 1));
    //         uint64_t out_address{};
    //         Result ret = SetHeapSize64(system, std::addressof(out_address), size);
    //         SetArg64(args, 0, Convert<uint64_t>(ret));
    //         SetArg64(args, 1, Convert<uint64_t>(out_address));
    //     }
    //
    // E `svc_physical_memory.cpp:14`:
    //
    //     Result SetHeapSize(Core::System& system, u64* out_address, u64 size) {
    //         ...
    //         *out_address = GetInteger(address);
    //
    // Ou seja: **x0 e' o ponteiro de saida** e **x1 e' o tamanho**. O
    // endereco base do heap volta em **x1** — e tambem e' escrito no
    // ponteiro.
    //
    // **Medido no SDK** (modulo `sdk` desassemblado):
    //
    //   0x82d799ec — a wrapper do SVC 1:
    //       str  x0, [sp, #-16]!   ; guarda o ponteiro de saida
    //       brk  #0x1
    //       ldr  x2, [sp]
    //       str  x1, [x2]          ; escreve x1 no endereco x0
    //
    //   0x82d29ee0 — `nn::os::detail::MemoryHeapManagerImplByHorizon::SetHeapSize`:
    //       mov  x0, x1            ; x0 do SVC = 1.o argumento
    //       mov  x1, x2            ; x1 do SVC = 2.o argumento
    //       bl   0x82d799ec
    //
    // **O sintoma da versao errada tambem esta medido.** Com x0 tratado
    // como o tamanho, o SDK lia x1 (0x7e00000 — o tamanho) como se fosse
    // o endereco base do heap, e escrevia la. Era o fault em 0x82d25ca0,
    // com a escrita a 0x7e00010.
    //
    // Uma versao anterior deste ficheiro inverteu a ordem para
    // (tamanho, endereco) — com a justificacao de que "a fonte da verdade
    // para a ABI do guest e' o guest". O guest diz o contrario, e o
    // Eden concorda com o guest.
    const std::uint64_t out_address = ctx.x[0];
    const std::uint64_t requested   = ctx.x[1];
    if (requested == 0)
        return Result::InvalidSize;

    // Não temos uma política de heap sophisticated: devolvemos o início da
    // reserva que o layout definiu. É o que o Eden faz para NCE (o heap é
    // gerido pelo próprio runtime do guest, não pelo emulador), e é
    // suficiente para o rtld arrancar.
    std::uint64_t base = heap_base_;
    if (base == 0) {
        // Sem layout ligado: ainda assim devolver um endereço dentro dos 39
        // bits é melhor que 0. Um endereço plausível deixa o rtld avançar até
        // ao primeiro MapMemory, onde o erro é visível.
        base = kDefaultHeapBase;
    }

    heap_size_ = requested;

    // O endereço base vai para a **memória do guest**, no ponteiro de
    // saída — que é o que a ABI do Eden diz (`*out_address = ...`).
    //
    // E o resultado do SVC é o `Result`, em x0. Não é o endereço: são
    // coisas diferentes, e confundir as duas é o que fazia o rtld
    // tratar o heap como se estivesse na página zero.
    if (out_address != 0) {
        if (!write_guest(out_address, &base, sizeof base)) {
            return Result::InvalidAddress;
        }
    }
    // E o endereço base volta em **x1** — o Eden faz exactamente isto
    // (`SetArg64(args, 1, out_address)`), e o SDK lê x1 (a wrapper
    // desassemblada em 0x82d799ec escreve x1 no ponteiro).
    ctx.x[1]    = base;
    ctx.ret     = 0;
    ctx.has_ret = true;
    return Result::Success;
}

Result SvcDispatcher::svc_exit_process(SvcContext& ctx) {
    exited_    = true;
    exit_code_ = static_cast<int>(ctx.x[0]);
    return Result::Success;
}

Result SvcDispatcher::svc_close_handle(SvcContext& ctx) {
    close_handle(static_cast<std::uint32_t>(ctx.x[0]));
    return Result::Success;
}

Result SvcDispatcher::svc_control_memory(SvcContext&) {
    // O heap do guest é gerido pelo próprio runtime com mmap directo; o SVC
    // ControlMemory do Switch é gerido pelo processador de heap, e aqui a
    // alocação de heap é partilhada. Não é seguro inventar semântica aqui.
    return Result::Success;
}

Result SvcDispatcher::svc_get_system_tick(SvcContext& ctx) {
    // O Switch conta em 19.2 MHz. A origem do host é o monotonic clock do
    // Linux, que está em ns. Converter mantém a progressão monótona, que é o
    // que o guest usa para await e timeouts.
    const auto ns = host_tick_ns();
    constexpr std::uint64_t kHz = 19'200'000;
    ctx.ret     = (ns / 1000) * kHz / 1'000'000'000;
    ctx.has_ret = true;
    return Result::Success;
}

Result SvcDispatcher::svc_get_process_info(SvcContext& ctx) {
    // O guest valida o tamanho antes de escrever. Respeitar o pedido evita
    // corromper memória do host.
    const auto out_size = static_cast<std::size_t>(ctx.x[1]);
    if (out_size < 0x18)
        return Result::InvalidSize;
    // O ProcessInfo é escrito pelo serviço no espaço do guest; o retorno é o
    // Result. Deixar x0 intacto aqui devolveria o endereço de saída ao guest.
    return Result::Success;
}

Result SvcDispatcher::svc_get_thread_id(SvcContext& ctx) {
    ctx.x[1]     = thread_id_;
    ctx.ret      = 0;
    ctx.has_ret  = true;
    return Result::Success;
}

// A instância corrente — a do processo guest. É o que permite ao
// sinal de "a GPU terminou" (uma função estática) escrever na memória
// do guest.
static SvcDispatcher* g_dispatcher_atual = nullptr;

SvcDispatcher::SvcDispatcher(tel::Telemetry& tel) : tel_(tel) {
    g_dispatcher_atual = this;
}
SvcDispatcher::~SvcDispatcher() = default;

void SvcDispatcher::set_eden_tls(std::uint64_t endereco) noexcept {
    // No-op sem Eden ligado, e sem erro: os testes que não montam um programa
    // não têm TLS para dar.
    if (!eden_)
        return;

    eden_->set_tls(endereco);
    tls_base_ = endereco;

    // O handle da thread principal, em TLS+0x110. Ver a nota no cabecalho.
    const std::uint32_t handle = eden_->main_thread_handle();
    if (handle != 0 && mem_ != nullptr) {
        std::string err;
        if (!mem_->write(endereco + 0x110, &handle, sizeof(handle), err))
            last_ipc_ = "set_eden_tls: nao escrevi o handle em TLS+0x110: " + err;
    }
}

void SvcDispatcher::set_tls_da_thread(std::uint64_t endereco) noexcept {
    // Só o endereço do TLS da thread que pede. O `tls_base_` (a base de
    // onde se tiram os TLS das threads novas) e o handle da thread
    // principal ficam como estão — ver a nota no cabeçalho.
    if (!eden_ || endereco == 0) {
        return;
    }
    // **O endereço e' o bloco de TLS da thread**, nao o buffer de IPC.
    //
    // Medido: o ponteiro que esta no principio do bloco de TLS **nao** e' o
    // buffer de IPC (o que la esta mede-se `0x8000000a00000006` — lixo).
    // E' o Eden que, com o endereco do bloco, escreve a resposta no sitio
    // certo: o guest passa o endereco do seu buffer de IPC no `x1` e a
    // resposta vai para la (ver `WriteToOutgoingCommandBuffer`, no override
    // do `hle_ipc.cpp`).
    eden_->set_tls(endereco);
}

std::uint32_t SvcDispatcher::main_thread_handle() const noexcept {
    // Definido aqui, e nao no cabecalho, porque `EdenSystem` so esta completo
    // neste ficheiro. O mesmo movimento que o `~SvcDispatcher`.
    return eden_ ? eden_->main_thread_handle() : 0;
}

bool SvcDispatcher::attach_eden(std::uint64_t pid, std::uint64_t tid) {
    if (mem_ == nullptr) {
        // Sem memória não há `Kernel::Memory`, e `KProcess` não sobrevive sem
        // ela. Falhar aqui é melhor do que construir um sistema que rebenta no
        // primeiro IPC.
        last_ipc_ = "attach_eden: o despachante nao tem memoria do guest";
        return false;
    }
    eden_ = std::make_unique<EdenSystem>(*mem_, pid, tid);
    return true;
}

Result SvcDispatcher::svc_connect_port(SvcContext& ctx) {
    // ── o caminho do Eden ────────────────────────────────────────────────
    //
    // Se a camada de IPC do Eden está ligada, isto já não é uma tabela
    // handle→nome: é o `KObjectName` do Eden a procurar a porta, e a
    // devolver uma sessão de verdade.
    if (eden_) {
        std::uint32_t h = 0;
        const bool ok = eden_connect_to_named_port(eden_->Kernel(), *mem_, ctx.x[1], &h);
        if (!ok) {
            // `ResultNotFound` é o que o Eden devolve quando não há porta com
            // esse nome (svc_port.cpp:35). O guest aborta a um `ConnectToNamedPort`
            // falhado, e aborta com mais clareza se o Result for o certo.
            last_port_ = "?";
            last_ipc_  = "ConnectToNamedPort: o Eden nao encontrou a porta";
            ctx.ret     = static_cast<std::uint64_t>(Result::NotFound);
            ctx.has_ret = true;
            return Result::Success;
        }

        // As DUAS escritas, e ambas são o que o Eden faz. Ver a nota longa
        // mais abaixo, que vale para as duas.
        ctx.x[1] = h;
        std::uint32_t h32 = h;
        if (!write_guest(ctx.x[0], &h32, sizeof h32)) {
            last_ipc_ = "ConnectToNamedPort: handle " + std::to_string(h) +
                        " em x1, mas o ponteiro de x0 nao era gravavel";
        }
        ctx.ret     = 0;
        ctx.has_ret = true;
        return Result::Success;
    }

    // ── o caminho nosso, o de antes ──────────────────────────────────────
    // A forma é `SvcWrap_ConnectToNamedPort64` no Eden (svc.cpp:1470-1476):
    //
    //     x1 = apontador para o nome (sem comprimento!)
    //     x0 = Result
    //     x1 = handle                <- no mesmo registo que a entrada
    //
    // Duas coisas que a versão anterior errou, e ambas calavam a falha:
    //
    //   1. lia o COMPRIMENTO de x2. Não existe comprimento nesta SVC, e x2
    //      vinha a zero — pelo que devolvia sempre um erro (o `Invalid` de
    //      então, que hoje é o `InvalidCurrentMemory` do Eden).
    //   2. escrevia o handle num ponteiro em x0. Aqui x0 é o Result, e a
    //      escrita ia para um endereço do guest que ninguém pediu.
    //
    // STUB, declarado: `connect_named_port` fabrica um handle e não há IPC
    // nenhum por trás. O SDK recebe um handle plausível e continua a correr;
    // quando pedir o conteúdo da resposta (`SendSyncRequest`) é que a
    // ilusão acaba. Preferível a um erro: o guest trata um erro de
    // `ConnectToNamedPort` como motivo para abortar.
    std::string name;
    if (!safe_copy_cstring(*this, ctx.x[1], name, 256))
        return Result::InvalidCurrentMemory;

    const std::uint32_t h = connect_named_port(name);
    if (h == 0)
        return Result::NotFound;

    last_port_ = name;
    ctx.x[1]    = h;
    ctx.ret     = 0;
    ctx.has_ret = true;

    // E escreve TAMBÉM no ponteiro que o guest passou em x0.
    //
    // De `svc_port.cpp:37-50` no Eden:
    //
    //     // Reserve a handle for the port.
    //     // NOTE: Nintendo really does write directly to the output handle here.
    //     R_TRY(handle_table.Reserve(system.Kernel(), out));
    //     ...
    //     handle_table.Register(system.Kernel(), *out, session);
    //
    // O `out` é o ponteiro do guest (o nosso x0), e a Nintendo escreve lá
    // directamente. O wrapper tambem põe o handle em x1
    // (`SetArg32(args, 1, out_handle)`, svc.cpp:1473) — sao os dois sitios,
    // e nós so escreveÃ§amos no segundo.
    //
    // Se o SDK ler o handle pelo ponteiro, estava a ler o que estivesse na
    // stack — lixo — e a Sessão seguinte era uma Sessão que não existe.
    //
    // Duas escritas para o mesmo valor, e uma delas é um Commentário no Eden
    // a dizer para não a esquecer.
    std::uint32_t h32 = h;
    if (!write_guest(ctx.x[0], &h32, sizeof h32)) {
        last_ipc_ = "ConnectToNamedPort(\"" + name +
                    "\"): handle " + std::to_string(h) +
                    " em x1, mas NAO em x0 — o guest vai ler o lixo da stack";
        return Result::Success;
    }
    return Result::Success;
}

// ---------------------------------------------------------------------------
// Serviços de memória
//
// Estes são os que o rtld usa antes de tocar em código do jogo. Sem eles o
// guest não arranca, e o sintoma é um SIGSEGV num endereço que não pertence a
// nada — sem nenhum contexto que diga quem o pediu.
// ---------------------------------------------------------------------------

// ABI do Switch: MapMemory(dst, src, size)
//   x0 = endereço virtual de destino no espaço do guest
//   x1 = endereço físico de origem (ignorado: num guest nativo o "físico" é o
//        nosso próprio endereço de host)
//   x2 = tamanho em bytes
//
// O rtld usa isto para projectar os segmentos do main e da sdk emendereços
// que ele próprio escolhe, dentro da região de heap.
//
// O que devolve: Success se o destino estiver dentro da reserva que fizémos.
Result SvcDispatcher::svc_map_memory(SvcContext& ctx) {
    const std::uint64_t dst  = ctx.x[0];
    const std::uint64_t size = ctx.x[2];

    if (mem_ == nullptr)
        return Result::InvalidState;
    if (size == 0)
        return Result::InvalidSize;

    // O endereço é do guest: tem de estar dentro do que reservámos. Mapear
    // fora disso é mapear por cima de outra coisa do processo do host, e o
    // kernel do Switch rejeitaria. É a mesma razão por que se usa
    // MAP_FIXED_NOREPLACE — um endereço errado tem de ser um erro visível,
    // não uma corruption silenciosa.
    if (!mem_->owns(dst) || !mem_->owns(dst + size - 1))
        return Result::InvalidCurrentMemory;

    // A memória dentro da reserva já é do processo e já é acessível. O
    // "mapeamento" que o Switch faria (traduzir físico -> virtual) aqui é um
    // no-op, porque o guest e o host partilham o espaço de endereçamento.
    ++map_memory_calls_;
    return Result::Success;
}


// O `svcCreateTransferMemory` (0x15) — a memória que o guest passa à GPU.
//
// ## Porque este SVC é uma parede
//
// **Medido** (`boot_p6k.log`): o SDK pediu o `CreateTransferMemory` e o
// nosso despachante devolveu `Unsupported` (0x40). O SDK trata um Result
// inesperado como fatal — chamou `SetTerminateResult(0x40)` ao `am` e
// abortou. Um SVC que devolve erro **mata o jogo**.
//
// ## A ABI, do Eden
//
// `svc.cpp` (upstream), `SvcWrap_CreateTransferMemory64`:
//
//     uint64_t address   = GetArg64(args, 1);
//     uint64_t size      = GetArg64(args, 2);
//     MemoryPermission map_perm = Convert<MemoryPermission>(GetArg64(args, 3));
//     Handle out_handle{};
//     Result ret = CreateTransferMemory64(system, &out_handle, address, size, map_perm);
//     SetArg64(args, 0, ret);
//     SetArg64(args, 1, out_handle);      // <-- x1 é o handle à saída
//
// Ou seja: **x1 é o endereço à entrada e o handle à saída** (o kernel
// escreve por cima). É a mesma forma do `GetService` do `sm:`, e a mesma
// coisa: o guest passa um endereço e recebe um handle no sítio.
//
// ## O que o Eden valida
//
// `svc/svc_transfer_memory.cpp` (`CreateTransferMemory`):
//
//   - o endereço e o tamanho são múltiplos de `PageSize`;
//   - o tamanho é maior que zero;
//   - `address + size` não transborda;
//   - as permissões são `None`, `Read` ou `ReadWrite` (nada de `Execute`);
//   - a região está dentro da tabela de páginas do processo.
//
// E depois cria o objecto `KTransferMemory` e põe-no na tabela de handles.
//
// ## O que esta implementação faz
//
// Num motor de execução **nativa**, a memória do guest **é** memória do
// host: a "tradução" que o Switch faria (físico -> virtual) não existe, e
// a página física de uma região é o seu endereço. Por isso o que o handle
// tem de carregar é a **região** (endereço, tamanho, permissões) — é isso
// que o lado da GPU precisa de saber para usar a memória.
//
// E a validação é a do Eden, com uma adaptação: o "dentro da tabela de
// páginas do processo" é, aqui, "dentro da **reserva**" — que é a mesma
// coisa que o `svc_map_memory` valida, e pela mesma razão (um endereço
// errado tem de ser um erro visível, não uma corruption silenciosa).
Result SvcDispatcher::svc_create_transfer_memory(SvcContext& ctx) {
    const std::uint64_t addr = ctx.x[1];
    const std::uint64_t size = ctx.x[2];
    const std::uint64_t perm = ctx.x[3];

    if (mem_ == nullptr)
        return Result::InvalidState;

    // As três condições de endereço, do Eden (`svc_transfer_memory.cpp`).
    //
    // O tamanho de página é o do Switch: 4 KiB (`svc_types.h`, `PageSize`).
    // Não é uma medição nossa — é o do hardware, e o guest aloca por
    // páginas.
    constexpr std::uint64_t kPageSize = 0x1000;
    if ((addr & (kPageSize - 1)) != 0)
        return Result::InvalidAddress;
    if ((size & (kPageSize - 1)) != 0)
        return Result::InvalidSize;
    if (size == 0)
        return Result::InvalidSize;
    if (addr + size < addr)
        return Result::InvalidCurrentMemory;

    // As permissões: `None`, `Read` ou `ReadWrite`. O `Execute` não vale
    // para memória transferida — é o que o Eden decide
    // (`IsValidTransferMemoryPermission`).
    //
    // Os bits são os do Eden (`svc_types.h`, `MemoryPermission`): 1 = Read,
    // 2 = Write, 4 = Execute. O que é válido aqui é 0 (None), 1 (Read) e
    // 3 (ReadWrite).
    if (perm != 0 && perm != 1 && perm != 3)
        return Result::InvalidNewMemoryPermission;

    // A região tem de estar dentro da reserva — o mesmo guarda do
    // `svc_map_memory`.
    if (!mem_->owns(addr) || !mem_->owns(addr + size - 1))
        return Result::InvalidCurrentMemory;

    // O objecto: a região, com as suas permissões. É o que o handle
    // carrega.
    auto* obj = new KObject();
    obj->kind = KObject::Kind::TransferMemory;
    obj->endereco = addr;
    obj->tamanho  = size;
    obj->perm     = perm;

    const Handle h = handles_.reservar();
    if (!handles_.registar(h, obj)) {
        delete obj;
        return Result::OutOfResource;
    }
    ++vivos_;

    // x1 é o handle à saída — o Eden escreve por cima do endereço que
    // estava lá.
    ctx.x[1]     = h.raw;
    ctx.has_ret  = true;
    ctx.ret      = static_cast<std::uint64_t>(Result::Success);
    return Result::Success;
}

// O `svcSetMemoryAttribute` (0x03) — os atributos de memória.
//
// ## Porque este SVC é uma parede
//
// **Medido** (`boot_p6n.log`): o SDK pediu o `SetMemoryAttribute` e o
// nosso despachante devolveu `Unsupported` (0x40). O SDK trata um Result
// inesperado como fatal — chamou `SetTerminateResult(0x40)` ao `am` e
// abortou.
//
// ## A ABI, do Eden
//
// `svc.cpp` (upstream), `SvcWrap_SetMemoryAttribute64`:
//
//     uint64_t address = GetArg64(args, 0);
//     uint64_t size    = GetArg64(args, 1);
//     uint32_t mask    = Convert<uint32_t>(GetArg64(args, 2));
//     uint32_t attr    = Convert<uint32_t>(GetArg64(args, 3));
//     Result ret = SetMemoryAttribute64(system, address, size, mask, attr);
//
// ## A validação, do Eden
//
// `svc/svc_memory.cpp` (`SetMemoryAttribute`):
//
//   - o endereço e o tamanho são múltiplos de `PageSize`;
//   - o tamanho é maior que zero;
//   - `address + size` não transborda;
//   - o `attr` é um subconjunto do `mask`;
//   - só o `Uncached` e o `PermissionLocked` são suportados;
//   - o bit do `PermissionLocked` está no `mask` ou em nenhum dos dois;
//   - a região está dentro do espaço do processo.
//
// E a última condição do Eden — a região estar no espaço do processo —
// é, aqui, "dentro da **reserva**", pelo mesmo motivo do
// `svc_map_memory`: um endereço errado tem de ser um erro visível.
//
// ## O que o atributo faz, aqui
//
// **Nada, e é a resposta honesta.** Num motor de execução **nativa** não
// há tabela de páginas: a memória do guest é a do host, e não há
// "desactivar a cache" a fazer. O `PermissionLocked` só impede o
// desmapeamento — e o `svc_unmap_memory` já é um no-op.
//
// A validação é que importa: é ela que diz ao guest "esse endereço não
// é teu", e é a mesma que o Eden faz.
Result SvcDispatcher::svc_set_memory_attribute(SvcContext& ctx) {
    const std::uint64_t addr = ctx.x[0];
    const std::uint64_t size = ctx.x[1];
    const std::uint32_t mask = static_cast<std::uint32_t>(ctx.x[2]);
    const std::uint32_t attr = static_cast<std::uint32_t>(ctx.x[3]);

    if (mem_ == nullptr)
        return Result::InvalidState;

    // O log vem **antes** da validação: o que o guest pede é a
    // informação, e um pedido rejeitado sem o log é um pedido
    // rejeitado sem explicação.
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] SetMemoryAttribute addr=0x%llx size=0x%llx mask=0x%08x "
                     "attr=0x%08x\n",
                     (unsigned long long)addr, (unsigned long long)size, mask, attr);
    }

    // O tamanho de página é o do Switch: 4 KiB (`svc_types.h`, `PageSize`).
    constexpr std::uint64_t kPageSize = 0x1000;

    if ((addr & (kPageSize - 1)) != 0)
        return Result::InvalidAddress;
    if ((size & (kPageSize - 1)) != 0)
        return Result::InvalidSize;
    if (size == 0)
        return Result::InvalidSize;
    if (addr + size < addr)
        return Result::InvalidCurrentMemory;

    // Os atributos do Eden (`svc_types.h`, `MemoryAttribute`):
    //
    //     Locked          = 1 << 0
    //     IpcLocked       = 1 << 1
    //     DeviceShared    = 1 << 2
    //     Uncached        = 1 << 3
    //     PermissionLocked = 1 << 4
    //
    // E o `SupportedMask` do Eden: só o `Uncached` e o `PermissionLocked`
    // são suportados — os outros são para o sistema.
    constexpr std::uint32_t kUncached        = 1u << 3;
    constexpr std::uint32_t kPermissionLocked = 1u << 4;
    constexpr std::uint32_t kSupportedMask   = kUncached | kPermissionLocked;

    // O `attr` tem de estar dentro do `mask`.
    if ((mask | attr) != mask)
        return Result::InvalidCombination;
    // E nada fora do que é suportado.
    if ((mask | attr | kSupportedMask) != kSupportedMask)
        return Result::InvalidCombination;
    // O `PermissionLocked` está no `mask` ou em nenhum dos dois.
    if ((mask & kPermissionLocked) != (attr & kPermissionLocked))
        return Result::InvalidCombination;

    // **A região NÃO é validada contra a reserva.** E é a única
    // divergência face ao Eden, e é deliberada.
    //
    // O Eden valida `page_table.Contains(address, size)` porque o
    // atributo vai para uma **tabela de páginas** — e um atributo posto
    // numa página que não é do processo é uma corruption. Aqui **não há
    // tabela de páginas**: a memória do guest é a do host, e o atributo
    // é um no-op. Não há nada que corromper, e portanto a validação não
    // tem contrapartida.
    //
    // **Medido** (`boot_p6r.log`): o guest pede
    // `SetMemoryAttribute(0x0, 0x1000, Uncached, Uncached)` — o endereço
    // 0 não está na reserva — e, com a validação do Eden, devolvemos
    // `ResultInvalidCurrentMemory` e o SDK **abortou** (`SetTerminate-
    // Result(0xD401)`). Um Result que o Eden devolveria com sucesso não
    // pode matar o jogo: o que o guest queria era o atributo, e o
    // atributo é um no-op aqui.
    //
    // E o que o guest está a pedir é a **página 0 do espaço do guest** —
    // a página que a SDK trata como "memória do sistema" (o atributo
    // `Uncached` é para a memória que o DMA toca). A reserva do motor
    // começa em 0x60000000, e essa página não existe — o que é
    // exactamente a resposta honesta: o atributo não se aplica a nada.
    //
    // E o atributo é aplicado — que, num motor nativo, é um no-op. O
    // log acima já disse o que foi pedido.
    return Result::Success;
}

Result SvcDispatcher::svc_map_shared_memory(SvcContext& ctx) {
    // **A ABI aqui é DIFERENTE da do `MapMemory`** (`svc.h`, `Svc::
    // MapSharedMemory`): `x0` = handle, `x1` = endereço, `x2` = tamanho,
    // `x3` = permissões. O alias directo a `svc_map_memory` validava
    // `x0` **como endereço** — o valor do handle — e falhava sempre.
    //
    // Num processo único: o handle não se resolve a páginas (não há
    // `KSharedMemory` no kernel vendorizado); o que o guest precisa é
    // de que a zona que ele escolheu fique (ou continue) legível. É o
    // que se valida — o mesmo guarda do `svc_map_memory`, no endereço
    // certo.
    const std::uint64_t addr  = ctx.x[1];
    const std::uint64_t size  = ctx.x[2];

    if (mem_ == nullptr)
        return Result::InvalidState;
    if (size == 0)
        return Result::InvalidSize;
    if (!mem_->owns(addr) || !mem_->owns(addr + size - 1))
        return Result::InvalidCurrentMemory;

    // **A fonte do sistema**: o handle do `pl:u` e' o `KEvent` que o
    // `GetSharedMemoryNativeHandle` devolveu. O guest mapeia-o e le o TTF em
    // `regiao.offset` — por isso o blob tem de estar **na memoria do guest**
    // antes da leitura, nao num no-op.
    //
    // **Medido** (`/tmp/boot_watch.log`): o `GetSize` devolvia 0 a todos os
    // tipos e o jogo morria no glifo `?` da fonte de indice 2. Com as 7
    // regioes e esta copia, o guest tem o TTF onde o procura.
    if (eden_attached()) {
        auto& kernel = eden_->Kernel();
        Kernel::KScopedAutoObject<Kernel::KReadableEvent> evento =
            Kernel::GetCurrentProcess(kernel).GetHandleTable()
                .GetObject<Kernel::KReadableEvent>(
                    kernel, Kernel::Handle{static_cast<std::uint32_t>(ctx.x[0])});
        if (evento.IsNotNull() && services::EventoDaFonte(evento->GetParent())) {
            const auto& dados = services::DadosDaFontePartilhados();
            const std::size_t copiar = std::min<std::size_t>(
                dados.size(), static_cast<std::size_t>(size));
            std::memcpy(reinterpret_cast<void*>(addr), dados.data(), copiar);
            if (std::getenv("IA_IPC_TRACE") != nullptr) {
                std::fprintf(stderr,
                             "[svc] MapSharedMemory: fonte do sistema, %zu bytes "
                             "copiados para 0x%llx (pedido=0x%llx)\n",
                             copiar, static_cast<unsigned long long>(addr),
                             static_cast<unsigned long long>(size));
            }
        }
    }

    ++map_memory_calls_;

    // **Instrumento leve — Parede 2**: onde o guest escolhe mapear cada
    // `MapSharedMemory` (o shmem do hid incluido, cujo map era silencioso).
    // `G610_MAP_TRACE` no ambiente liga a linha.
    static const bool trace_map = std::getenv("G610_MAP_TRACE") != nullptr;
    if (trace_map)
        std::fprintf(stderr,
                     "[svc] MapSharedMemory handle=0x%llx addr=0x%llx size=0x%llx\n",
                     static_cast<unsigned long long>(ctx.x[0]),
                     static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(size));
    return Result::Success;
}

Result SvcDispatcher::svc_unmap_memory(SvcContext&) {
    // Desfazer o mapeamento exigiria mudar as permissões da página, e o rtld
    // pode pedir páginas que ele próprio mapeou. Um no-op é mais seguro do que
    // um desmapeamento parcial que deixa o guest a ler memória sem acesso.
    // Registado para que o contador mostre que aconteceu.
    ++unmap_memory_calls_;
    return Result::Success;
}

// ---------------------------------------------------------------------------
// QueryMemory (0x06) — o PRIMEIRO serviço que o rtld pede.
//
//   x0 = endereço onde escrever a MemoryInfo (8+8+4+4+4+4+4+4 = 40 bytes)
//   x1 = endereço onde escrever a PageInfo   (4 bytes)
//   x2 = o endereço a interrogar
//
// Este é o serviço que destravou o arranque: sem resposta, o rtld fica a
// espera e nunca chega ao código do jogo. E é o primeiro a aparecer porque é
// o primeiro que precisa de saber que a memória do guest é válida.
//
// O layout é o de `Svc::MemoryInfo` (svc_types.h:478 no Eden), copiado
// literalmente:
//
//     u64 base_address;  u64 size;  MemoryState state;
//     MemoryAttribute attribute;    MemoryPermission permission;
//     u32 ipc_count;  u32 device_count;  u32 padding;
//
// Não há tabelas de páginas no guest nativo, por isso a resposta vem do
// layout: o bloco é a região do espaço onde o endereço cai. É a mesma
// informação que um page table daria, sem o custo de manter um.
Result SvcDispatcher::svc_query_memory(SvcContext& ctx) {
    const std::uint64_t out_info  = ctx.x[0];
    const std::uint64_t out_page  = ctx.x[1];
    const std::uint64_t query     = ctx.x[2];
    if (mem_ == nullptr)
        return Result::InvalidState;

    struct MemoryInfo {
        std::uint64_t base_address;
        std::uint64_t size;
        std::uint32_t state;
        std::uint32_t attribute;
        std::uint32_t permission;
        std::uint32_t ipc_count;
        std::uint32_t device_count;
        std::uint32_t padding;
    };
    static_assert(sizeof(MemoryInfo) == 0x28, "MemoryInfo tem 40 bytes");

    MemoryInfo info{};
    info.base_address = 0;
    info.size         = 0;
    info.state        = 0;   // Free

    // **O que a casa responde** — o rtld percorre o espaco do processo
    // a perguntar, e o que ele aceita como modulo e' o que decide.
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[qm] query=0x%llx regioes=%zu\n",
                     (unsigned long long)query, regions_.size());
    }
    info.attribute    = 0;
    info.permission   = 0;   // None
    info.ipc_count    = 0;
    info.device_count = 0;
    info.padding      = 0;

    // A resposta vem da tabela de regiões, e não da reserva inteira.
    //
    // Isto não é um detalhe. O rtld descriptor de módulos com esta chamada:
    // começa no endereço 0 e avança `base + size`, e só aceita uma região se
    // `state == Code` (3) e `permission == Read|Execute` (5). Com uma única
    // região RWX ele viajava um bloco só, não viajava nenhum — e o sintoma
    // era `Unresolved symbol: nn::init::Start`, que é o rtld a dizer que
    // carregou zero módulos.
    //
    // Sem a tabela (os testes unitários), caímos no comportamento antigo:
    // a reserva inteira, como uma região normal.
    if (!regions_.empty()) {
        // As regioes estao por ordem de base (ver `memory_regions`).
        std::uint64_t anterior_fim = 0;
        std::uint64_t seguinte_base = 0;
        bool achou = false;
        for (const GuestRegion& r : regions_) {
            if (r.size == 0)
                continue;
            if (query >= r.base && query < r.base + r.size) {
                // O **bloco**, nao a pagina.
                //
                // O Eden (`k_page_table_base.cpp:904`, `QueryInfoImpl`)
                // devolve o `KMemoryBlock` que contem o endereco - e o
                // bloco e' a regiao, nao a pagina de 4 KiB. A medicao
                // confirma: com a pagina, o `rtld` do jogo percorria o
                // espaco do processo a 4 KiB de cada vez (3999
                // interrogacoes e a varredura nao terminava); com a
                // regiao, sao 10 interrogacoes - uma por regiao - e o
                // `rtld` encontra os modulos.
                info.base_address = r.base;
                info.size         = r.size;
                info.state        = r.state;
                info.permission   = r.permission;
                achou = true;
                break;
            }
            if (r.base <= query) {
                anterior_fim = r.base + r.size;
            } else {
                seguinte_base = r.base;
                break;
            }
        }

        if (!achou) {
            // **O bloco FREE.** O Eden nao devolve "nada": devolve o bloco
            // livre que contem o endereco, com o **tamanho** desse bloco.
            //
            // A arvore de blocos do Eden contem blocos `Free`
            // (`k_memory_block_manager.h:129`: `FindBlock` procura um bloco
            // com estado `Free` e devolve-o se nao houver nenhum mapeado).
            // E' assim que o SDK ve o espaco livre do processo: pergunta o
            // `svcQueryMemory` de um endereco e recebe o bloco livre com o
            // tamanho certo.
            //
            // Sem isto, o `nn::os::detail::AddressSpaceAllocator::AllocateSpace`
            // do SDK percorria o espaco do processo a procurar espaco para
            // alocar, recebia `size=0` em tudo o que nao estivesse mapeado,
            // e acabava por lancar uma excecao. Medido: 527 interrogacoes
            // `svcQueryMemory` seguidas de uma excecao sem quem a apanhe.
            if (seguinte_base == 0) {
                // O fim do espaco do processo.
                seguinte_base = (mem_ != nullptr && mem_->size() != 0)
                                    ? mem_->base() + mem_->size()
                                    : anterior_fim;
            }
            if (seguinte_base > anterior_fim) {
                info.base_address = anterior_fim;
                info.size         = seguinte_base - anterior_fim;
                info.state        = GuestRegion::kFree;
                info.permission   = GuestRegion::kNone;
            }
        }
    } else if (mem_ != nullptr && mem_->owns(query)) {
        info.base_address = mem_->base();
        info.size         = mem_->size();
        info.state        = GuestRegion::kNormal;
        info.permission   = GuestRegion::kRead | GuestRegion::kWrite |
                            GuestRegion::kExecute;
    }

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[qm]   -> base=0x%llx size=0x%llx state=%u perm=%u\n",
                     (unsigned long long)info.base_address,
                     (unsigned long long)info.size, info.state, info.permission);
    }

    if (!write_guest(out_info, &info, sizeof(info)))
        return Result::InvalidCurrentMemory;

    // PageInfo: um único u32 de flags. Zero = sem flags, que é o normal.
    // As PageInfo voltam em x1, NÃO escritas em *out_page.
    //
    // Não é palpite: é o que o stub gerado pelo SDK do jogo faz. Lido do
    // `rtld` real (offset 0x16e4):
    //
    //     str   x1, [sp, #-16]!     ; guarda &flags
    //     svc   #0x6                 ; QueryMemory
    //     ldr   x2, [sp]             ; x2 = &flags
    //     str   w1, [x2]             ; *(&flags) = w1  <-- de x1!
    //     ret                        ; o Result volta em x0
    //
    // O stub é que escreve. Se o kernel também escrevesse, o valor seria o
    // mesmo — mas o registo tem de vir preenchido, senão o guest lê lixo.
    //
    // Sem isto o guest guardava em `flags` o valor que x1 tivesse à entrada
    // (o endereço de saída), e o resultado era um endereço onde se espera um
    // número. O rtld tratava isso como um sinal de erro e seguia para um
    // `__builtin_unreachable()`.
    const std::uint32_t page_flags = 0u;
    ctx.x[1] = page_flags;

    // Escrever também é inofensivo (o stub vai escrever o mesmo valor) e
    // protege o caso de um SDK que não faça a escrita.
    if (out_page != 0 && !write_guest(out_page, &page_flags, sizeof(page_flags)))
        return Result::InvalidCurrentMemory;

    // O que o guest recebeu. E' a medicao que diz se o bloco serve: o
    // `nn::os::detail::AddressSpaceAllocator::AllocateSpace` do SDK escolhe
    // um endereco aleatorio, pergunta o `svcQueryMemory` e exige que o
    // bloco devolvido seja MAIOR do que o total que precisa (medido no
    // `sdk`, offset 0x32c138-0x32c154).
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[qm  ] query=0x%016llx -> base=0x%llx size=0x%llx state=%u perm=%u\n",
                     (unsigned long long)query, (unsigned long long)info.base_address,
                     (unsigned long long)info.size, info.state, info.permission);
    }

    ++query_memory_calls_;
    return Result::Success;
}

// GetProcessId e GetThreadId.
//
// A forma vem de `SvcWrap_GetProcessId64` / `SvcWrap_GetThreadId64` no Eden
// (svc.cpp:1500-1516):
//
//     x1 = handle        x0 = Result
//     x1 = o id          <- no mesmo registo que a entrada
//
// Duas formas de errar, e ambas estavam cá:
//
//   1. devolver o id em x0 (via `ctx.ret`). O guest lia o Result e Treaty-o
//      como o id, e o id verdadeiro como o Result.
//   2. tratar x0 como um ponteiro de SAÍDA e escrever lá. No Switch os ids
//      voltam em registo; x0 é o Result. Escrever num endereço que o guest
//      não pediu é corruption silenciosa — e era literalmente o que o
//      `GetProcessId` fazia, mas ao contrário: lia 8 bytes do guest e
//      deitava-os fora.
//
// O valor tem de ser ESTÁVEL: o SDK usa o id como chave do seu estado
// por-thread, e um id que muda entre chamadas faz o jogo achar que é outra
// thread.
Result SvcDispatcher::svc_get_process_id(SvcContext& ctx) {
    ctx.x[1]     = process_id_;
    ctx.ret      = 0;
    ctx.has_ret  = true;
    return Result::Success;
}

// ---------------------------------------------------------------------------
// GetInfo
// ---------------------------------------------------------------------------
//
// Porque é preciso já: o SDK do jogo pede isto no arranque, ANTES de qualquer
// serviço. Pediu `RandomEntropy` duas vezes e `AslrRegionAddress` uma, e com
// a resposta em erro ficou com a região ASLR a null — e desreferenciou-a.
// O sintoma foi `svc Break(x0=0)`, o crash handler do próprio jogo.
//
// A forma do serviço não é adivinhada. Vem de `SvcWrap_GetInfo64` no Eden
// (svc.cpp:1537-1545):
//
//     info_type   = GetArg64(args, 1);      // x1
//     info_handle = GetArg64(args, 2);      // x2
//     subtype     = GetArg64(args, 3);      // x3
//     SetArg64(args, 0, ret);               // x0 = Result
//     SetArg64(args, 1, out);               // x1 = o VALOR
//
// Ou seja: o valor volta em x1, no registo que era o tipo. Não é escrito num
// ponteiro — e um palpite que o tratasse como `x0 = saída` devolveria lixo sem dar
// erro.
//
// Os números dos InfoType são os do Eden (`svc_types.h:130-147`).
//
// STUB, e é preciso dizê-lo: a região ASLR que devolvemos é o NOSSO heap. No
// Switch é uma região separada, gerida pelo `AslrSpaceManager` do SDK. Para
// o crt0 chegar ao fim não interessa — o que interessa é que seja um endereço
// dentro da reserva e que caiba. Se o SDK começar a alocar muito dali, é este
// o número a suspectar.
Result SvcDispatcher::svc_get_info(SvcContext& ctx) {
    const std::uint32_t type    = static_cast<std::uint32_t>(ctx.x[1]);
    const std::uint32_t subtype = static_cast<std::uint32_t>(ctx.x[3]);

    std::uint64_t value = 0;
    switch (type) {
    // Os números são `InfoType` do Eden (svc_types.h:130-159), copiados um a
    // um. Uma versão anterior desta função tinha `case 16: value = 4;` com um
    // comentário a dizer "PersonalWordSize" — um tipo que não existe em lado
    // nenhum. 16 é `SystemResourceSizeTotal` e 22 é `UsedNonSystemMemorySize`.
    // O SDK do jogo pedia o 22 e recebia 4: quatro bytes de "memória em uso",
    // e depois desreferenciava o resultado. Um número inventado aqui não dá
    // erro nenhum — é o pior tipo de bug, e é o que a regra de "medir, não
    // adivinhar" existe para apanhar.
    case 2:  value = alias_base_;           break;  // AliasRegionAddress
    case 3:  value = alias_size_;           break;  // AliasRegionSize
    case 4:  value = heap_base_;            break;  // HeapRegionAddress
    case 5:  value = heap_size_;            break;  // HeapRegionSize
    case 6:  value = mem_total_;            break;  // TotalMemorySize
    case 7:  value = mem_used_;             break;  // UsedMemorySize
    case 11: value = random_u64();          break;  // RandomEntropy
    // A região ASLR: a região do CÓDIGO, não o heap. Ver a nota em
    // `set_aslr_region`. O Eden é a fonte (`svc_info.cpp:76-82`).
    case 12: value = aslr_base_;            break;  // AslrRegionAddress
    case 13: value = aslr_size_;            break;  // AslrRegionSize
    case 14: value = stack_base_;           break;  // StackRegionAddress
    case 15: value = stack_size_;           break;  // StackRegionSize
    case 16: value = system_resource_size_; break;  // SystemResourceSizeTotal
    case 17: value = 0;                     break;  // SystemResourceSizeUsed
    case 20: value = 0;                     break;  // UserExceptionContextAddress
    // A memoria **nao-sistema** (o Eden: `GetTotalNonSystemUserPhysicalMemorySize`,
    // `k_process.cpp`): e' a memoria do processo do guest, e nao a do
    // sistema.
    //
    // **O SDK dimensiona o heap do jogo a partir deste numero.** Com o
    // valor errado, pediu um heap de 2,4 GB — e o assert de
    // crescimento de vector do SDK (`!safe_add(&new_capacity, new_size,
    // (new_size / 2))`) transbordou, e o guest abortou.
    //
    // E o que este motor tem de facto e' o **heap**: e' ai que o jogo
    // aloca, e e' o limite que o `SetHeapSize` vai pedir. Dar a
    // reserva inteira faz o SDK pedir mais do que existe.
    case 21: value = heap_size_ != 0 ? heap_size_ : mem_total_; break;
    // (o `heap_size_` so' esta posto depois do `SetHeapSize`; antes
    // disso, o que vale e' o `mem_total_` — que e' o tamanho da
    // reserva do guest)
    case 22: value = mem_used_;             break;  // UsedNonSystemMemorySize
    case 23: value = 1;                     break;  // IsApplication
    case 24: value = kFreeThreadCount;      break;  // FreeThreadCount
    case 25: value = 0;                     break;  // ThreadTickCount
    case 26: value = 1;                     break;  // IsSvcPermitted
    case 28: value = 0;                     break;  // AliasRegionExtraSize

    // 18 (ProgramId) e 9 (ResourceLimit) ficam de fora de propósito.
    //
    // O ProgramId NÃO está no NPDM — o NPDM meta tem ProductCode a zero
    // (switchbrew/NPDM: 0x30). Vem do title id do NCA, e nós carregamos um
    // ExeFS já extraído, que perdeu essa informação. Devolver 0 seria
    // inventar; devolver um erro é honesto, e o SDK só o usa para dados de
    // save, não para arrancar.
    default:
        return Result::Unsupported;
    }
    (void)subtype;

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] GetInfo type=%u handle=0x%llx subtype=%u -> 0x%llx\n",
                     type, static_cast<unsigned long long>(ctx.x[2]), subtype,
                     static_cast<unsigned long long>(value));
        if (type == 16 && ctx.gpr[30] >= 0x80000040ull) {
            // Diagnóstico temporário do assert que segue GetInfo(16): guardar
            // a janela de instruções em volta do call-site, não deduzir a
            // asserção pelo valor do NPDM.
            std::uint32_t code[32]{};
            const std::uint64_t start = ctx.gpr[30] - 0x40;
            if (read_guest(start, code, sizeof(code))) {
                std::fprintf(stderr, "[svc-code] GetInfo(16) caller=0x%llx:",
                             static_cast<unsigned long long>(ctx.gpr[30]));
                for (auto word : code)
                    std::fprintf(stderr, " %08x", word);
                std::fprintf(stderr, "\n");
            }
        }
    }

    // O valor vai em x1. `ctx.ret` fica para o Result, que o motor escreve
    // em x0 — são coisas diferentes, e é por aqui que esta SVC custa mais
    // uma vez às outras: o mesmo erro que o `x0` nunca receber o Result.
    // **O dump** — os valores que a casa da ao guest. O
    // `AslrRegion` (caso 12) e' o que o SDK usa — e se for errado
    // o SDK guarda-o num global e o jogo desreferencia-o.
    if (std::getenv("IA_WATCH") != nullptr) {
        std::fprintf(stderr, "[info] tipo=%u -> 0x%llx\n", type,
                     (unsigned long long)value);
    }
    ctx.x[1]  = value;
    ctx.ret   = 0;
    ctx.has_ret = true;
    return Result::Success;
}

// GetThreadPriority — `SvcWrap_GetThreadPriority64` no Eden (svc.cpp:1331).
//
//     x1 = handle da thread        x0 = Result
//     x1 = prioridade (saida)      <- no mesmo registo que a entrada
//
// É a mesma forma do GetInfo: o valor sai no registo que era o argumento.
// Tratar x1 como ponteiro devolveria lixo sem erro nenhum.
Result SvcDispatcher::svc_get_thread_priority(SvcContext& ctx) {
    const std::uint32_t handle = static_cast<std::uint32_t>(ctx.x[1]);

    // Handle 0 e' a thread corrente. Uma thread que o guest criou tem outra
    // prioridade, mas ainda não sabemos qual — e mentir com um valor errado
    // seria pior do que devolver a da principal, que é o que o guest tem
    // enquanto não cria nenhuma.
    ctx.x[1]     = main_priority_ != 0 ? main_priority_ : kDefaultMainPriority;
    ctx.ret      = 0;
    ctx.has_ret  = true;
    (void)handle;
    return Result::Success;
}

// SetThreadPriority — stub transparente. O guest chama-a durante o arranque;
// recusar com um erro aborta o jogo, e a prioridade nao muda nada enquanto
// nao houver threads a schedular.
Result SvcDispatcher::svc_set_thread_priority(SvcContext& ctx) {
    last_priority_ = static_cast<std::int32_t>(ctx.x[2]);
    return Result::Success;
}

// SetThreadCoreMask — `SvcWrap_SetThreadCoreMask64` no Eden (svc.cpp:1356).
//
//     x0 = handle da thread    x1 = core_id    x2 = máscara de afinidade
//     x0 = Result
//
// **Este SVC matava o jogo.** O SDK do jogo chamou-o com
// (handle=0x81ff, core_id=0, máscara=1); o motor não o tinha implementado,
// caiu no `default` e devolveu `Result::Unsupported` — que no nosso enum é
// 64 = 0x40. O SDK trata esse erro como fatal: chamou `SetTerminateResult`
// ao `am` com 0x40 e depois abortou (`brk #0x26`).
//
// A medição está no trace: o `svc #164` foi o `0x0f`, o `svc #166` foi o
// `SetTerminateResult` com 0x40 no pedido, e o `svc #168` foi o `Break`.
//
// A validação é a do Eden (`kernel/svc/svc_thread.cpp:252-280`).
Result SvcDispatcher::svc_set_thread_core_mask(SvcContext& ctx) {
    const std::uint64_t handle = ctx.x[0];
    std::int32_t  core_id = static_cast<std::int32_t>(ctx.x[1]);
    std::uint64_t mask = ctx.x[2];

    // Os valores mágicos são os do Eden (`kernel/svc_types.h:97-99`).
    constexpr std::int32_t kIdealCoreDontCare   = -1;
    constexpr std::int32_t kIdealCoreUseProcess = -2;
    constexpr std::int32_t kIdealCoreNoUpdate   = -3;

    if (core_id == kIdealCoreUseProcess) {
        // O Eden usa o "ideal core" do processo. Não temos scheduler, e o
        // processo não tem ideal core próprio: o núcleo 0.
        core_id = 0;
        mask    = 1;
    } else {
        // A máscara tem de ser um subconjunto da máscara do processo.
        if ((mask | kProcessCoreMask) != kProcessCoreMask)
            return Result::InvalidCoreId;
        if (mask == 0)
            return Result::InvalidCombination;
        if (core_id >= 0 && core_id < static_cast<std::int32_t>(kVirtualCoreCount)) {
            if (((std::uint64_t{1} << core_id) & mask) == 0)
                return Result::InvalidCombination;
        } else if (core_id != kIdealCoreNoUpdate && core_id != kIdealCoreDontCare) {
            return Result::InvalidCoreId;
        }
    }

    // O handle tem de existir. São três os casos:
    //
    //   - o handle **pseudo** da thread corrente (-2), como no Eden;
    //   - o handle da thread principal, que o Eden dá na sua tabela
    //     (medido no guest: 0x81ff);
    //   - uma thread que **nosotros** criámos (`CreateThread`), que vive na
    //     nossa tabela de handles.
    //
    // O terceiro caso é o que o SDK usa a seguir a criar a thread: medido,
    // o `SetThreadCoreMask` seguinte ao `CreateThread` vem com o handle
    // da thread nova, e devolvia `InvalidHandle`.
    const std::uint32_t principal = main_thread_handle();
    const bool pseudo = (handle == kCurrentThreadHandle);
    const Handle h{static_cast<std::uint32_t>(handle)};
    const bool nossa = handles_.obter(h) != nullptr;
    if (eden_attached() && principal != 0 && handle != principal && !pseudo && !nossa)
        return Result::InvalidHandle;

    core_mask_ = mask;
    core_id_   = core_id;

    // O guest é nativo: a máscara de núcleos do Switch mapeia-se
    // directamente nos CPUs do host (núcleo virtual i -> CPU i). O jogo pediu
    // só o núcleo 0, e é isso que passa a valer.
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    for (std::uint32_t i = 0; i < kVirtualCoreCount; i++)
        if ((mask >> i) & 1u)
            CPU_SET(i, &cpus);
    if (CPU_COUNT(&cpus) != 0)
        ::sched_setaffinity(0, sizeof(cpus), &cpus);

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] SetThreadCoreMask handle=0x%llx core_id=%d mask=0x%llx\n",
                     static_cast<unsigned long long>(handle), core_id,
                     static_cast<unsigned long long>(mask));
    }

    ctx.ret      = 0;
    ctx.has_ret  = true;
    return Result::Success;
}

// GetThreadCoreMask — `SvcWrap_GetThreadCoreMask64` no Eden (svc.cpp:1346).
//
//     x0 = Result    x1 = core_id (saída)    x2 = máscara (saída)
//     x2 = handle da thread (entrada)
//
// O handle entra em x2 e a máscara sai em x2: é o que o Eden faz, e o Eden
// é a fonte. Os SVCs que devolvem valores põem-nos no registo que era o
// argumento.
Result SvcDispatcher::svc_get_thread_core_mask(SvcContext& ctx) {
    const std::uint64_t handle = ctx.x[2];

    const std::uint32_t principal = main_thread_handle();
    const bool pseudo = (handle == kCurrentThreadHandle);
    const Handle h{static_cast<std::uint32_t>(handle)};
    const bool nossa = handles_.obter(h) != nullptr;
    if (eden_attached() && principal != 0 && handle != principal && !pseudo && !nossa)
        return Result::InvalidHandle;

    ctx.x[1] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(core_id_));
    ctx.x[2] = core_mask_;
    ctx.ret      = 0;
    ctx.has_ret  = true;
    return Result::Success;
}

// GetCurrentProcessorNumber — `SvcWrap_GetCurrentProcessorNumber64` no Eden
// (svc.cpp:1364).
//
//     x0 = o número do núcleo (NÃO é um Result)
//
// O Eden põe o número do núcleo em x0 e não escreve Result nenhum. Fazer
// como o Eden: x0 é o valor, não o código de erro.
Result SvcDispatcher::svc_get_current_processor_number(SvcContext& ctx) {
    const int cpu = ::sched_getcpu();
    ctx.x[1] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cpu < 0 ? 0 : cpu));
    ctx.ret      = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cpu < 0 ? 0 : cpu));
    ctx.has_ret  = true;
    return Result::Success;
}

// CreateThread — `SvcWrap_CreateThread64` no Eden (svc.cpp:1304).
//
//     x0 = ponteiro de saída (o handle)    x1 = o handle (saída)
//     x1 = entry point   x2 = argumento   x3 = fundo da stack
//     x4 = prioridade    x5 = core_id
//
// **Este SVC matava o jogo**, do mesmo modo que o 0x0f: nao estava
// implementado, caia no `default` e devolvia `Result::Unsupported` (64 =
// 0x40), que o SDK trata como fatal. Medido: o `svc #180` foi o `0x08`, o
// `svc #182` foi o `SetTerminateResult` e o `svc #184` foi o `Break`.
//
// **STUB, e aqui esta' a razao.** A thread nao e' criada: este motor corre
// o guest NATIVAMENTE, numa thread do host, com as traps resolvidas por
// sinais — correr uma segunda thread de guest exige que o estado do
// despachante, o TLS e o `TPIDR_EL0` sejam por thread, e o despachante
// hoje tem um estado so. Criar a thread e' o passo seguinte; o que esta' aqui
// e' o que o guest precisa para nao morrer: um handle valido.
//
// A thread que o SDK cria aqui e' do proprio SDK (o entry point medido esta
// no `subsdk1`), e o jogo continua depois dela.
// ── A paragem das threads de guest no fim do run (o 139/133 do core) ──────
//
// Medido (core do 23/10, LOGX3): o fim do run (rc=139) destruiu objectos
// kernel (a via `~ServerManager → KEvent::Destroy → Unregister`) mientras
// as threads guest AINDA vivas esperavam em
// `svc_wait_process_wide_key_atomic` — e uma trapava com os handlers JÁ
// restaurados (rc=133). Os objectos vivem aqui; as funções da mecânica —
// `guest_threads_parar`, o `guest_threads_aguardar` e o predicado das
// esperas do quadro — vão mais abaixo, no quadro da arbitragem, porque
// o despertar das esperas passa pelo `g_arbiter`.
static std::atomic<bool> g_paragem_presente{false};
static std::atomic<int>  g_guests_vivos{0};
static std::mutex        g_thread_mux;
static std::vector<std::thread> g_threads;
static std::condition_variable g_thread_termiou;

Result SvcDispatcher::svc_create_thread(SvcContext& ctx) {
    const std::uint64_t out_handle = ctx.x[0];
    const std::uint64_t entry       = ctx.x[1];
    const std::uint64_t arg         = ctx.x[2];
    const std::uint64_t stack_bottom = ctx.x[3];
    const std::int32_t  priority    = static_cast<std::int32_t>(ctx.x[4]);
    const std::int32_t  core_id     = static_cast<std::int32_t>(ctx.x[5]);

    // A validacao e' a do Eden (`kernel/svc/svc_thread.cpp:26-44`), com os
    // limites do Eden (`kernel/svc_types.h:101-102`): a prioridade vai de 0
    // (a mais alta) a 63 (a mais baixa) — 64 niveis. O guest passou 0x2C
    // (44), que e' a prioridade da thread principal. Uma versao anterior
    // aceitava so 0..31 e devolvia `InvalidPriority` — o mesmo genre de
    // erro que matou o jogo com o 0x0f.
    if (priority < 0 || priority > 63)
        return Result::InvalidPriority;

    auto* to = new KObject();
    to->kind = KObject::Kind::Thread;
    to->name = "thread";
    const Handle h = handles_.reservar();
    if (!handles_.registar(h, to)) {
        delete to;
        return Result::OutOfResource;
    }

    // O handle sai no ponteiro E em x1 (o Eden faz as duas coisas:
    // `SetArg64(args, 1, out_handle)`).
    if (!write_guest(out_handle, &h.raw, sizeof h.raw))
        return Result::InvalidAddress;
    ctx.x[1]     = h.raw;
    ctx.ret      = 0;
    ctx.has_ret  = true;

    // O `CreateThread` do Horizon **nao** poe a thread a correr: so' a cria
    // e devolve o handle (`svc_thread.cpp:26-83`). Quem a poe a correr e' o
    // `StartThread` (`:86-98`, `thread->Run()`). Medido (`/tmp/boot_csrng.log`):
    // o guest cria uma thread, esta' a correr o entry point do SDK antes do
    // `StartThread`, e o `Run` dela espera num `ConditionVariable` que o
    // `StartThread` devia acordar; o `StartThread` era um stub, logo a
    // thread ficava presa e a principal tambem. Aqui so' se prepara o TLS;
    // o lancamento fica no `svc_start_thread`.
    //
    // O TLS e' uma copia do da thread principal, com o handle novo em
    // TLS+0x110 — o SDK le-o de la' para saber qual e' a thread corrente.
    if (mem_ != nullptr && tls_base_ != 0) {
        const std::uint64_t tls = tls_base_ + 0x1000ull * (++tls_threads_);
        std::vector<std::uint8_t> bloco(0x1000, 0);
        if (read_guest(tls_base_, bloco.data(), bloco.size())) {
            std::memcpy(bloco.data() + 0x110, &h.raw, sizeof h.raw);
            std::memset(bloco.data(), 0, 0x100);
            if (!write_guest(tls, bloco.data(), bloco.size())) {
                last_ipc_ = "CreateThread: nao escrevi o TLS da thread";
            } else {
                to->thread_entry = entry;
                to->thread_arg   = arg;
                to->thread_stack = stack_bottom;
                to->thread_tls   = tls;
            }
        }
    }

    // **Tracer leve de criacoes** — quem (x30) cria cada thread e com que
    // entry/arg/prio. O IA_IPC_TRACE e' pesado (despeja IPC por chamada);
    // as criacoes em si sao poucas por run (~uma dezena).
    if (std::getenv("G610_SVC_CREATE_TRACE") != nullptr)
        std::fprintf(stderr,
                     "[svc] CreateThread handle=0x%x entry=0x%llx arg=0x%llx prio=%d x30=0x%llx\n",
                     static_cast<unsigned>(h.raw), (unsigned long long)entry,
                     (unsigned long long)arg, priority,
                     static_cast<unsigned long long>(ctx.gpr[30]));

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] CreateThread handle=0x%x entry=0x%llx arg=0x%llx "
                     "stack=0x%llx prio=%d core=%d  (criada; o StartThread lanca)\n",
                     static_cast<unsigned>(h.raw), (unsigned long long)entry,
                     (unsigned long long)arg, (unsigned long long)stack_bottom,
                     priority, core_id);
    }
    return Result::Success;
}

// StartThread — `SvcWrap_StartThread64` no Eden (svc.cpp:1367).
//
//     x0 = handle da thread
//     x0 = Result
//
// E' AQUI que a thread comeca a correr. O `CreateThread` do Horizon so'
// cria o objecto e devolve o handle (`svc_thread.cpp:26-83`); o
// `StartThread` e' que chama `thread->Run()` (`:86-98`). A casa faz o
// mesmo: os argumentos ficaram guardados no `KObject` e a thread de host
// nasce agora. Lancar no `CreateThread` (o que a casa fazia) punha o
// entry point do SDK a correr antes do `Run` do proprio SDK, e o
// `ConditionVariable` do arranque da thread ficava sem quem o acordasse.
Result SvcDispatcher::svc_start_thread(SvcContext& ctx) {
    const std::uint64_t handle = ctx.x[0];
    const Handle h{static_cast<std::uint32_t>(handle)};
    const std::uint32_t principal = main_thread_handle();
    const bool pseudo = (handle == kCurrentThreadHandle);
    KObject* object = handles_.obter(h);
    const bool nossa = object != nullptr;
    if (eden_attached() && principal != 0 && handle != principal && !pseudo && !nossa)
        return Result::InvalidHandle;

    if (object != nullptr && object->kind == KObject::Kind::Thread &&
        !object->thread_started) {
        if (object->thread_tls != 0) {
            object->thread_started = true;
            const std::uint64_t entry = object->thread_entry;
            const std::uint64_t arg   = object->thread_arg;
            const std::uint64_t stack = object->thread_stack;
            const std::uint64_t tls   = object->thread_tls;
            // O objecto std::thread fica no REGISTO (g610 no quadro da
            // paragem): o fim do run precisa de o JUNTAR — o `.detach`
            // velho deixava thread de guest para sempre sem dono,
            // vivas ao ~EdenSystem (core do 139, ver o bloco da
            // `ArbitrationBoard`).
            std::thread t([entry, arg, stack, tls]() {
                g_guests_vivos.fetch_add(1, std::memory_order_release);
                NativeEngine::run_guest_thread(entry, arg, stack, tls);
                const int restam = g_guests_vivos.fetch_sub(
                    1, std::memory_order_acq_rel) - 1;
                if (restam <= 0 && guest_threads_paragem_ativa()) {
                    std::lock_guard<std::mutex> lock(g_thread_mux);
                    g_thread_termiou.notify_all();
                }
            });
            {
                std::lock_guard<std::mutex> lock(g_thread_mux);
                g_threads.push_back(std::move(t));
            }
            if (std::getenv("IA_IPC_TRACE") != nullptr) {
                std::fprintf(stderr,
                             "[svc] StartThread handle=0x%x entry=0x%llx arg=0x%llx "
                             "stack=0x%llx tls=0x%llx  (a thread corre)\n",
                             static_cast<unsigned>(h.raw),
                             (unsigned long long)entry, (unsigned long long)arg,
                             (unsigned long long)stack, (unsigned long long)tls);
            }
        } else if (std::getenv("IA_IPC_TRACE") != nullptr) {
            std::fprintf(stderr,
                         "[svc] StartThread handle=0x%x sem TLS — a thread nao corre\n",
                         static_cast<unsigned>(h.raw));
        }
    }

    ctx.ret      = 0;
    ctx.has_ret  = true;
    return Result::Success;
}

namespace {

// ── O quadro de arbitragem (passo 4) ──────────────────────────────────────
//
// Semântica medida no Eden (fonte no Pi):
//
//   ArbitrateLock(h, addr, tag) → KConditionVariable::WaitForAddress
//   (k_condition_variable.cpp:156): lê o u32 em `addr`; devolve Success
//   LOGO se não for `handle | HandleWaitMask` (svc_common.h:19:
//   HandleWaitMask = 1<<30 = 0x40000000). Só nesse estado é que a thread
//   entra na fila do endereço.
//
//   ArbitrateUnlock(addr) → KConditionVariable::SignalToAddress (:116):
//   remove o PRIMEIRO waiter; se houver mais, o próximo dono leva a
//   máscara; escreve o tag do próximo dono em `addr` — write
//   INCONDICIONAL, sem waiters escreve 0 — com fence seq_cst antes (:132),
//   e SÓ ENTÃO desperta o removido (EndWait).
//
// Aqui a fila é por endereço, com (tag, token). O `token` é o dono do
// despacho que a thread LARGA ao estacionar: é o identificador estável
// usado no despertar (tipos na ABI: o `GetAddressKeyValue` do Eden é o
// `tag`, e o despertar sai do quadro, não de uma KThread).
//
// Bloqueio REAL: como o Eden bloqueia a thread na fila do kernel, nós
// bloqueamos a thread física dentro do SVC. Sem isso as threads andam em
// loop de retry a queimar traps (a medição do passo 3: 8004 traps #26 por
// boot, todas Unsupported porque o caso não existia).
//
// Serialização: a fila é tocada por (a) o unlocker — despachando, dono em
// mãos; (b) a thread que estaciona/acorda — FORA do mundo do despacho
// enquanto espera (largou o dono). O `gate` cobre os dois, e o
// `std::map` aloca debaixo dele: natural aqui, porque o estacionamento
// acontece no caminho do SVC (a alocação no handler é padrão deste
// motor — ver o rtld), e a única reentrância possível da mesma thread é
// uma trap aninhada, excluída enquanto a thread está estacionada (não
// executa guest).

struct ArbitrationBoard {
    struct Waiter {
        std::uint32_t tag;    // valor que o UNLOCKER escreve quando me torna dono
        std::uint64_t token;  // dono do despacho que larguei; identidade do despertar
    };

    // Espera de condition variable (0x1C). O Eden — KConditionVariable::Wait
    // (k_condition_variable.cpp:271) — guarda a thread na tree com
    // (cv_key, addr, valor); `GetAddressKey`/`GetAddressKeyValue` (:202-203)
    // são exactamente o `addr` e o `tag` que SignalImpl usa.
    struct CvWaiter {
        std::uint32_t tag;    // o tag PRÓPRIO (SignalImpl escreve-o na palavra)
        std::uint64_t addr;   // endereço do mutex largado/por re-ter
        std::uint64_t token;  // dono do despacho largado
    };

    // Espera do arbiter de ENDEREÇOS (0x34/0x35). O Eden guarda-a na
    // `m_tree` própria (`KAddressArbiter`, k_address_arbiter.h:26-33) —
    // uma tree POR PROCESSO, distinta da de mutex de handle e das cv's: um
    // `SignalToAddress` NÃO acorda quem estacionou por `ArbitrateLock` nem
    // por `WaitProcessWideKeyAtomic`. As namespaces separam-se com uma
    // fila própria, por endereço; sem tag — o Unlock não escreve um tag do
    // chamador: o word é do protocolo do SDK, escrito pelo tipo de sinal.
    struct AddressWaiter {
        std::uint64_t token;  // identidade de despertar (o quadro)
    };
    std::map<std::uint64_t, std::vector<AddressWaiter>> arb_fila;

    // **Diagnóstico da paragem (runL)**: gate cronometrado + rasto do
    // dono. O `timed_mutex` deixa cada aquisição medir-se: passando de
    // 1 s, o registador quadrado imprime QUEM esperou e QUEM estava com
    // o gate em mão — o dono eterno é o último a entrar e não sair.
    std::timed_mutex gate;
    std::condition_variable_any wake;
    std::atomic<const char*> gate_dono{nullptr};
    std::map<std::uint64_t, std::vector<Waiter>> fila;

    // Fila das condition variables, por cv_key (0x1C/0x1D). Sistema próprio
    // da extensão: o Eden difunde por tree global; aqui a fila é do quadro
    // porque o despertar passa pelo token (não por KThread).
    std::map<std::uint64_t, std::vector<CvWaiter>> cv_fila;

    // Estado de despertar por token. O Eden termina a espera com
    // `thread->EndWait(result)` — quem entrega (unlocker, SignalImpl) decide
    // o resultado. Nós registamos o token quando estacionamos (com 0) e
    // quem entrega escreve o código do resultado (1 = Success, 2 = falha de
    // memória). Um único canal para as duas rotas — lock e cv acabam
    // sempre em `EndWait`.
    enum : std::uint8_t {
        DespertaAguarda   = 0,  // registado, ninguém entregou ainda
        DespertaSucesso   = 1,  // EndWait(ResultSuccess) — dono do mutex
        DespertaInvalido  = 2,  // EndWait(ResultInvalidCurrentMemory)
    };
    std::map<std::uint64_t, std::uint8_t> desperta;
};

ArbitrationBoard g_arbiter;

// ── Diagnóstico do gate (runL: 0% CPU, congelado em svc #912) ─────────────
// Toda aquisição do gate passa por aqui com um RÓTULO. Se a espera passa
// de 1 s, o [gaten] diz que thread esperou e quem tinha o gate — a
// lealdade do rasto vale mesmo com o `store` relaxado: um ponteiro
// `const char*` para literal.
static void quadro_entrar(std::unique_lock<std::timed_mutex>& lock,
                          const char* quem) {
    if (lock.try_lock_for(std::chrono::seconds(1))) {
        g_arbiter.gate_dono.store(quem, std::memory_order_relaxed);
        return;
    }
    std::fprintf(stderr,
                 "[gaten] %s: gate ocupado >1s dono=%s\n",
                 quem, g_arbiter.gate_dono.load(std::memory_order_relaxed));
    lock.lock();
    g_arbiter.gate_dono.store(quem, std::memory_order_relaxed);
}

// Bandeira do rasto de threads (a convenção dos [thrd]), fixada no
// primeiro uso — os prints de diagnóstico usam-na.
inline bool atr() {
    static const bool sim = std::getenv("IA_THREAD_TRACE") != nullptr;
    return sim;
}

// Os sinais **pendentes**: um `signal` que chegou antes do `wait`.
//
// **Medido**: o jogo perde um sinal assim — o `signal` do *main* chega
// e o *render thread* ainda não estacionou. No Eden os dois são
// serializados pelo *scheduler lock*, e o nosso quadro não os serializa
// nessa janela.
//
// A correção é a mesma que a de uma *cv* ao nível: o sinal fica
// registado, e a espera seguinte consome-o.
std::set<std::uint64_t> g_cv_pendente;

constexpr std::uint32_t kArbWaitMask = 0x40000000u;  // HandleWaitMask, svc_common.h:19

} // namespace

bool guest_threads_paragem_ativa() {
    return g_paragem_presente.load(std::memory_order_acquire);
}

void guest_threads_parar() {
    if (g_paragem_presente.exchange(true, std::memory_order_acq_rel))
        return;
    // Acorda TODAS as esperas do quadro: com a flag posta, o predicado
    // delas atinge false e a volta delas vai pelo fim da fila — o
    // handler vê a flag e encaminha para a recuperação.
    {
        std::unique_lock<std::timed_mutex> lock(g_arbiter.gate,
                                                std::defer_lock);
        quadro_entrar(lock, "stop");
        g_arbiter.wake.notify_all();
    }
}

void guest_threads_aguardar(int teto_segundos) {
    const auto prazo = std::chrono::steady_clock::now() +
                       std::chrono::seconds(teto_segundos);
    // **Drenar o vector SEMPRE** — não só quando o contador > 0. Medido
    // (LOGZ2, 23/10): o while velho (`g_guests_vivos > 0`) saía antes de
    // juntar: a thread conta o *saída* (counter→0) mas o objeto `std::
    // thread` continua JOINABLE no registo — o destrutor estático do
    // vector chamava std::terminate ("terminate called without an
    // active exception", rc=134).
    for (;;) {
        std::vector<std::thread> copia;
        {
            std::lock_guard<std::mutex> lock(g_thread_mux);
            copia.swap(g_threads);
        }
        for (auto& t : copia) {
            if (!t.joinable())
                continue;   // já juntada nesta volta ou antes
            if (std::chrono::steady_clock::now() < prazo)
                t.join();
            else {
                // Teto estourado: DESLIGAR O OBJETO JOINABLE — a thread
                // continua presa (o duelo do fim do run): no exit do
                // processo um thread detached morre sem o terminate.
                t.detach();
            }
        }
        // Os juntados/desligados NÃO voltam para o g_threads — cada ciclo
        // é fechado com o vector vazio.
        if (g_guests_vivos.load(std::memory_order_acquire) > 0) {
            // Há quem não tenha parado (ou esteja a nascer): à espera de
            // incrementos ou saídos em OUTRO ciclo; pequenas pausas até
            // ao teto: o gets_aiti devolva quando o prazo estoura — o
            // snapshot seguinte drena o objecto novo.
            if (std::chrono::steady_clock::now() >= prazo) {
                const int rem = g_guests_vivos.load(std::memory_order_acquire);
                std::fprintf(stderr,
                             "[parag] teto de %d s estourou: %d thread(s) de "
                             "guest ainda viva(s) no fim do run\n",
                             teto_segundos, rem);
                // Uma última drenagem: os objetos que ficaram no registo
                // (recém-nascidos ou voltados) SÃO desligados aqui — sem
                // terminate no exit.
                std::vector<std::thread> dreno;
                {
                    std::lock_guard<std::mutex> lock(g_thread_mux);
                    dreno.swap(g_threads);
                }
                for (auto& t : dreno)
                    if (t.joinable())
                        t.detach();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Contador a zero: TODAS pararam. Sair quando o registo está
        // drenado (cada ciclo esvazia-o).
        if (g_threads.empty())
            break;
    }
}

Result SvcDispatcher::svc_arbitrate_lock(SvcContext& ctx) {
    const std::uint32_t handle = static_cast<std::uint32_t>(ctx.x[0]);
    const std::uint64_t addr   = ctx.x[1];
    const std::uint32_t tag    = static_cast<std::uint32_t>(ctx.x[2]);

    // Eden, svc_lock.cpp: `R_UNLESS(Common::IsAligned(address, sizeof(u32)),
    // ResultInvalidAddress)`. (IsKernelAddress não se aplica aqui: não há
    // metade kernel num processo destes.)
    if (addr == 0 || (addr & 3) != 0)
        return Result::InvalidAddress;

    // O token EM CURSO desta thread (o que se larga ao estacionar e a
    // identidade de despertar). Declarado a nível da função porque o
    // pós-wait ainda o usa. thread_local: nenhum outro despacho o toca.
    std::uint64_t token = 0;
    // O resultado que o entregador escreveu no quadro (Desperta*). Nível da
    // função pelo mesmo motivo do token: usado depois do bloco de espera.
    std::uint8_t acordei_com = ArbitrationBoard::DespertaAguarda;

    // Estacionar com a corrida fechada. A LEITURA da palavra entra no
    // mesmo gate que o registo: se o dono largar ENTRE a leitura e o
    // registo, estacionar era acordar uma fila que já se esvaziou — o
    // notify perde-se e a thread dorme para sempre. Sob o gate, a ordem
    // fica: [leitura → registo → largar o dono] numa única transacção
    // perante o unlocker (que também corre sob o gate). É o papel do
    // scheduler lock global no Eden.
    {
        std::unique_lock<std::timed_mutex> lock(g_arbiter.gate,
                                                std::defer_lock);
        quadro_entrar(lock, "0x1a");

        std::uint32_t word = 0;
        if (!read_guest(addr, &word, sizeof word))
            return Result::InvalidCurrentMemory;

        // Eden WaitForAddress: "If the tag isn't the handle (with wait
        // mask), we're done." Só estaciona quando o u32 em `addr` é o dono
        // COM a máscara — o nn::os escreve `owner | 0x40000000` exactamente
        // nesse caso, e é esse estado que este SVC consome. Os outros
        // valores (0 = livre, tag puro, dono de outra thread) voltam
        // Success imediato — inclusive ACABAR de largar (0 lido aqui).
        //
        // O fast-path NÃO depende do despacho: é a volta que o Eden faz
        // sem tocar em fila nem thread — por isso o teste de unidade (fora
        // de trap) passa aqui.
        if (word != (handle | kArbWaitMask))
            return Result::Success;

        // O token EM CURSO desta thread é o que se larga ao estacionar e a
        // identidade de despertar. LER SÓ AQUI — não fica antes da
        // leitura, senão bloqueava o fast-path inteiro (o teste de unidade
        // é fora de despacho).
        token = dispatch_owner_itself();
        if (token == 0) {
            // Estacionar sem despacho em curso não é possível nesta
            // estrutura — o lock é chamado de dentro da trap. Se algum dia
            // chegar 0, é um bug a registar, não semântica a burlar.
            return Result::InvalidState;
        }

        // O despertar passa pelo QUADRO: regista-se ANTES de largar o
        // dono, e quem entrega (o unlocker) escreve o código do resultado.
        // Registo e entrega correm ambos sob o gate — sem corrida de
        // lost-wake. O velho predicado "saiu da fila" não chega ao 0x1C:
        // o SignalImpl MOVE o waiter da fila de cv para a fila do dono
        // SEM despertar (signal impl, k_condition_variable.cpp:222-236) —
        // nesse estado a thread tem de continuar a dormir, e "saiu da
        // fila" deixaria de o dizer.
        g_arbiter.desperta[token] = ArbitrationBoard::DespertaAguarda;
        g_arbiter.fila[addr].push_back({tag, token});

        // **Diagnóstico (runL)**: só pode imprimir se o lock estacionou
        // DE FATO. Imprimo o `word` LIDO, a fila deste ENDEREÇO (não a
        // global), e a fila de CV's que apontam para este mutex — se o
        // dono (word) estiver dormindo num CV, o protocolo do mutex
        // dependia DELE para desbloquear, e a cadeia é a briga.
        static std::size_t lock_n = 0;
        if (atr() && (lock_n++ < 128 || lock_n % 1000 == 0)) {
            std::size_t this_fila = 0;
            if (const auto f = g_arbiter.fila.find(addr);
                f != g_arbiter.fila.end())
                this_fila = f->second.size();
            std::uint16_t cv_esperando = 0;
            for (const auto& [k, fila] : g_arbiter.cv_fila)
                for (const auto& w : fila)
                    if (w.addr == addr)
                        cv_esperando++;
            std::fprintf(stderr,
                         "[thrd] LPAUSA token=0x%llx addr=0x%llx tag=0x%x "
                         "word=0x%x fila_endereco=%zu cvs_apontando=%u\n",
                         static_cast<unsigned long long>(token),
                         static_cast<unsigned long long>(addr), tag, word,
                         this_fila, cv_esperando);
            // **A vizinhança do mutex** (o `0x20000` do runCAS0/1 não sai
            // de nenhum print — o oeste e o leste da palavra podem dizer
            // DE ONDE ele veio: o SDK enche a estrutura à volta).
            if (std::getenv("IA_WORD_TRACE") != nullptr) {
                for (std::uint64_t o = 0x40; o >= 4; o -= 4) {
                    std::uint32_t v = 0;
                    if (read_guest(addr - o, &v, sizeof v))
                        std::fprintf(stderr,
                                     "  [viz] -0x%02llx 0x%llx = 0x%08x\n",
                                     static_cast<unsigned long long>(o),
                                     static_cast<unsigned long long>(addr - o),
                                     v);
                }
                for (std::uint64_t o = 4; o <= 0x40; o += 4) {
                    std::uint32_t v = 0;
                    if (read_guest(addr + o, &v, sizeof v))
                        std::fprintf(stderr,
                                     "  [viz] +0x%02llx 0x%llx = 0x%08x\n",
                                     static_cast<unsigned long long>(o),
                                     static_cast<unsigned long long>(addr + o),
                                     v);
                }
            }
        }

        dispatch_owner_vacate();
        g_arbiter.wake.wait(lock, [&] {
            if (guest_threads_paragem_ativa())
                return true;
            const auto d = g_arbiter.desperta.find(token);
            return d != g_arbiter.desperta.end() &&
                   d->second != ArbitrationBoard::DespertaAguarda;
        });
        // (a paragem acorda as duas esperas; sair sem `.at` — ver a
        // nota na rota da cv.)
        if (guest_threads_paragem_ativa()) {
            g_arbiter.desperta.erase(token);
            return Result::TimedOut;
        }
        if (atr() && (lock_n < 128 || lock_n % 1000 == 0))
            std::fprintf(stderr,
                         "[thrd] LACORDEI token=0x%llx acordei=%d\n",
                         static_cast<unsigned long long>(token),
                         static_cast<int>(acordei_com));
        acordei_com = g_arbiter.desperta.at(token);
        g_arbiter.desperta.erase(token);
    }

    // O unlocker escreveu o NOVO dono (o meu tag) na memória do guest
    // ANTES de despertar (Eden: WriteToUser, depois EndWait) — o resultado
    // na memória já está. Voltar a tomar o despacho para continuar; se o
    // teto de spin expirar (dono agarrado por muito tempo), a thread segue
    // na mesma: o guest já É dono do mutex, e a janela sem dono é o preço
    // do caso degenerado, não uma rota de erro.
    dispatch_owner_set(token);
    (void)dispatch_owner_claim();
    // Falha de memória entregue pelo entregador (não existe no lock puro,
    // mas o canal é comum): devolve o que o Eden devolveria.
    if (acordei_com != ArbitrationBoard::DespertaSucesso)
        return Result::InvalidCurrentMemory;
    return Result::Success;
}

Result SvcDispatcher::svc_arbitrate_unlock(SvcContext& ctx) {
    const std::uint64_t addr = ctx.x[0];
    if (addr == 0 || (addr & 3) != 0)
        return Result::InvalidAddress;

    std::uint32_t next    = 0;
    std::uint64_t waker   = 0;

    {
        std::unique_lock<std::timed_mutex> lock(g_arbiter.gate,
                                                std::defer_lock);
        quadro_entrar(lock, "0x1b");
        // A palavra ATUAL entra-fed para o CAS final (o Eden :132-137).
        std::uint32_t palavra = 0;
        if (!read_guest(addr, &palavra, sizeof palavra))
            return Result::InvalidCurrentMemory;
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            const auto it = g_arbiter.fila.find(addr);
            const std::size_t na_fila =
                it == g_arbiter.fila.end() ? 0 : it->second.size();
            std::fprintf(stderr, "[arb] unlock addr=0x%llx palavra=0x%x na_fila=%zu\n",
                         static_cast<unsigned long long>(addr), palavra, na_fila);
        }
        const auto it = g_arbiter.fila.find(addr);
        if (it != g_arbiter.fila.end() && !it->second.empty()) {
            const ArbitrationBoard::Waiter w = it->second.front();
            it->second.erase(it->second.begin());
            if (it->second.empty())
                g_arbiter.fila.erase(it);
            // Eden :122: `if (has_waiters) next_value |= Svc::HandleWaitMask`
            const bool more_waiters = g_arbiter.fila.count(addr) != 0;
            next  = w.tag | (more_waiters ? kArbWaitMask : 0u);
            waker = w.token;
            // O despertar sai do quadro agora: `EndWait(ResultSuccess)`.
            // O waiter lê isto no seu predicado e verifica que saiu da fila.
            g_arbiter.desperta[waker] = ArbitrationBoard::DespertaSucesso;
            // **Diagnóstico (runL)**: cada transferencia de dono pelo
            // unlock — a corrente que faz o mutex andar.
            if (atr())
                std::fprintf(stderr,
                             "[thrd] DLOCK waker=0x%llx addr=0x%llx next=0x%x "
                             "mais_waiters=%d\n",
                             static_cast<unsigned long long>(waker),
                             static_cast<unsigned long long>(addr), next,
                             more_waiters ? 1 : 0);
        }
        // Eden :132-137: fence seq_cst antes do write; e o write é
        // INCONDICIONAL — unlock sem waiters escreve 0 (cabeça livre). O
        // rtld/jogo leem isto via CAS rápida própria no próximo lock.
        //
        // **A escrita é CAS-guardada** (a lição da runL): a troca cega
        // pode sobrescrever uma mudança que o loop LL/SC do guest fez no
        // meio — sobrevivência de dono fantasma. O `next` é a decisão do
        // quadro; a re-leitura interna repõe o valor visto.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!escrita_atomica_word(addr, next))
            return Result::InvalidCurrentMemory;
    }

    // Despertar FORA do gate, e SÓ até quem saiu da fila. (notify_all e os
    // outros endereços voltam a dormir pelo predicado — barato com 2-4
    // threads, e mantém o quadro sem cond por endereço.)
    if (waker != 0)
        g_arbiter.wake.notify_all();

    return Result::Success;
}

// ── 0x1C: WaitProcessWideKeyAtomic ────────────────────────────────────────
//
// Medida no Eden do Orange Pi, svc_condition_variable.cpp:18-48 →
// KConditionVariable::Wait (k_condition_variable.cpp:271). Dentro do
// scheduler lock:
//
//   1. "Remove waiter thread" (:285-292): o PRIMEIRO waiter estacionado
//      no endereço do MUTEX sai da fila e torna-se o próximo dono —
//      next_value = tag dele (+ máscara se houver mais), `EndWait` dele;
//   2. escreve 1 (HasWaiterFlag) na PALAVRA do cv_key, fence seq_cst
//      (:304-311) — o cv_key é um ENDEREÇO no espaço do guest;
//   3. escreve next_value no endereço do mutex (:313-317);
//   4. timeout == 0 → ResultTimedOut, DEPOIS do release (:319);
//   5. timeout != 0: marca (addr, cv_key, tag) na tree e dorme (:324-331) —
//      o despertar é decidido pelo SignalImpl (0x1D), que transfere o
//      mutex ANTES de acordar a thread: `Success` devolve o mutex já em
//      mão. Nada de re-lock no caminho de despertar do Eden.
//
// timeout < 0 é espera sem fim (o jogo passa -1: trap #28 mediu
// x3=0xffffffffffffffff); timeout > 0 é prazo em ns — aqui sobre o
// relógio monotónico do host (o Eden converte ns→ticks no svc layer;
// a conversão duplicada não muda a semântica do prazo).
Result SvcDispatcher::svc_wait_process_wide_key_atomic(SvcContext& ctx) {
    const std::uint64_t addr    = ctx.x[0];
    const std::uint64_t cv_key  = ctx.x[1] & ~std::uint64_t{3};  // AlignDown (svc :54)
    const std::uint32_t tag     = static_cast<std::uint32_t>(ctx.x[2]);
    const std::int64_t  timeout = static_cast<std::int64_t>(ctx.x[3]);

    // **Instrumento leve — Parede 2**: a evolucao do predicate da parede.
    // `G610_CV_DUMP=base,key` (ex.: `0xd9c2c200,0xd9c2c22c`): a cada wait
    // da chave `key`, le 0x30 bytes em `base` e despeja-as. As primeiras
    // 24 esperas e depois 1 em cada 128 — o laco repete a ~5 ms e o log
    // nao inunda (medido na LOG5: 729 esperas numa corrida curta).
    static const char* dump_cfg = std::getenv("G610_CV_DUMP");
    if (dump_cfg != nullptr) {
        std::uint64_t dump_base = 0;
        std::uint64_t dump_key = 0;
        {
            char* fim = nullptr;
            dump_base = std::strtoull(dump_cfg, &fim, 16);
            if (fim != nullptr && *fim == ',')
                dump_key = std::strtoull(fim + 1, nullptr, 16);
        }
        static std::size_t dump_n = 0;
        if (dump_base != 0 && cv_key == dump_key) {
            ++dump_n;
            if (dump_n <= 24 || dump_n % 128 == 0) {
                std::uint8_t janela[0x30] = {};
                const bool lido =
                    read_guest(dump_base, janela, sizeof janela);
                std::fprintf(stderr,
                             "[svc] cv-dump n=%zu cv=0x%llx base=0x%llx:",
                             dump_n,
                             static_cast<unsigned long long>(cv_key),
                             static_cast<unsigned long long>(dump_base));
                if (lido) {
                    for (std::size_t i = 0; i < sizeof janela; i += 4) {
                        std::uint32_t d = 0;
                        std::memcpy(&d, janela + i, sizeof d);
                        std::fprintf(stderr, " %08X", d);
                    }
                    std::fprintf(stderr, "\n");
                } else {
                    std::fprintf(stderr, " NAO_LIDO\n");
                }
            }
        }
    }

    // **Cadeia de chamados do lado da espera** (env `G610_SVC_WAIT_FP=1`):
    // o mesmo esquema do `G610_SVC_SIGNAL_FP`: [x29] = fp do chamador,
    // [x29+8] = lr; sobe tres niveis. Barato (3 leituras de 16 bytes por
    // espera) e so' com o env posto. Medida LOG15: a lane da parede
    // (0xd9c2c22c, ~5 ms) sobe para `WaitTimerEvent` no sdk e para
    // `main+0x65A630` — quem chama e' o "armador do re-espera"; a cadeia
    // distingue a lane da parede da lane da bomba (que espera ∞).
    static const bool trace_wait_fp = std::getenv("G610_SVC_WAIT_FP") != nullptr;
    if (trace_wait_fp) {
        constexpr std::uint64_t largura = 0x200000ull;
        constexpr std::uint64_t topo = 0x800000000ull;
        const std::uint64_t fp = ctx.gpr[29];
        std::uint64_t f[6] = {0, 0, 0, 0, 0, 0};
        const bool ok = fp >= largura && fp < topo &&
                        read_guest(fp, f, 2 * sizeof f[0]);
        if (ok && f[0] >= largura && f[0] < topo)
            (void)read_guest(f[0], f + 2, 2 * sizeof f[0]);
        if (ok && f[2] >= largura && f[2] < topo)
            (void)read_guest(f[2], f + 4, 2 * sizeof f[0]);
        std::fprintf(stderr,
                     "[svc] WaitProcessWideKeyAtomic cadeia cv=0x%llx tag=0x%x "
                     "x30=0x%llx lvl1=0x%llx lvl2=0x%llx lvl3=0x%llx\n",
                     static_cast<unsigned long long>(cv_key), tag,
                     static_cast<unsigned long long>(ctx.gpr[30]),
                     static_cast<unsigned long long>(f[1]),
                     static_cast<unsigned long long>(f[3]),
                     static_cast<unsigned long long>(f[5]));
    }

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] WaitProcessWideKeyAtomic addr=0x%llx cv=0x%llx tag=0x%x "
                     "timeout=%lld\n",
                     static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(cv_key), tag,
                     static_cast<long long>(timeout));

        // **Onde está o guest.** O `x30` é o endereço de retorno — ou
        // seja, o PC do guest que está a esperar. Sem símbolos, é o
        // único way de saber *que* caminho do SDK está bloqueado.
        std::fprintf(stderr, "  [x30=0x%llx]", static_cast<unsigned long long>(ctx.gpr[30]));
        // A stack do guest: o SDK guarda o estado do objecto de
        // sincronização num slot da stack.
        const std::uint64_t sp = ctx.sp;
        for (std::uint64_t off = 0; off < 0x80; off += 8) {
            std::uint64_t v = 0;
            if (read_guest(sp + off, &v, sizeof v))
                std::fprintf(stderr, " [sp+%llx=0x%llx]",
                             static_cast<unsigned long long>(off),
                             static_cast<unsigned long long>(v));
        }
        // E o **conteúdo do mutex** e da cv — o que o guest espera que
        // mude.
        std::uint32_t v_addr = 0;
        std::uint32_t v_cv = 0;
        (void)read_guest(addr, &v_addr, sizeof v_addr);
        (void)read_guest(cv_key, &v_cv, sizeof v_cv);
        std::fprintf(stderr, " [mutex=0x%x cv=0x%x]\n", v_addr, v_cv);

        // **O código do guest** — o PC e a cadeia de chamadas. Sem
        // símbolos, os bytes são a única forma de saber que caminho do
        // SDK está bloqueado.
        auto despejar_codigo = [this](std::uint64_t pc) {
            std::uint8_t bytes[48] = {0};
            if (!read_guest(pc, bytes, sizeof bytes))
                return;
            std::fprintf(stderr, "  [cod 0x%llx]:", static_cast<unsigned long long>(pc));
            for (std::uint8_t b : bytes)
                std::fprintf(stderr, " %02x", b);
            std::fprintf(stderr, "\n");
        };
        despejar_codigo(ctx.gpr[30]);
        // A cadeia: os valores da stack que são endereços do módulo
        // principal (0x80000000..0x84000000).
        for (std::uint64_t off = 0; off < 0x80; off += 8) {
            std::uint64_t v = 0;
            if (!read_guest(sp + off, &v, sizeof v))
                continue;
            if (v >= 0x80000000ull && v < 0x84000000ull && v != ctx.gpr[30])
                despejar_codigo(v);
        }
    }

    // **A espera verdadeira, no modelo do ArbitrateLock/WaitForAddress** —
    // a thread física PARKA dentro do SVC e larga o dono do despacho.
    //
    // O que este SVC faz no Eden (KConditionVariable::Wait,
    // k_condition_variable.cpp:271-331), tudo sob o scheduler lock —
    // aqui, sob o `g_arbiter.gate`:
    //
    //   1. sinal pendente (`g_cv_pendente`): consumido antes — o `signal`
    //      que chegou quando ninguém estava registado é desta fila;
    //   2. "Remove waiter thread" (:285-292): o PRIMEIRO waiter
    //      estacionado no endereço do MUTEX sai da fila e torna-se o
    //      próximo dono — next_value = tag dele (+ máscara se houver
    //      mais), `EndWait(Success)` dele;
    //   3. escreve 1 (HasWaiterFlag) na PALAVRA do cv_key, fence seq_cst
    //      (:304-311);
    //   4. escreve next_value no endereço do mutex (:313-317);
    //   5. timeout == 0 → ResultTimedOut DEPOIS do handshake (:319),
    //      SEM estacionar;
    //   6. timeout != 0: regista (tag, addr, token) na fila do cv e dorme
    //      (:324-331) — o despertar é do SignalImpl (0x1D), que transfere
    //      o mutex ANTES de acordar (o `Success` devolve o mutex já em
    //      mão). Nada de re-lock no caminho de despertar.
    //
    // **Identidade**: `read_thread_pointer()` NÃO serve o despacho — o
    // trampolim troca o TPIDR_EL0 para o do host ANTES de chamar o
    // handler, por isso `find_guest_thread_state` nunca casava (medido:
    // BLOCK=0/SWITCH=0 em toda a corrida — as lanes rodavam em user-space
    // a queimar traps sem nunca bloquear). O token em curso
    // (`dispatch_owner_itself()`) é a identidade das outras esperas do
    // quadro — e é o que se larga ao estacionar.
    //
    // **O que NÃO se faz aqui**: não se guarda gpr/pc e não se marca
    // `waiting_on_cv`/`g_switch_to_tls` — a troca coopérativa por contexto
    // salvo REEXECUTARIA código de guest depois do estacionamento (a
    // thread fotográfica estacionada é VIVA: acorda pelo quadro). O
    // `mark_ready_threads_on_cv` do sinal continua, e encontra ZERO
    // thread com `waiting_on_cv` — os estacionados são por con-di-var,
    // não pela troca.

    // O resultado que o entregador escreveu no quadro (Desperta*). Nível
    // da função porque o pós-wait o usa.
    std::uint8_t acordei_com = ArbitrationBoard::DespertaAguarda;
    std::uint64_t token = 0;
    // Quem levou o mutex NESTE Wait (a fila do mutex tinha alguém): acorda
    // FORA do gate, como o `ArbitrateUnlock` faz.
    std::vector<std::uint64_t> wakers;

    {
        std::unique_lock<std::timed_mutex> lock(g_arbiter.gate,
                                                std::defer_lock);
        quadro_entrar(lock, "0x1c");

        // **Sinal pendente** (o `signal` chegou antes do `wait`): consome
        // e devolve Success — o mesmo acordo que os runs velhos tinham.
        // (Sob o gate: qualquer `signal` posterior já nos encontra
        // registados na fila — sem janela de sinal perdido entre o teste
        // e o registo.)
        {
            const auto it = g_cv_pendente.find(cv_key);
            if (it != g_cv_pendente.end()) {
                g_cv_pendente.erase(it);
                std::fprintf(stderr,
                             "[svc] WaitProcessWideKeyAtomic: sinal pendente na cv 0x%llx\n",
                             static_cast<unsigned long long>(cv_key));
                ctx.ret     = 0;
                ctx.has_ret = true;
                ctx.x[1]    = 0;
                return Result::Success;
            }
        }

        // Estacionar sem despacho em curso não é possível nesta estrutura —
        // o wait chega de dentro da trap. Se algum dia chegar 0, é bug a
        // registar, não semântica a burlar.
        token = dispatch_owner_itself();
        if (token == 0)
            return Result::InvalidState;

        // O despertar passa pelo QUADRO: regista-se ANTES de largar o
        // dono, e quem entrega (o SignalImpl) escreve o código do
        // resultado. Registo e entrega correm ambos sob o gate — sem
        // corrida de lost-wake (é o papel do scheduler lock no Eden).
        g_arbiter.desperta[token] = ArbitrationBoard::DespertaAguarda;

        // Registo na fila do cv — a MESMA entrada que o SignalImpl capita
        // e pova. Para timeout == 0 não há registo (upstream :324: a tree
        // só recebe quem dorme).
        const bool dorme = timeout != 0;
        if (dorme)
            g_arbiter.cv_fila[cv_key].push_back({tag, addr, token});

        // ── "Remove waiter thread" — a fila do MUTEX ─────────────────
        // O PRIMEIRO waiter do mutex torna-se o próximo dono; fila vazia
        // ⇒ a palavra passa a 0 (cabeça livre).
        std::uint32_t next_value = 0;
        {
            const auto it = g_arbiter.fila.find(addr);
            if (it != g_arbiter.fila.end() && !it->second.empty()) {
                const ArbitrationBoard::Waiter w = it->second.front();
                it->second.erase(it->second.begin());
                if (it->second.empty())
                    g_arbiter.fila.erase(it);
                // Eden :122 — `if (has_waiters) next_value |= HandleWaitMask`.
                const bool mais = g_arbiter.fila.count(addr) != 0;
                next_value = w.tag | (mais ? kArbWaitMask : 0u);
                // `EndWait(ResultSuccess)` do novo dono.
                g_arbiter.desperta[w.token] =
                    ArbitrationBoard::DespertaSucesso;
                wakers.push_back(w.token);
            }
        }

        // **A entrega vai COM o despertar.** O waiter popado já saiu da
        // fila e, no Eden, EndWait dele acontece DENTRO do scheduler
        // lock (a thread já fica pronta). Aqui o notify saía só DEPOIS
        // do MEU próprio park — e se eu (A) estacionar antes de um
        // desperta meu que só o waiter entregue pode produzir, a
        // lógica trava: A parqueia, B entregue (`desperta[B]=1`) dorme
        // sem notify — ninguém unlock, ninguém sinaliza (medida
        // eden_arb.log tick 1-5: word=0x81ff, desperta[0x1]=1, fila=0,
        // o produtor parou). O notify sob o gate repete o padrão do
        // `ArbitrateUnlock` (DLOCK: desperta + notify) e do
        // `guest_threads_parar`; quem for acordado sem ter o que
        // fazer apenas re-avalia o predicado e volta a dormir.
        if (!wakers.empty())
            g_arbiter.wake.notify_all();

        // ── HasWaiterFlag na palavra do cv_key ────────────────────────
        bool estacionado = true;
        {
            const std::uint32_t um = 1;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (!escrita_atomica_word(cv_key, um)) {
                // A palavra não está mapeada: a espera não se regista no
                // guest — o resultado é o par do SignalImpl :247
                // (`EndWait(ResultInvalidCurrentMemory)`).
                g_arbiter.desperta[token] =
                    ArbitrationBoard::DespertaInvalido;
                estacionado = false;
            }
        }

        // ── next_value no endereço do mutex ──────────────────────────
        // (O caminho do unlock do Eden escreve com o mesmo fence — a
        // ordem palavra→desperto é o que o protocolo do SDK lê.)
        // **CAS-guardada**: o dono que se entrega tem de CHUMBAR a troca
        // que o próprio SDK fez no meio (a lição da runL).
        {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            (void)escrita_atomica_word(addr, next_value);
        }

        static std::size_t park_n = 0;
        if (std::getenv("IA_THREAD_TRACE") != nullptr &&
            (park_n++ < 64 || park_n % 1000 == 0)) {
            std::fprintf(stderr,
                         "[thrd] PARK token=0x%llx cv=0x%llx sp=0x%llx pc=0x%llx "
                         "tag=0x%x addr=0x%llx timeout=%lld\n",
                         static_cast<unsigned long long>(token),
                         static_cast<unsigned long long>(cv_key),
                         static_cast<unsigned long long>(ctx.sp),
                         static_cast<unsigned long long>(ctx.gpr[30]),
                         tag,
                         static_cast<unsigned long long>(addr),
                         static_cast<long long>(timeout));
        }

        if (timeout == 0) {
            // Eden :319 — ResultTimedOut DEPOIS do release, sem
            // estacionar. O despacho JÁ é desta thread (nunca largado
            // neste caminho): devolver directo.
            g_arbiter.desperta.erase(token);
            lock.unlock();
            if (!wakers.empty())
                g_arbiter.wake.notify_all();
            return Result::TimedOut;
        }

        if (estacionado) {
            dispatch_owner_vacate();
            const auto dono_acordou = [&] {
                if (guest_threads_paragem_ativa())
                    return true;
                const auto d = g_arbiter.desperta.find(token);
                return d != g_arbiter.desperta.end() &&
                       d->second != ArbitrationBoard::DespertaAguarda;
            };

            if (timeout > 0) {
                // HOS: prazo relativo em nanosegundos. (Tampa longe do
                // overflow do relógio; ninguém espera 10 dias.)
                const std::int64_t teto = timeout < (1LL << 56)
                                              ? timeout : (1LL << 56);
                const auto prazo = std::chrono::steady_clock::now() +
                                   std::chrono::nanoseconds(teto);
                if (!g_arbiter.wake.wait_until(lock, prazo, dono_acordou)) {
                    // Prazo estourou. A entrega pode ter acontecido no
                    // MESMO instante (notify contra o prazo): o quadro é a
                    // verdade — preferir o resultado REAL da entrega, um
                    // `TimedOut` aqui estragava o protocolo do mutex (o
                    // sinalizador já lá escreveu o nosso tag).
                    const auto d = g_arbiter.desperta.find(token);
                    if (d == g_arbiter.desperta.end() ||
                        d->second == ArbitrationBoard::DespertaAguarda) {
                        // Timeout verdadeiro: sair da fila do cv — a tree
                        // do Eden desmonta a espera no cancelamento do
                        // timer; aqui a remoção é por token.
                        if (auto cv = g_arbiter.cv_fila.find(cv_key);
                            cv != g_arbiter.cv_fila.end()) {
                            auto& fila = cv->second;
                            for (auto w = fila.begin(); w != fila.end(); ++w)
                                if (w->token == token) {
                                    fila.erase(w);
                                    break;
                                }
                            if (fila.empty())
                                g_arbiter.cv_fila.erase(cv);
                        }
                        g_arbiter.desperta.erase(token);
                        lock.unlock();
                        if (!wakers.empty())
                            g_arbiter.wake.notify_all();
                        dispatch_owner_set(token);
                        (void)dispatch_owner_claim();
                        return Result::TimedOut;
                    }
                    // Entrega no instante do prazo: seguir como entrega
                    // (abaixo). NÃO sair da fila — o sinalizador já nos
                    // povou fora dela.
                }
            } else {
                // timeout negativo: espera SEM FIM, como a ABI manda e a
                // cv da parede passa (`x3 = 0xffffffffffffffff`). O
                // `wait` recebe o predicado: a paragem (o fim de run) e a
                // entrega ambos acordam.
                g_arbiter.wake.wait(lock, dono_acordou);
            }
        }

        // A paragem pode ser o que acordou (o fim de run) — como no Lock:
        // sair limpo, a recuperação segue no próximo despacho.
        if (guest_threads_paragem_ativa()) {
            if (auto cv = g_arbiter.cv_fila.find(cv_key);
                cv != g_arbiter.cv_fila.end()) {
                auto& fila = cv->second;
                for (auto w = fila.begin(); w != fila.end(); ++w)
                    if (w->token == token) {
                        fila.erase(w);
                        break;
                    }
                if (fila.empty())
                    g_arbiter.cv_fila.erase(cv);
            }
            g_arbiter.desperta.erase(token);
            lock.unlock();
            if (!wakers.empty())
                g_arbiter.wake.notify_all();
            dispatch_owner_set(token);
            (void)dispatch_owner_claim();
            return Result::TimedOut;
        }

        if (atr() && (park_n < 128 || park_n % 1000 == 0))
            std::fprintf(stderr,
                         "[thrd] ACORDEI token=0x%llx cv=0x%llx acordei=%d\n",
                         static_cast<unsigned long long>(token),
                         static_cast<unsigned long long>(cv_key),
                         static_cast<int>(acordei_com));
        acordei_com = g_arbiter.desperta.at(token);
        g_arbiter.desperta.erase(token);
    }

    // Voltar a tomar o despacho para terminar o SVC — idêntico ao Lock: se
    // o teto de spin expirar (dono agarrado por muito tempo), a thread
    // segue na mesma: a janela sem dono é o preço do caso degenerado.
    dispatch_owner_set(token);
    (void)dispatch_owner_claim();
    // O novo dono do mutex estacionado acorda FORA do gate.
    if (!wakers.empty())
        g_arbiter.wake.notify_all();

    // Falha de memória entregue pelo entregador: o Eden devolve
    // InvalidCurrentMemory. `Success` devolve o mutex JÁ em mão — o
    // sinalizador escreveu o nosso tag na palavra ANTES de despertar
    // (Eden: WriteToUser, depois EndWait), nada a re-escrever aqui.
    if (acordei_com != ArbitrationBoard::DespertaSucesso)
        return Result::InvalidCurrentMemory;
    ctx.ret     = 0;
    ctx.has_ret = true;
    ctx.x[1]    = 0;
    return Result::Success;
}

// ── 0x34: WaitForAddress ──────────────────────────────────────────────────
//
// Eden: `svc_wait_for_address` (svc/svc_address_arbiter.cpp:44-76) →
// `KAddressArbiter::WaitIfLessThan/WaitIfEqual` (k_address_arbiter.cpp:242-322).
//
// **E' o SVC no quadro do abort da runL** (`Break(payload 0x40)` com o
// `sdk 0x83ab1dc4` no fim da cadeia): o `SdkMutexType::Lock` do nn::os —
// medido no módulo (0x83ab1c60; wrapper svc `0x83b0bc34`) — chama
// `WaitForAddress(&word, WaitIfEqual=2, value=1, timeout)` e ACEITA SÓ
// `Success` e `InvalidState` (0xFA01, prog. 0x83ab1d64); qualquer outro
// Result — incluído o `Unsupported` que o motor devolvia — vai para
// `SetTerminateResult` + `Break`. O timeout do caminho do mutex é sempre
// `-1` (`x20 = #-1`, prog. 0x83ab1c84); timeouts finitos existem noutros
// chamadores do nn::os e são respeitados abaixo.
//
// Semântica por tipo (`ArbitrationType`, svc_types.h:73-77):
//   0 WaitIfLessThan             — lê s32; word >= value ⇒ InvalidState;
//   1 DecrementAndWaitIfLessThan — lê s32; word < value ⇒ DECREMENTA
//                                  (RMW atómico, o monitor é o hardware)
//                                  e espera; senão InvalidState;
//   2 WaitIfEqual                — lê s32; word != value ⇒ InvalidState.
// Validado ⇒ estaciona na fila do ENDEREÇO até um SignalToAddress (0x35)
// — o Eden, dentro do scheduler lock, lê, valida e insere na tree numa
// janela só; aqui a janela é o `g_arbiter.gate`, e a LEITURA entra nela:
// fora dela, cai a corrida "sinal perdido" já medida na cv (o 0x35
// escreve o word e desperta sob o MESMO gate).
Result SvcDispatcher::svc_wait_for_address(SvcContext& ctx) {
    const std::uint64_t addr    = ctx.x[0];
    const std::uint32_t tipo    = static_cast<std::uint32_t>(ctx.x[1]);
    const std::int32_t  valor   = static_cast<std::int32_t>(ctx.x[2]);
    const std::int64_t  timeout = static_cast<std::int64_t>(ctx.x[3]);

    // Eden :51-53 — alinhamento sizeof(s32) → InvalidAddress; enum →
    // InvalidEnumValue. (IsKernelAddress não se aplica: o processo não
    // cruza a metade kernel.)
    if (addr == 0 || (addr & 3) != 0)
        return Result::InvalidAddress;
    if (tipo > 2)
        return Result::InvalidEnumValue;

    // O token EM CURSO (= o do ArbitrateLock): é o que se larga ao
    // estacionar e a identidade de despertar do quadro.
    const std::uint64_t token = dispatch_owner_itself();

    // **A janela atómica de leitura**: sob o gate — ver cabeçalho.
    std::unique_lock<std::timed_mutex> lock(g_arbiter.gate, std::defer_lock);
    quadro_entrar(lock, "0x34");

    std::uint32_t word = 0;
    if (!read_guest(addr, &word, sizeof word))
        return Result::InvalidCurrentMemory;  // Eden :57 R_THROW

    bool esperar = false;
    if (tipo == 1) {
        // DecrementAndWaitIfLessThan — Eden `DecrementIfLessThan`
        // (k_address_arbiter.cpp:41-69): exclusive-read, se < value
        // exclusive-store value-1, repetindo o par até alcançar.
        // O monitor exclusivo é o HARDWARE (o guest corre nativo); do
        // lado de cá o par load/store exclusivo equivalente é um
        // compare-exchange no byte do guest — a reserva é memória do
        // host, e `host_pointer` é o `GetPointer` do Eden.
        if (mem_ == nullptr)
            return Result::InvalidCurrentMemory;
        std::uint8_t* p = mem_->host_pointer(addr, sizeof word);
        if (p == nullptr)
            return Result::InvalidCurrentMemory;
        for (;;) {
            std::uint32_t bruto = 0;
            std::memcpy(&bruto, p, sizeof bruto);
            const std::int32_t atual = static_cast<std::int32_t>(bruto);
            if (atual >= valor) {
                // Eden :55-63 — ClearExclusive + *out = current; o teste
                // `user_value >= value` manda InvalidState (o mesmo teste
                // de WaitIfLessThan).
                esperar = false;
                break;
            }
            const std::uint32_t novo =
                static_cast<std::uint32_t>(atual - 1);
            std::uint32_t esperado = bruto;  // mutável: o CAS escreve lá o
                                             // valor visto em caso de falha
            if (__atomic_compare_exchange_n(
                    p, &esperado, novo, false,
                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                esperar = true;
                break;
            }
            // Perdeu o exclusive: o Eden repete o conjugado inteiro —
            // re-ler e re-verificar (o word pode ter passado o valor).
        }
    } else if (tipo == 2) {
        // WaitIfEqual — Eden :364-371: ler sob o lock e testar a
        // igualdade EXACTA.
        esperar = static_cast<std::int32_t>(word) == valor;
    } else {
        // WaitIfLessThan — Eden :305-311 (sem decremento; só lê).
        esperar = static_cast<std::int32_t>(word) < valor;
    }

    if (!esperar) {
        // Eden :309/:373 — R_THROW(ResultInvalidState) imediatamente, sem
        // estacionar. É o Result que o SDK TOLERA (0xFA01): "verificámos,
        // não calha".
        return Result::InvalidState;
    }
    if (timeout == 0) {
        // Eden :319/:381 — R_THROW(ResultTimedOut) DEPOIS do teste (o
        // pedido é "esperar, mas com prazo zero").
        return Result::TimedOut;
    }

    // ── Estacionar: Eden :32-34 — `SetAddressArbiter` + `m_tree.insert`,
    // e dormir. O despertar é do 0x35.
    g_arbiter.arb_fila[addr].push_back({token});
    g_arbiter.desperta[token] = ArbitrationBoard::DespertaAguarda;
    dispatch_owner_vacate();

    const auto dono_acordou = [&] {
        if (guest_threads_paragem_ativa())
            return true;
        const auto d = g_arbiter.desperta.find(token);
        return d != g_arbiter.desperta.end() &&
               d->second != ArbitrationBoard::DespertaAguarda;
    };

    if (timeout > 0) {
        const auto prazo = std::chrono::steady_clock::now() +
                           std::chrono::nanoseconds(timeout);
        if (!g_arbiter.wake.wait_until(lock, prazo, dono_acordou)) {
            // Prazo estourou: sair da fila do ENDEREÇO (a tree do Eden
            // desmonta a espera no cancel do timer; aqui a remoção é por
            // token — o sinal pode já ter registado entrega, mas o woken
            // não reclamou: o `.at(token)` iria deitar fora a entrega e
            // o signaler perde um waiter contado. O Eden resolve por o
            // `EndWait` acontecer em UM lado só; nós largamos a entrega —
            // o waiter sai TimedOut e re-avalia o seu predicado.
            if (auto arb = g_arbiter.arb_fila.find(addr);
                arb != g_arbiter.arb_fila.end()) {
                auto& fila = arb->second;
                for (auto w = fila.begin(); w != fila.end(); ++w)
                    if (w->token == token) { fila.erase(w); break; }
                if (fila.empty())
                    g_arbiter.arb_fila.erase(arb);
            }
            g_arbiter.desperta.erase(token);
            dispatch_owner_set(token);
            (void)dispatch_owner_claim();
            return Result::TimedOut;
        }
    } else {
        // Timeout negativo: espera SEM FIM, como a ABI manda e o SDK
        // passa (`x20 = #-1`). O `wait` recebe o predicado: a paragem
        // (o fim de run) e a entrega ambos acordam.
        g_arbiter.wake.wait(lock, dono_acordou);
    }

    // A paragem pode ser o que acordou (predicado ✓ sem entrega): o fim
    // de run. Sair limpo — o handler re-encaminha para a recuperação.
    if (guest_threads_paragem_ativa()) {
        g_arbiter.desperta.erase(token);
        dispatch_owner_set(token);
        (void)dispatch_owner_claim();
        return Result::TimedOut;
    }

    const auto acordei = g_arbiter.desperta.find(token);
    if (acordei == g_arbiter.desperta.end() ||
        acordei->second != ArbitrationBoard::DespertaSucesso) {
        g_arbiter.desperta.erase(token);
        dispatch_owner_set(token);
        (void)dispatch_owner_claim();
        return Result::InvalidCurrentMemory;
    }
    g_arbiter.desperta.erase(token);
    lock.unlock();

    // A espera acabou em SUCESSO. O word já foi escrito pelo signaler
    // (o tipo de sinal manda) — o Eden não re-verifica o word à saída:
    // `EndWait(ResultSuccess)` e regresso directo.
    dispatch_owner_set(token);
    (void)dispatch_owner_claim();
    return Result::Success;
}

// ── 0x35: SignalToAddress ─────────────────────────────────────────────────
//
// Eden: `svc_signal_to_address` (svc/svc_address_arbiter.cpp:79-100) →
// `KAddressArbiter::Signal*` (k_address_arbiter.cpp:90-240).
//
// **Medido no módulo sdk** — o `SdkMutexType::Unlock` (0x83ab1ad0, wrapper
// svc `0x83b0bc3c`) acorda com dois tipos:
//   • word==1 → `SignalToAddress(word, SignalAndModifyByWaiting-
//     CountIfEqual=2, value=1, count=1)` (prog. 0x83ab1b40-0x83ab1b50);
//   • build não flagada: `SignalToAddress(word, SignalAndIncrementIf-
//     Equal=1, value=1, count=-1)` (prog. 0x83ab1bac-0x83ab1bbc).
// E aceita de novo só Success/InvalidState (0xFA01): InvalidState = "o
// word não é o valor esperado" ⇒ o unlock RECOMEÇA (prog. 0x83ab1b5c,
// 0x83ab1bc8) — por isso o Result tem de ser o EXACTO da ABI.
//
// Modelo de valor (`SignalType`, svc_types.h:67-71):
//   0 Signal                    — sem acesso ao word; desperta até count;
//   1 SignalAndIncrementIfEqual — word != value ⇒ InvalidState; senão
//                                 word = value+1 e desperta até count;
//   2 SignalAndModifyByWaitingCountIfEqual — word != value ⇒ InvalidState;
//                                 senão novo = f(waiters, count) e desperta.
// f, medida no Eden (`SignalAndModifyByWaitingCountIfEqual` :140-179), com
// w = waiters estacionados no endereço:
//   • count <= 0 (todos): w > 0 → value-2; w = 0 → value+1;
//   • count > 0: w = 0 → value+1; 0 < w <= count → value-1; w > count →
//     value (o "novo" IGUAL ao value não escreve — o Eden só lê).
// `count <= 0` no despertar significa TODOS (`count <= 0 || num_waiters <
// count`, :93/:118/:184).
Result SvcDispatcher::svc_signal_to_address(SvcContext& ctx) {
    const std::uint64_t addr  = ctx.x[0];
    const std::uint32_t tipo  = static_cast<std::uint32_t>(ctx.x[1]);
    const std::int32_t  valor = static_cast<std::int32_t>(ctx.x[2]);
    const std::int32_t  count = static_cast<std::int32_t>(ctx.x[3]);

    if (addr == 0 || (addr & 3) != 0)
        return Result::InvalidAddress;
    if (tipo > 2)
        return Result::InvalidEnumValue;

    // Os tokens a acordar — o notify é FORA do gate (o precedente das
    // restantes sincronizações).
    std::vector<std::uint64_t> acordados;

    {
        std::unique_lock<std::timed_mutex> lock(g_arbiter.gate,
                                                std::defer_lock);
        quadro_entrar(lock, "0x35");

        std::uint32_t word = 0;
        if (tipo != 0 && !read_guest(addr, &word, sizeof word))
            return Result::InvalidCurrentMemory;

        std::int32_t novo_valor = 0;
        if (tipo == 1) {
            if (static_cast<std::int32_t>(word) != valor)
                return Result::InvalidState;  // Eden :129 R_UNLESS(user==value)
            novo_valor = valor + 1;
        } else if (tipo == 2) {
            if (static_cast<std::int32_t>(word) != valor)
                return Result::InvalidState;
            // A CONTA dos waiters é o CONTEÚDO da fila: quem está
            // estacionado no endereço NO MOMENTO do sinal, sob o MESMO
            // lock em que o word é escrito — a janela atómica é esta.
            const auto w = g_arbiter.arb_fila.find(addr);
            const std::size_t waiters =
                (w == g_arbiter.arb_fila.end()) ? 0 : w->second.size();
            if (count <= 0)
                novo_valor = (waiters > 0) ? valor - 2 : valor + 1;
            else if (waiters == 0)
                novo_valor = valor + 1;
            else if (waiters <= static_cast<std::size_t>(count))
                novo_valor = valor - 1;
            else
                novo_valor = valor;
        }

        if (novo_valor != static_cast<std::int32_t>(word) && tipo != 0) {
            // Eden :166-171/:224-227 — `UpdateIfEqual` escreve; sem
            // escrita quando o novo é igual (o caminho `ReadFromUser`).
            // A barreira vem antes da escrita, como nas restantes
            // sincronizações do motor.
            //
            // **CAS-guardada** (a lição da runL): o decremento cego pode
            // sobrescrever um passo LL/SC que o SDK do guest fez no meio
            // deste SVC — a re-leitura interna repõe o valor rival e a
            // troca repete. O `novo` é a DECISÃO do quadro (o waiters=
            // já foi consumido), nunca derivada da re-leitura.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const std::uint32_t novo = static_cast<std::uint32_t>(novo_valor);
            if (!escrita_atomica_word(addr, novo))
                return Result::InvalidCurrentMemory;  // Eden :226 R_UNLESS
        }

        // ── EndWait(ResultSuccess) até `count` (count <= 0 ⇒ todos).
        auto it = g_arbiter.arb_fila.find(addr);
        if (it == g_arbiter.arb_fila.end())
            return Result::Success;  // Eden: tree vazia ⇒ SUCCEED (:90-101)
        std::size_t quero = (count <= 0)
                                ? it->second.size()
                                : std::min<std::size_t>(
                                      static_cast<std::size_t>(count),
                                      it->second.size());
        for (std::size_t i = 0; i < quero; ++i) {
            const std::uint64_t token = it->second.front().token;
            it->second.erase(it->second.begin());
            // Eden :97-99/:126/:187 — `EndWait(ResultSuccess)`.
            g_arbiter.desperta[token] = ArbitrationBoard::DespertaSucesso;
            acordados.push_back(token);
        }
        if (it->second.empty())
            g_arbiter.arb_fila.erase(it);
    }

    if (!acordados.empty())
        g_arbiter.wake.notify_all();

    return Result::Success;
}

// ── 0x1D: SignalProcessWideKey ────────────────────────────────────────────
//
// Medida no Eden, svc_condition_variable.cpp:50-57 → KConditionVariable::
// Signal (k_condition_variable.cpp:~247-270). Nada de no-op:
//
//   • consome até `count` waiters da fila do cv_key (count <= 0 = todos);
//   • CADA waiter: SignalImpl (:198-248): UpdateLockAtomic na palavra do
//     mutex do waiter — palavra 0 ⇒ escreve o TAG do waiter e desperta-o
//     JÁ dono (:220); palavra ≠ 0 ⇒ escreve `prev | HandleWaitMask` (o
//     dono actual fica com a marca "tem waiters") e a thread vira WAITER
//     do dono (:222-236): passa da fila de cv para a fila do endereço e
//     continua a dormir até o unlock do dono a tornar dona;
//   • fila do cv_key esgotada ⇒ escreve 0 na palavra do cv_key
//     (HasNoWaiterFlag, :257) — inclusive quando nunca houve waiters
//     (nfind nulo e WriteToUser na mesma; retorno de WriteToUser não é
//     verificado aqui no Eden).
Result SvcDispatcher::svc_signal_process_wide_key(SvcContext& ctx) {
    const std::uint64_t cv_key = ctx.x[0] & ~std::uint64_t{3};
    const auto count = static_cast<std::int32_t>(ctx.x[1]);

    // **Instrumento leve — Parede 2** (a produtora das cv's do jogo). O
    // `IA_IPC_TRACE` despeja codigo e stack por chamada e fica fora das
    // corridas vivas; os **sinais** por si sao poucos (medido: 1847 numa
    // corrida completa, quase todos da bomba 0x10a5096e4). Esta linha leve
    // identifica o CIRCUITO — o `x30` e' o pc do guest produtor.
    static const bool trace_sinal = std::getenv("G610_SVC_SIGNAL_TRACE") != nullptr;
    if (trace_sinal)
        std::fprintf(stderr,
                     "[svc] SignalProcessWideKey key=0x%llx count=%d x30=0x%llx\n",
                     static_cast<unsigned long long>(cv_key), count,
                     static_cast<unsigned long long>(ctx.gpr[30]));

    // **Nivel de cima** — o frame-chain de x29: [x29] = fp do chamador,
    // [x29+8] = lr do chamador (o pc dentro do site que chamou o produtor).
    // O produtor da bomba (x30=0x805e6eb8, main+0x5E6EB8) vive no fim do
    // submitter 0x805e6970; o lr do chamador identifica qual dos 23 call-
    // sites posta a cada ciclo. Leves: uma leitura de 16 bytes por sinal.
    static const bool trace_pai = std::getenv("G610_SVC_SIGNAL_FP") != nullptr;
    if (trace_pai && !trace_sinal)
        std::fprintf(stderr, "[svc] SignalProcessWideKey key=0x%llx x30=0x%llx\n",
                     static_cast<unsigned long long>(cv_key),
                     static_cast<unsigned long long>(ctx.gpr[30]));
    if (trace_pai) {
        const std::uint64_t fp = ctx.gpr[29];
        constexpr std::uint64_t largura = 0x200000ull;
        constexpr std::uint64_t topo = 0x800000000ull;
        std::uint64_t f[6] = {0, 0, 0, 0, 0, 0};
        bool ok = fp >= largura && fp < topo && read_guest(fp, f, 2 * sizeof f[0]);
        if (ok && f[0] >= largura && f[0] < topo)
            (void)read_guest(f[0], f + 2, 2 * sizeof f[0]);
        if (ok && f[2] >= largura && f[2] < topo)
            (void)read_guest(f[2], f + 4, 2 * sizeof f[0]);
        if (ok)
            std::fprintf(stderr,
                         "[svc] SignalProcessWideKey cadeia key=0x%llx lvl1=0x%llx lvl2=0x%llx lvl3=0x%llx\n",
                         static_cast<unsigned long long>(cv_key),
                         static_cast<unsigned long long>(f[1]),
                         static_cast<unsigned long long>(f[3]),
                         static_cast<unsigned long long>(f[5]));

        // O prologo do submitter (0x805e6970) salvou o x19/x20 do chamador
        // em fp_sub-0x08/-0x10 (str x25,[sp,#-0x50]!; stp x20,x19,[sp,#0x30];
        // add x29,sp,#0x40). Vale para todos os sites que o chamam (o
        // submitter e unico): medir o dono por sinal, com o lvl1 emparelhado.
        const std::uint64_t fps = ctx.gpr[29];
        std::uint64_t cal[2] = {};
        if (fps >= largura + 0x10 && fps < topo &&
            read_guest(fps - 0x10, cal, sizeof cal)) {
            std::uint64_t filas[2] = {0, 0};
            const bool tem_fila =
                cal[1] >= largura && cal[1] < topo &&
                read_guest(cal[1] + 0x138, filas, sizeof filas);
            if (tem_fila)
                std::fprintf(stderr,
                             "[svc] Sinal origem lvl1=0x%llx dono=0x%llx "
                             "filaA=0x%llx filaB=0x%llx\n",
                             static_cast<unsigned long long>(f[1]),
                             static_cast<unsigned long long>(cal[1]),
                             static_cast<unsigned long long>(filas[0]),
                             static_cast<unsigned long long>(filas[1]));
            else
                std::fprintf(stderr,
                             "[svc] Sinal origem lvl1=0x%llx dono=0x%llx\n",
                             static_cast<unsigned long long>(f[1]),
                             static_cast<unsigned long long>(cal[1]));
        }
        if (ok) {
            const std::uint64_t fp1 = f[0];
            std::uint64_t janela[13] = {};
            if (fp1 >= largura + 0x48 && fp1 < topo &&
                read_guest(fp1 - 0x48, janela, sizeof janela))
                std::fprintf(stderr,
                             "[svc] Sinal janela fp1=0x%llx "
                             "j0=0x%llx j1=0x%llx j2=0x%llx j3=0x%llx "
                             "j4=0x%llx j5=0x%llx j6=0x%llx j7=0x%llx "
                             "j8=0x%llx j9=0x%llx j10=0x%llx j11=0x%llx\n",
                             static_cast<unsigned long long>(fp1),
                             static_cast<unsigned long long>(janela[0]),
                             static_cast<unsigned long long>(janela[1]),
                             static_cast<unsigned long long>(janela[2]),
                             static_cast<unsigned long long>(janela[3]),
                             static_cast<unsigned long long>(janela[4]),
                             static_cast<unsigned long long>(janela[5]),
                             static_cast<unsigned long long>(janela[6]),
                             static_cast<unsigned long long>(janela[7]),
                             static_cast<unsigned long long>(janela[8]),
                             static_cast<unsigned long long>(janela[9]),
                             static_cast<unsigned long long>(janela[10]),
                             static_cast<unsigned long long>(janela[11]));
        }
    }

    std::vector<std::uint64_t> wakers;
    std::size_t acordados = 0;

    {
        std::unique_lock<std::timed_mutex> lock(g_arbiter.gate,
                                                std::defer_lock);
        quadro_entrar(lock, "0x1d");

        const auto cv = g_arbiter.cv_fila.find(cv_key);
        const std::size_t total = (cv == g_arbiter.cv_fila.end())
                                      ? 0 : cv->second.size();
        const std::size_t quero = (count <= 0)
                                      ? total
                                      : std::min(static_cast<std::size_t>(count),
                                                 total);
        if (std::getenv("IA_IPC_TRACE") != nullptr && cv_key >= 0x100000000ull &&
            cv_key < 0x200000000ull) {
            std::fprintf(stderr, "[arb] signal cv=0x%llx waiters=%zu count=%d\n",
                         static_cast<unsigned long long>(cv_key), total, count);
        }

        for (std::size_t i = 0; i < quero; ++i) {
            const ArbitrationBoard::CvWaiter w = cv->second.front();
            cv->second.erase(cv->second.begin());

            // SignalImpl :209-215 — UpdateLockAtomic: prev==0 ⇒ if_zero
            // (o tag do waiter); prev≠0 ⇒ prev | HandleWaitMask.
            std::uint32_t prev = 0;
            if (!read_guest(w.addr, &prev, sizeof prev)) {
                // can_access == false → EndWait(InvalidCurrentMemory) :247
                g_arbiter.desperta[w.token] = ArbitrationBoard::DespertaInvalido;
                ++acordados;
                wakers.push_back(w.token);
                continue;
            }

            if (std::getenv("IA_IPC_TRACE") != nullptr) {
                std::fprintf(stderr,
                             "[arb] signal cv=0x%llx mutex=0x%llx palavra=0x%x tag=0x%x "
                             "-> %s\n",
                             static_cast<unsigned long long>(cv_key),
                             static_cast<unsigned long long>(w.addr), prev, w.tag,
                             prev == 0 ? "acorda (dono)" : "mexe para a fila do mutex");
            }
            // **Entrega do sinal** (o `UpdateLockAtomic` do Eden). Um
            // ÚNICO ciclo cobre os dois caminhos e resolve a troca
            // perdida contra o LL/SC do guest:
            //   - prev == 0: CAS(0 → tag) — o waiter acorda JÁ dono
            //     (`SignalImpl :217-220`);
            //   - prev != 0: CAS(observado → observado|0x40000000) sobre
            //     o VALOR ATUAL — o waiter vai para a fila do dono e só
            //     o `ArbitrateUnlock` do dono o acorda (:222-236).
            //
            // **Por que tantos CAS**: a escrita cega sobrescrevia uma
            // troca feita pelo SDK no meio — e a palavra podia ficar
            // `dono|waiters` SEM dono real (o congelamento da runL, svc
            // #912: o worker estacionou sobre 0x400081ff órfã e dormiu
            // para sempre enquanto o main esperava o sinal dele).
            const auto entrega_acorda_dono = [&] {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (cas_word_guest(w.addr, prev, w.tag))
                    g_arbiter.desperta[w.token] =
                        ArbitrationBoard::DespertaSucesso;
                else
                    g_arbiter.desperta[w.token] =
                        ArbitrationBoard::DespertaInvalido;
                ++acordados;
                wakers.push_back(w.token);
            };
            for (int tenta = 0;; ++tenta) {
                if (prev == 0) {
                    entrega_acorda_dono();
                    break;
                }
                const std::uint32_t novo = prev | kArbWaitMask;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (cas_word_guest(w.addr, prev, novo)) {
                    // **O waiter NAO acorda** — vai para a fila do
                    // mutex, e so' o `ArbitrateUnlock` do dono o acorda
                    // (o upstream).
                    g_arbiter.fila[w.addr].push_back({w.tag, w.token});
                    break;
                }
                // A troca caiu (o SDK correu no meio): re-ler a palavra
                // e REPETIR — o caminho livre/em-fila re-decide sobre o
                // valor ATUAL, nunca na leitura velha.
                if (!read_guest(w.addr, &prev, sizeof prev)) {
                    // Sem acesso a memoria — o upstream
                    // (`k_condition_variable.cpp:243`):
                    // `thread->EndWait(ResultInvalidCurrentMemory)`.
                    g_arbiter.desperta[w.token] =
                        ArbitrationBoard::DespertaInvalido;
                    ++acordados;
                    wakers.push_back(w.token);
                    break;
                }
                if (tenta >= 16) {
                    // Reserva degenerada (o rival nunca solta a palavra):
                    // entrega pela fila do dono e a palavra com reserva.
                    const std::uint32_t dele = prev | kArbWaitMask;
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    (void)escrita_atomica_word(w.addr, dele);
                    g_arbiter.fila[w.addr].push_back({w.tag, w.token});
                    break;
                }
            }
        }

        // **O sinal fica registado** se não havia ninguém à espera: o
        // `wait` que vier a seguir consome-o. É o que uma *cv* ao nível
        // faz — e o que o Eden garante por serializar `signal` e `wait`
        // no *scheduler lock*.
        if (total == 0)
            g_cv_pendente.insert(cv_key);

        // Eden :253-258 — fila da chave esgotada (ou nunca cheia): a
        // palavra do cv_key volta a 0, com a mesma tolerância do retorno.
        bool sem_espera = true;
        if (cv != g_arbiter.cv_fila.end()) {
            if (cv->second.empty())
                g_arbiter.cv_fila.erase(cv);
            else
                sem_espera = false;
        }
        if (sem_espera) {
            const std::uint32_t zero = 0;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            (void)write_guest(cv_key, &zero, sizeof zero);
        }
    }

    // Marca como prontas apenas as threads que esperavam nesta cv.
    mark_ready_threads_on_cv(cv_key);

    if (std::getenv("IA_THREAD_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[thrd] SIGNAL cv=0x%llx count=%d acordados=%zu pendente=%zu\n",
                     static_cast<unsigned long long>(cv_key), count, acordados,
                     g_cv_pendente.size());
    }

    if (!wakers.empty())
        g_arbiter.wake.notify_all();

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] SignalProcessWideKey key=0x%llx count=%d "
                     "acordados=%zu\n",
                     static_cast<unsigned long long>(cv_key), count, acordados);
    }

    // O SVC do Eden é `void`; o wrapper não escreve um resultado de volta
    // em x0. Preservamos x0, como o no-op antes medio fazia.
    ctx.ret = ctx.x[0];
    ctx.has_ret = true;
    return Result::Success;
}

std::uint64_t SvcDispatcher::random_u64() {
    // Sem estado, sem semente: `/dev/urandom` é a única fonte que não depende
    // de nada que este motor ainda não tenha. E o SDK pede `RandomEntropy`
    // duas vezes seguidas — se devolvêssemos o mesmo valor, o RNG do jogo
    // arrancaria com uma semente previsível, o que é pior do que não ter RNG.
    std::uint64_t v = 0;
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (f != nullptr) {
        if (std::fread(&v, 1, sizeof(v), f) != sizeof(v))
            v = 0;
        std::fclose(f);
    } else {
        v = entropy_counter_++;
    }
    return v;
}

// A SVC desconhecida. O `Unsupported` não é um Result HOS: é a marca do
// motor — ver o comentário de `Result::Unsupported` no cabeçalho.
Result SvcDispatcher::svc_unsupported(SvcContext&) {
    return Result::Unsupported;
}

// ── O sinal de "a GPU terminou" ────────────────────────────────────────────
//
// O driver chama isto no fim de uma submissão. **Medido**: sem ele, o
// jogo fica à espera de uma *condition variable* que ninguém acorda —
// e o `SignalProcessWideKey` do próprio jogo não ajuda, porque o que o
// jogo está a esperar é o driver.
//
// **O mecanismo actual**: o `KEvent` do evento NV — o mesmo
// objecto que o upstream sinaliza (`nvhost_ctrl.cpp`,
// `event.kevent->Signal()`). O `WaitSynchronization` do guest
// consulta-o e prossegue.
//
// A flag `g_gpu_trabalho_pronto` que existia aqui era **estado
// morto**: era escrita e nunca lida. O sinal agora vem do
// syncpoint (`g610/gpu/syncpoint.hpp`) — que dispara a accao
// registada pelo `EventWait`.
//
// O mecanismo é o do Eden (`SignalImpl`,
// `k_condition_variable.cpp:209-220`): a palavra do mutex passa a zero
// (liberta) e o *waiter* acorda **já dono** — por isso é preciso
// escrever na memória do guest, e não só tocar no token.
void g610::SvcDispatcher::ProcessPendingIpcPendente() {
    // Helper não-estático: permite que hooks static (RegistarTrabalhoGpu,
    // DespertarCvWaitersDoGuest) alcancem o EdenSystem do dispatcher corrente
    // através de `g_dispatcher_atual`.
    if (eden_)
        eden_->ProcessPendingSynchronously();
}

void g610::SvcDispatcher::RegistarTrabalhoGpu() {
    // **Sem sinal proprio** — o sinal e' o do `KEvent`.
    //
    // Esta funcao fica porque o `nvdrv` a chama no `FenceAction` (o
    // metodo de canal que incrementa a fence). Mas ja' nao ha flag
    // para ler: o `KEvent` ja' foi sinalizado, e o
    // `WaitSynchronization` do guest ve-o.
    //
    // Quando o syncpoint da GPU avanca, threads de guest podem estar
    // bloqueadas em cv's esperando a conclusao do trabalho. Em vez de
    // acordar todas (que causa busy round-robin sem progresso),
    // tentamos processar IPCs pendentes — um deles pode sinalizar
    // a cv que alguma thread espera.
    if (g_dispatcher_atual)
        g_dispatcher_atual->ProcessPendingIpcPendente();
}

void g610::SvcDispatcher::AcordarFutexNoEndereco(std::uint64_t endereco) {
    if (endereco == 0)
        return;
    std::unique_lock<std::timed_mutex> lock(g_arbiter.gate, std::defer_lock);
    quadro_entrar(lock, "gpu-wake");
    bool acordou = false;
    const auto cv = g_arbiter.cv_fila.find(endereco);
    if (cv != g_arbiter.cv_fila.end() && !cv->second.empty()) {
        const ArbitrationBoard::CvWaiter w = cv->second.front();
        cv->second.erase(cv->second.begin());
        if (cv->second.empty())
            g_arbiter.cv_fila.erase(cv);
        // A palavra do mutex passa a zero (liberta) e o waiter acorda
        // **dono** — o Eden, `SignalImpl` (`k_condition_variable.cpp:209-220`).
        // **CAS-guardada** (a lição da runL): a escrita cega podia
        // sobrescrever um passo LL/SC do SDK no meio — a reserva
        // degenerada da `escrita_atomica_word` mantém liveness.
        (void)g_dispatcher_atual->escrita_atomica_word(w.addr, 0);
        g_arbiter.desperta[w.token] = ArbitrationBoard::DespertaSucesso;
        acordou = true;
    }

    // **E o arbiter de endereços (0x34)**. O driver escreve o VALOR do
    // semáforo e desperta quem estava a olhar para o endereço — o
    // equivalente de `SignalToAddress(addr, Signal, value, todos)` do
    // Eden (`puller.cpp` — ProcessSemaphoreTriggerMethod). Sem isto, um
    // waiter do 0x34 estacionado numa fence do GPU dorme para sempre: a
    // cv e a árvore do arbiter não partilham namespace (`KAddressArbiter`
    // é distinta de `KConditionVariable`), por isso cada fila acorda a
    // sua.
    if (auto arb = g_arbiter.arb_fila.find(endereco);
        arb != g_arbiter.arb_fila.end() && !arb->second.empty()) {
        while (!arb->second.empty()) {
            const std::uint64_t token = arb->second.front().token;
            arb->second.erase(arb->second.begin());
            g_arbiter.desperta[token] = ArbitrationBoard::DespertaSucesso;
        }
        g_arbiter.arb_fila.erase(arb);
        acordou = true;
    }

    lock.unlock();
    if (acordou)
        g_arbiter.wake.notify_all();
}

void g610::SvcDispatcher::DespertarCvWaitersDoGuest() {
    if (g_dispatcher_atual == nullptr)
        return;

    // Quando o trabalho da GPU conclui, despertamos todas as threads de
    // guest bloqueadas em cv's para que re-avaliem os seus predicados.
    // Em vez de acordar todas (busy round-robin), processamos IPCs
    // pendentes — um deles pode sinalizar a cv que alguma thread espera.
    if (g_dispatcher_atual)
        g_dispatcher_atual->ProcessPendingIpcPendente();
    g_arbiter.wake.notify_all();
}

// **Acorda um waiter de cada fila de ArbitrateLock (mutex).**
// Usado para romper deadlocks onde o dono de um mutex está estacionado
// numa cv e ninguém o acorda. Cada fila de mutex acorda o seu primeiro
// waiter, que retoma o despacho e pode avançar.
// Não limpa as filas inteiras — só solta um por endereço para evitar
// tempestade de despertares.
void g610::SvcDispatcher::DespertarUmWaiterDeCadaFilaDeMutexECvs() {
    bool acordou = false;
    for (auto it = g_arbiter.fila.begin(); it != g_arbiter.fila.end();) {
        auto& fila = it->second;
        if (fila.empty()) {
            ++it;
            continue;
        }
        const ArbitrationBoard::Waiter w = fila.front();
        const std::uint64_t addr = it->first;
        fila.erase(fila.begin());
        if (fila.empty())
            it = g_arbiter.fila.erase(it);
        else
            ++it;

        const bool mais = g_arbiter.fila.count(addr) != 0;
        const std::uint32_t next = w.tag | (mais ? kArbWaitMask : 0u);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        (void)escrita_atomica_word(addr, next);
        g_arbiter.desperta[w.token] = ArbitrationBoard::DespertaSucesso;
        acordou = true;
    }

    // Acorda TODOS os waiters de cada cv_fila. Cada thread acordado
    // reavalia a sua predicada de espera — se ainda nao estiver satisfeita,
    // estaciona de novo. Isto rompe deadlocks onde um sinal foi enviado
    // para uma cv sem waiters enquanto threads esperam em outras cv's.
    for (auto it = g_arbiter.cv_fila.begin(); it != g_arbiter.cv_fila.end();) {
        auto& cvfila = it->second;
        while (!cvfila.empty()) {
            const ArbitrationBoard::CvWaiter w = cvfila.front();
            cvfila.erase(cvfila.begin());

            const std::uint32_t zero = 0;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            (void)escrita_atomica_word(it->first, zero);
            g_arbiter.desperta[w.token] = ArbitrationBoard::DespertaSucesso;
            acordou = true;
        }
        it = g_arbiter.cv_fila.erase(it);
    }

    if (acordou)
        g_arbiter.wake.notify_all();
}

// **Dump do quadro de arbitração** (g610_dump_arbiter) — chamado do tracer
// (um por tick do alarme e antes do relatório final).
//
// Por que existe: a fila do run mistura as threads (prints entre si) e os
// prints por chamada são limitados (`park_n % 1000`); sem o quadro no
// MOMENTO do congelamento a leitura fica na hipótese. O quadro dá: os
// waiters de mutex (fila), os waiters de cv (cv_fila), o canal de
// desperta e as PALAVRAS guest (mutex/cv) — o estado impossível "dono
// parca na cv" torna-se visível na hora, não por inferência.
//
// Limites: sem alocação, snprintf+::write (padrão do dump por thread);
// NÃO TOMA o gate — try_lock: se o interessado interrompeu o dono —
// os mapas podem estar em transição e não percorremos; despeja só as
// palavras. No contexto normal (fim do run) o gate está livre: dump
// completo.
// Escrita em 2 sem (void)-cast (GCC ignora o cast para warn_unused_result).
static inline void esc2(const void* dado, std::size_t tam) {
    const ssize_t w = ::write(2, dado, tam);
    (void)w;
}

extern "C" void g610_dump_arbiter() {
    char b[192];

    // Palavras de referência (o par de render e o par da bomba) — leitura
    // barata e segura (mapa de memória do guest).
    struct Par { std::uint64_t addr; const char* nome; };
    static const Par par[4] = {
        {0x10a5096d8, "mutex.render"},
        {0x10a5096e4, "cv.render"},
        {0x10a52c308, "mutex.bomba"},
        {0x10a52c314, "cv.bomba"},
    };
    for (const auto& p : par) {
        std::uint32_t w = 0;
        const bool ok = g_dispatcher_atual != nullptr &&
                        g_dispatcher_atual->read_guest(p.addr, &w, sizeof w);
        int k = std::snprintf(
            b, sizeof b, "[arb] word %s 0x%llx = 0x%x%s\n", p.nome,
            (unsigned long long)p.addr, ok ? w : 0, ok ? "" : " (sem leitura)");
        esc2(b, std::size_t(k));
    }

    // Janela da fila de render (x20+0x300..0x340: a cabeça/cauda e a
    // vizinhanca) — um quadro por tick: se o produtor mexe, aparece aqui.
    if (g_dispatcher_atual != nullptr) {
        std::uint8_t janela[0x40] = {};
        const bool ok = g_dispatcher_atual->read_guest(
            0x10a509660, janela, sizeof janela);
        if (ok) {
            char linha[320];
            std::size_t n = 0;
            n = static_cast<std::size_t>(
                std::snprintf(linha, sizeof linha, "[arb] janela.render:"));
            for (std::size_t o = 0; o < sizeof janela; o += 4) {
                std::uint32_t d = 0;
                std::memcpy(&d, janela + o, sizeof d);
                n += static_cast<std::size_t>(std::snprintf(
                    linha + n, sizeof(linha) - n, " %08x", d));
                if (n + 5 >= sizeof(linha))
                    break;
            }
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            esc2(linha, n);
        } else {
            esc2("[arb] janela.render: sem leitura\n", 27);
        }
    }

    // As outras janelas de interesse (a sentinela da fila, a fila
    // cruzada +0x310/+0x318 e o contexto do submitter dono+0x138) —
    // a mesma regra: um quadro por tick, para ver o produtor mexer.
    if (g_dispatcher_atual != nullptr) {
        struct Janela { std::uint64_t base; const char* nome; };
        static const Janela jan[3] = {
            {0x10a509cd0, "sentinela"},
            {0x10a2c29e8, "cruzada"},
            {0x10a2c6ef0, "dono"},
        };
        for (const auto& j : jan) {
            std::uint8_t janela[0x40] = {};
            const bool ok = g_dispatcher_atual->read_guest(
                j.base, janela, sizeof janela);
            if (!ok) {
                char vazio[64];
                const int kv = std::snprintf(
                    vazio, sizeof vazio, "[arb] janela.%s: sem leitura\n",
                    j.nome);
                esc2(vazio, std::size_t(kv));
                continue;
            }
            char linha[320];
            std::size_t n = std::snprintf(
                linha, sizeof linha, "[arb] janela.%s:", j.nome);
            for (std::size_t o = 0; o < sizeof janela; o += 4) {
                std::uint32_t d = 0;
                std::memcpy(&d, janela + o, sizeof d);
                n += std::snprintf(linha + n, sizeof(linha) - n,
                                   " %08x", d);
                if (n + 5 >= sizeof(linha))
                    break;
            }
            n += std::snprintf(linha + n, sizeof(linha) - n, "\n");
            esc2(linha, n);
        }
    }

    // Sob try_lock: a travessia dos mapas. Falhou ⇒ só palavras.
    bool guardado = g_arbiter.gate.try_lock();

    int k = std::snprintf(b, sizeof b,
                          "[arb] quadro gate=%s fila=%zu cv=%zu desperta=%zu "
                          "pendente=%zu\n",
                          guardado ? "TOMADO (para o dump)" : "ocupado",
                          guardado ? g_arbiter.fila.size() : std::size_t(0),
                          guardado ? g_arbiter.cv_fila.size() : std::size_t(0),
                          guardado ? g_arbiter.desperta.size() : std::size_t(0),
                          guardado ? g_cv_pendente.size() : std::size_t(0));
    esc2(b, std::size_t(k));

    if (guardado) {
        std::size_t n = 0;
        for (const auto& [addr, fila] : g_arbiter.fila) {
            std::uint32_t w = 0;
            const bool ok = g_dispatcher_atual != nullptr &&
                            g_dispatcher_atual->read_guest(addr, &w, sizeof w);
            int kk = std::snprintf(
                b, sizeof b, "[arb] fila 0x%llx (%zu) word=0x%x:",
                (unsigned long long)addr, fila.size(), ok ? w : 0);
            esc2(b, std::size_t(kk));
            for (std::size_t i = 0;
                 i < fila.size() && i < 8; ++i) {
                kk = std::snprintf(b, sizeof b, " {0x%x,0x%llx}",
                                   fila[i].tag,
                                   (unsigned long long)fila[i].token);
                esc2(b, std::size_t(kk));
            }
            esc2("\n", 1);
            if (++n >= 8) break;
        }

        n = 0;
        for (const auto& [cv_key, fila] : g_arbiter.cv_fila) {
            int kk = std::snprintf(
                b, sizeof b, "[arb] cv 0x%llx (%zu):",
                (unsigned long long)cv_key, fila.size());
            esc2(b, std::size_t(kk));
            for (std::size_t i = 0;
                 i < fila.size() && i < 8; ++i) {
                kk = std::snprintf(
                    b, sizeof b, " {0x%x,addr=0x%llx,0x%llx}",
                    fila[i].tag, (unsigned long long)fila[i].addr,
                    (unsigned long long)fila[i].token);
                esc2(b, std::size_t(kk));
            }
            esc2("\n", 1);
            if (++n >= 8) break;
        }

        n = 0;
        for (const auto& [tok, cod] : g_arbiter.desperta) {
            int kk = std::snprintf(
                b, sizeof b, "[arb] desperta 0x%llx = %u\n",
                (unsigned long long)tok, (unsigned)cod);
            esc2(b, std::size_t(kk));
            if (++n >= 8) { esc2("[arb] desperta (...)\n", 18); break; }
        }

        g_arbiter.gate.unlock();
    }
}

} // namespace g610
