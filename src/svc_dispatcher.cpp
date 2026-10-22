// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação do despachante de SVC.
#include "g610/svc_dispatcher.hpp"

#include "g610/eden_bridge.hpp"
#include "g610/native_engine.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <sched.h>
#include <thread>
#include <vector>

namespace g610 {

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

bool SvcDispatcher::write_guest(std::uint64_t addr, const void* src,
                                std::size_t size) {
    if (mem_ == nullptr || size == 0)
        return false;
    return mem_->write(addr, src, size, g_err_scratch);
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

void SvcDispatcher::close_handle(std::uint32_t handle) {
    auto it = name_by_handle_.find(handle);
    if (it == name_by_handle_.end())
        return;
    port_by_name_.erase(it->second);
    name_by_handle_.erase(it);
}

// ---------------------------------------------------------------------------
// dispatch — o caminho quente.
// ---------------------------------------------------------------------------

void SvcDispatcher::dispatch(std::uint32_t number, SvcContext& ctx) {
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

    // **O x0 que o guest recebe** ✗ — o SDK le-o ✗ ✗.

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

    ++map_memory_calls_;
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

    // **A thread corre.** O codigo do guest e' nativo, logo uma thread de
    // guest e' uma thread do host a correr o entry point, com o `sp` que o
    // guest deu (o Eden chama-lhe `user_stack_top`, `k_thread.h:737`) e com
    // o `TPIDR_EL0` apontado para o TLS *dela*.
    //
    // O TLS e' uma copia do da thread principal, com o handle novo em
    // TLS+0x110 — que e' exactamente o que o Eden escreve
    // (`Write32(tls + 0x110, *out_handle)`, `svc_thread.cpp:79`).
    if (mem_ != nullptr && tls_base_ != 0) {
        const std::uint64_t tls = tls_base_ + 0x1000ull * (++tls_threads_);
        std::vector<std::uint8_t> bloco(0x1000, 0);
        if (read_guest(tls_base_, bloco.data(), bloco.size())) {
            std::memcpy(bloco.data() + 0x110, &h.raw, sizeof h.raw);
            std::memset(bloco.data(), 0, 0x100);
            if (!write_guest(tls, bloco.data(), bloco.size())) {
                last_ipc_ = "CreateThread: nao escrevi o TLS da thread";
            } else {
                std::thread([entry, arg, stack_bottom, tls]() {
                    NativeEngine::run_guest_thread(entry, arg, stack_bottom, tls);
                }).detach();
            }
        }
    }

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] CreateThread handle=0x%x entry=0x%llx arg=0x%llx "
                     "stack=0x%llx prio=%d core=%d  (STUB: a thread nao corre)\n",
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
// **STUB**, pela mesma razao do `CreateThread`: a thread existe na nossa
// tabela de handles, mas **nao corre** — este motor corre o guest
// nativamente, numa thread do host. Devolver `Success` e' o que o guest
// precisa para nao morrer: um erro aqui e' fatal para o SDK (medido: o
// `svc #183` foi o `0x09` a devolver `Unsupported` (0x40), e o SDK foi
// logo a `SetTerminateResult` e ao `Break`).
Result SvcDispatcher::svc_start_thread(SvcContext& ctx) {
    const std::uint64_t handle = ctx.x[0];
    const Handle h{static_cast<std::uint32_t>(handle)};
    const std::uint32_t principal = main_thread_handle();
    const bool pseudo = (handle == kCurrentThreadHandle);
    const bool nossa = handles_.obter(h) != nullptr;
    if (eden_attached() && principal != 0 && handle != principal && !pseudo && !nossa)
        return Result::InvalidHandle;

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

    std::mutex gate;
    std::condition_variable wake;
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
        std::unique_lock<std::mutex> lock(g_arbiter.gate);

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
        dispatch_owner_vacate();
        g_arbiter.wake.wait(lock, [&] {
            const auto d = g_arbiter.desperta.find(token);
            return d != g_arbiter.desperta.end() &&
                   d->second != ArbitrationBoard::DespertaAguarda;
        });
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
        std::lock_guard<std::mutex> lock(g_arbiter.gate);
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            std::uint32_t prev = 0;
            (void)read_guest(addr, &prev, sizeof prev);
            const auto it = g_arbiter.fila.find(addr);
            const std::size_t na_fila =
                it == g_arbiter.fila.end() ? 0 : it->second.size();
            std::fprintf(stderr, "[arb] unlock addr=0x%llx palavra=0x%x na_fila=%zu\n",
                         static_cast<unsigned long long>(addr), prev, na_fila);
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
        }
        // Eden :132-137: fence seq_cst antes do write; e o write é
        // INCONDICIONAL — unlock sem waiters escreve 0 (cabeça livre). O
        // rtld/jogo leem isto via CAS rápida própria no próximo lock.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!write_guest(addr, &next, sizeof next))
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

    // **Havia um sinal pendente nesta cv?** (o `signal` chegou antes do
    // `wait` — ver a nota de `g_cv_pendente`.)
    {
        std::lock_guard<std::mutex> lock(g_arbiter.gate);
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

    // Eden :20-21: R_UNLESS(!IsKernelAddress(address), InvalidCurrentMemory) e
    // alinhamento sizeof(s32) → InvalidAddress. (IsKernelAddress não se
    // aplica a este guest: o espaço do processo não cruza a metade kernel.)
    if (addr == 0 || (addr & 3) != 0)
        return Result::InvalidAddress;

    std::uint64_t token = 0;
    std::uint8_t  acordei_com = ArbitrationBoard::DespertaAguarda;
    {
        std::unique_lock<std::mutex> lock(g_arbiter.gate);

        // ── (1) o mutex passa ao próximo waiter do ENDEREÇO ──
        // A MESMA transacção do ArbitrateUnlock, mas conduzida pela thread
        // da cv: sou o dono em curso e pop o primeiro da minha fila.
        //
        // **E o próximo dono tem de ser ACORDADO.** O Eden faz isto em
        // `KConditionVariable::Wait` (`k_condition_variable.cpp:285-292`):
        //
        //     KThread* next_owner_thread = cur_thread->RemoveUserWaiterByKey(...);
        //     if (next_owner_thread != nullptr) {
        //         next_value = next_owner_thread->GetAddressKeyValue();
        //         next_owner_thread->EndWait(ResultSuccess);   // <-- ACORDA
        //     }
        //
        // Sem o `EndWait` — e portanto sem o nosso `notify` — o próximo
        // dono fica **marcado** como entregue e **dorme para sempre**.
        //
        // **Medido** (`boot_p6o.log`): o `SignalProcessWideKey` encontrava
        // o waiter (`count=1`) e não acordava ninguém (`acordados=0`); as
        // threads do guest acabavam todas paradas à espera do mesmo
        // mutex, e o jogo não avançava. Era este o `notify` em falta.
        std::uint32_t next = 0;
        bool acordou_proximo = false;
        const auto fila_it = g_arbiter.fila.find(addr);
        if (fila_it != g_arbiter.fila.end() && !fila_it->second.empty()) {
            const ArbitrationBoard::Waiter w = fila_it->second.front();
            fila_it->second.erase(fila_it->second.begin());
            if (fila_it->second.empty())
                g_arbiter.fila.erase(fila_it);
            const bool mais = g_arbiter.fila.count(addr) != 0;
            next = w.tag | (mais ? kArbWaitMask : 0u);
            g_arbiter.desperta[w.token] = ArbitrationBoard::DespertaSucesso;
            acordou_proximo = true;
        }

        // ── (2)+(3) a ORDEM está medida no Eden: bandeira do cv_key
        // primeiro, fence seq_cst, e SÓ ENTÃO a palavra do mutex ──
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const std::uint32_t um = 1;  // HasWaiterFlag (:308)
        if (!write_guest(cv_key, &um, sizeof um))
            return Result::InvalidCurrentMemory;
        if (!write_guest(addr, &next, sizeof next))
            return Result::InvalidCurrentMemory;  // Eden (:316)

        // O próximo dono acorda **aqui**, com o gate tomado.
        //
        // É o que o Eden faz: o `EndWait` está dentro do scheduler lock.
        // E é seguro: o predicado (`desperta[token]`) já está escrito
        // ANTES do `notify`, por isso quem ainda não chegou ao `wait()`
        // re-avalia o predicado quando tiver o gate — e quem já está a
        // dormir éaccordado pelo próprio `notify`.
        if (acordou_proximo)
            g_arbiter.wake.notify_all();

        // ── (4) timeout == 0: TimedOut pós-release, sem dormir ──
        if (timeout == 0)
            return Result::TimedOut;

        // ── (5) marca na fila da cv e dorme ──
        token = dispatch_owner_itself();
        if (token == 0)
            return Result::InvalidState;

        g_arbiter.desperta[token] = ArbitrationBoard::DespertaAguarda;
        g_arbiter.cv_fila[cv_key].push_back({tag, addr, token});
        if (std::getenv("IA_IPC_TRACE") != nullptr && cv_key >= 0x100000000ull &&
            cv_key < 0x200000000ull) {
            std::fprintf(stderr, "[arb] registado cv=0x%llx tag=0x%x fila=%zu token=0x%llx\n",
                         static_cast<unsigned long long>(cv_key), tag,
                         g_arbiter.cv_fila[cv_key].size(),
                         static_cast<unsigned long long>(token));
        }
        dispatch_owner_vacate();

        const auto dono_acordou = [&] {
            const auto d = g_arbiter.desperta.find(token);
            return d != g_arbiter.desperta.end() &&
                   d->second != ArbitrationBoard::DespertaAguarda;
        };

        if (timeout > 0) {
            const auto prazo = std::chrono::steady_clock::now() +
                               std::chrono::nanoseconds(timeout);
            // Predic-versão do wait_until: devolve o VALOR FINAL do
            // predicado. Se o sinal entrou no último instante, o
            // predicado final é o que conta — a mesma arbitragem de quem
            // faz EndWait primeiro no Eden.
            if (!g_arbiter.wake.wait_until(lock, prazo, dono_acordou)) {
                // Prazo sob o gate, ninguém entregou. Sai do QUADRO para
                // sempre: posso estar na fila da cv OU, se o Signal já me
                // moveu, na fila do dono (a tree do Eden desmonta a espera
                // no cancel do timer; aqui a remoção é por token nas duas
                // filas).
                if (auto cv = g_arbiter.cv_fila.find(cv_key);
                    cv != g_arbiter.cv_fila.end()) {
                    auto& fila = cv->second;
                    for (auto w = fila.begin(); w != fila.end(); ++w)
                        if (w->token == token) { fila.erase(w); break; }
                    if (fila.empty())
                        g_arbiter.cv_fila.erase(cv);
                }
                for (auto f = g_arbiter.fila.begin(); f != g_arbiter.fila.end();) {
                    auto& fila = f->second;
                    for (auto w = fila.begin(); w != fila.end(); ++w)
                        if (w->token == token) { fila.erase(w); break; }
                    if (fila.empty())
                        f = g_arbiter.fila.erase(f);
                    else
                        ++f;
                }
                g_arbiter.desperta.erase(token);
                dispatch_owner_set(token);
                (void)dispatch_owner_claim();
                return Result::TimedOut;
            }
        } else {
            // Negativo: espera sem fim.
            //
            // **Com uma rede de segurança.** Medido: o jogo perde um
            // sinal (o `signal` chega *antes* do `wait` — no Eden os
            // dois são serializados pelo scheduler lock, e o nosso quadro
            // não os serializa nessa janela). Com a espera a expirar, o
            // guest re-avalia o seu predicado e volta a esperar — que é
            // o que uma *cv* bem usada faz.
            //
            // `IA_WAIT_MS` desliga a rede (espera infinita, como o Eden).
            //
            // **O prazo é só para diagnóstico** (`IA_WAIT_MS`). Com ele,
            // o *waiter* desiste e sai da fila — e o sinal que chegava
            // depois já não tinha ninguém para acordar (medido: o
            // *worker* do jogo ficou à espera para sempre, e o jogo
            // parou num "limite de traps").
            //
            // A espera normal é **sem fim**, como no Eden ✓ — e as
            // perdas de sinal estão cobertas pelo "sinal pendente" e
            // pelo signal que acorda o *waiter*.
            const char* prazo_ms = std::getenv("IA_WAIT_MS");
            bool expirou = false;
            if (prazo_ms != nullptr) {
                expirou = !g_arbiter.wake.wait_for(
                    lock, std::chrono::milliseconds(std::atoi(prazo_ms)), dono_acordou);
            } else {
                g_arbiter.wake.wait(lock, dono_acordou);
            }
            if (expirou) {
                // Expirou: sai da fila (como o caminho do prazo).
                if (auto cv = g_arbiter.cv_fila.find(cv_key);
                    cv != g_arbiter.cv_fila.end()) {
                    auto& fila = cv->second;
                    for (auto w = fila.begin(); w != fila.end(); ++w)
                        if (w->token == token) { fila.erase(w); break; }
                    if (fila.empty())
                        g_arbiter.cv_fila.erase(cv);
                }
                for (auto f = g_arbiter.fila.begin(); f != g_arbiter.fila.end();) {
                    auto& fila = f->second;
                    for (auto w = fila.begin(); w != fila.end(); ++w)
                        if (w->token == token) { fila.erase(w); break; }
                    if (fila.empty())
                        f = g_arbiter.fila.erase(f);
                    else
                        ++f;
                }
                g_arbiter.desperta.erase(token);
                dispatch_owner_set(token);
                (void)dispatch_owner_claim();
                return Result::TimedOut;
            }
        }

        acordei_com = g_arbiter.desperta.at(token);
        g_arbiter.desperta.erase(token);
    }

    // Sucesso ⇒ o SignalImpl escreveu o MEU tag na palavra: mutex em mão,
    // como o `Success` de fim de espera do Eden devolve a posse (:220).
    // A thread volta ao despacho como o ArbitrateLock faz ao acordar.
    dispatch_owner_set(token);
    (void)dispatch_owner_claim();
    if (acordei_com != ArbitrationBoard::DespertaSucesso)
        return Result::InvalidCurrentMemory;
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

    std::vector<std::uint64_t> wakers;
    std::size_t acordados = 0;

    {
        std::lock_guard<std::mutex> lock(g_arbiter.gate);

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
            if (prev == 0) {
                // :217-220 — prev_tag == Svc::InvalidHandle: a palavra
                // passa ao waiter e ele acorda JÁ dono.
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (write_guest(w.addr, &w.tag, sizeof w.tag))
                    g_arbiter.desperta[w.token] =
                        ArbitrationBoard::DespertaSucesso;
                else
                    g_arbiter.desperta[w.token] =
                        ArbitrationBoard::DespertaInvalido;
                ++acordados;
                wakers.push_back(w.token);
            } else {
                // ── Existe dono ✗ — o upstream NAO acorda ✗ ───────────────
                //
                // O upstream (`k_condition_variable.cpp:222-236` ✗) e'
                // o `SignalImpl` ✗: com o mutex ocupado ✗, o `prev_tag`
                // nao e' `InvalidHandle` ✗ — e o `EndWait` **nao** e'
                // chamado ✗ ✗. O waiter fica a espera ✗ e passa a fila
                // do **dono** ✗ (`owner_thread->AddWaiter(thread)` ✗) ✗.
                //
                // **O `ArbitrateUnlock`** ✗ — o `SignalToAddress`
                // (`k_condition_variable.cpp:113-151` ✗) — e' o que passa
                // o mutex ao proximo ✗ e o **acorda** ✗ ✗.
                //
                // **A casa acordava na mesma** ✗ ✗ e **dava-lhe o
                // mutex** ✗ ✗ — o que **roubava o mutex ao dono** ✗ ✗.
                // O log mostra ✗: `[arb] signal cv=0x10a2c7844 mutex=0x10a2c7838
                // palavra=0x81ff tag=0x20003 -> mexe para a fila do mutex`
                // ✗ ✗ — o mutex estava ocupado ✗ (`0x81ff` ✗) ✗ e o
                // waiter foi **acordado na mesma** ✗.
                //
                // A consequencia ✗: o dono do mutex ✗ (a thread de render
                // ✗) perde-o ✗ ✗ — e a cadeia de cv's do jogo ✗ nunca
                // mais avanca ✗ ✗. O jogo trava ✗ ✗ e o watchdog aborta
                // ✗ (`SetTerminateResult modulo=2 desc=3005` ✗) ✗ ✗.
                //
                // **O que se escreve** ✗ — o upstream ✗
                // (`k_condition_variable.cpp:217-220` ✗) ✗:
                //
                //     if (prev_tag == Svc::InvalidHandle) {
                //         thread->EndWait(ResultSuccess);   // <- so' aqui
                //     } else {
                //         owner_thread->AddWaiter(thread);   // <- espera
                //     }
                //
                // A palavra do mutex fica ✗ — o upstream ✗
                // (`UpdateLockAtomic` ✗) ✗ faz `value = expected |
                // HandleWaitMask` ✗ — ou seja ✗, o valor antigo **com o
                // bit de "ha waiters"** ✗ ✗.
                const std::uint32_t novo = prev | kArbWaitMask;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (write_guest(w.addr, &novo, sizeof novo)) {
                    // **O waiter NAO acorda** ✗ ✗ — vai para a fila do
                    // mutex ✗, e so' o `ArbitrateUnlock` do dono o acorda
                    // ✗ ✗ (o upstream ✗).
                    g_arbiter.fila[w.addr].push_back({w.tag, w.token});
                } else {
                    // Sem acesso a memoria ✗ — o upstream ✗
                    // (`k_condition_variable.cpp:243` ✗) ✗:
                    // `thread->EndWait(ResultInvalidCurrentMemory)` ✗.
                    g_arbiter.desperta[w.token] =
                        ArbitrationBoard::DespertaInvalido;
                    ++acordados;
                    wakers.push_back(w.token);
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
// **O mecanismo actual** ✗: o `KEvent` do evento NV ✗ — o mesmo
// objecto que o upstream sinaliza ✗ (`nvhost_ctrl.cpp` ✗,
// `event.kevent->Signal()` ✗) ✗. O `WaitSynchronization` do guest
// consulta-o e prossegue ✗ ✗.
//
// A flag `g_gpu_trabalho_pronto` que existia aqui ✗ era **estado
// morto** ✗: era escrita ✗ e nunca lida ✗ ✗. O sinal agora vem do
// syncpoint ✗ (`g610/gpu/syncpoint.hpp` ✗) ✗ — que dispara a accao
// registada pelo `EventWait` ✗ ✗.
//
// O mecanismo é o do Eden (`SignalImpl`,
// `k_condition_variable.cpp:209-220`): a palavra do mutex passa a zero
// (liberta) e o *waiter* acorda **já dono** — por isso é preciso
// escrever na memória do guest, e não só tocar no token.
void g610::SvcDispatcher::RegistarTrabalhoGpu() {
    // **Sem sinal proprio** ✗ — o sinal e' o do `KEvent` ✗ ✗.
    //
    // Esta funcao fica porque o `nvdrv` a chama no `FenceAction` ✗ (o
    // metodo de canal que incrementa a fence ✗) ✗. Mas ja' nao ha flag
    // para ler ✗ ✗: o `KEvent` ja' foi sinalizado ✗ ✗, e o
    // `WaitSynchronization` do guest ve-o ✗ ✗.
}

void g610::SvcDispatcher::AcordarFutexNoEndereco(std::uint64_t endereco) {
    if (endereco == 0)
        return;
    std::unique_lock<std::mutex> lock(g_arbiter.gate);
    const auto cv = g_arbiter.cv_fila.find(endereco);
    if (cv == g_arbiter.cv_fila.end() || cv->second.empty())
        return;
    const ArbitrationBoard::CvWaiter w = cv->second.front();
    cv->second.erase(cv->second.begin());
    if (cv->second.empty())
        g_arbiter.cv_fila.erase(cv);
    // A palavra do mutex passa a zero (liberta) e o waiter acorda
    // **dono** — o Eden, `SignalImpl` (`k_condition_variable.cpp:209-220`).
    const std::uint32_t zero = 0;
    (void)g_dispatcher_atual->write_guest(w.addr, &zero, sizeof zero);
    g_arbiter.desperta[w.token] = ArbitrationBoard::DespertaSucesso;
    lock.unlock();
    g_arbiter.wake.notify_all();
}

void g610::SvcDispatcher::DespertarCvWaitersDoGuest() {
    if (g_dispatcher_atual == nullptr)
        return;

    std::unique_lock<std::mutex> lock(g_arbiter.gate);
    std::size_t acordados = 0;
    for (auto& [cv_key, fila] : g_arbiter.cv_fila) {
        (void)cv_key;
        while (!fila.empty()) {
            const ArbitrationBoard::CvWaiter w = fila.front();
            fila.erase(fila.begin());

            // Liberta o mutex: a palavra tem de estar a zero para o
            // waiter acordar dono.
            const std::uint32_t zero = 0;
            (void)g_dispatcher_atual->write_guest(w.addr, &zero, sizeof zero);

            g_arbiter.desperta[w.token] = ArbitrationBoard::DespertaSucesso;
            ++acordados;
        }
    }

    if (acordados > 0) {
        std::fprintf(stderr, "[gpu] a GPU terminou: %zu waiter(s) acordados\n", acordados);
        g_arbiter.wake.notify_all();
    }
}

} // namespace g610
