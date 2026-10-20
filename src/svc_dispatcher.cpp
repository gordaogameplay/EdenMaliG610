// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação do despachante de SVC.
#include "g610/svc_dispatcher.hpp"

#include "g610/eden_bridge.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <thread>

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
    }
    return "Svc#" + std::to_string(static_cast<std::uint32_t>(s));
}

std::string to_string(Result r) {
    switch (r) {
    case Result::Success:           return "Success";
    case Result::Invalid:           return "Invalid";
    case Result::NotInitialized:    return "NotInitialized";
    case Result::NotSupported:      return "NotSupported";
    case Result::InvalidHandle:     return "InvalidHandle";
    case Result::OutOfMemory:       return "OutOfMemory";
    case Result::NotFound:          return "NotFound";
    case Result::InvalidAddress:    return "InvalidAddress";
    case Result::PermissionDenied:  return "PermissionDenied";
    case Result::OutOfResource:     return "OutOfResource";
    case Result::Unsupported:       return "Unsupported";
    case Result::InvalidCoreId:    return "InvalidCoreId";
    case Result::InvalidCombination: return "InvalidCombination";
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

    Result r;
    switch (static_cast<Svc>(number)) {
    case Svc::SetHeapSize:          r = svc_set_heap_size(ctx); break;
    case Svc::ExitProcess:          r = svc_exit_process(ctx);  break;
    case Svc::CloseHandle:          r = svc_close_handle(ctx);  break;
    case Svc::SetMemoryPermission:  r = svc_control_memory(ctx); break;
    case Svc::GetSystemTick:        r = svc_get_system_tick(ctx); break;
    case Svc::GetThreadId:          r = svc_get_thread_id(ctx); break;
    case Svc::GetProcessId:         r = svc_get_process_id(ctx); break;
    case Svc::ConnectToNamedPort:   r = svc_connect_port(ctx);   break;
    case Svc::MapMemory:            r = svc_map_memory(ctx);     break;
    case Svc::UnmapMemory:          r = svc_unmap_memory(ctx);   break;
    case Svc::MapSharedMemory:      r = svc_map_shared_memory(ctx); break;
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
    case Svc::SignalProcessWideKey: r = svc_signal_process_wide_key(ctx); break;
    default:                        r = svc_unsupported(ctx);    break;
    }

    // Só escreve o Result se o serviço não tiver devolvido um valor.
    // Ver `SvcContext::has_ret`.
    if (!ctx.has_ret)
        ctx.ret = static_cast<std::uint64_t>(r);

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
        return Result::Invalid;

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

SvcDispatcher::SvcDispatcher(tel::Telemetry& tel) : tel_(tel) {}
SvcDispatcher::~SvcDispatcher() = default;

void SvcDispatcher::set_eden_tls(std::uint64_t endereco) noexcept {
    // No-op sem Eden ligado, e sem erro: os testes que não montam um programa
    // não têm TLS para dar.
    if (!eden_)
        return;

    eden_->set_tls(endereco);

    // O handle da thread principal, em TLS+0x110. Ver a nota no cabecalho.
    const std::uint32_t handle = eden_->main_thread_handle();
    if (handle != 0 && mem_ != nullptr) {
        std::string err;
        if (!mem_->write(endereco + 0x110, &handle, sizeof(handle), err))
            last_ipc_ = "set_eden_tls: nao escrevi o handle em TLS+0x110: " + err;
    }
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
    //      vinha a zero — pelo que devolvia sempre `Result::Invalid`.
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
        return Result::Invalid;

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
        return Result::NotInitialized;
    if (size == 0)
        return Result::Invalid;

    // O endereço é do guest: tem de estar dentro do que reservámos. Mapear
    // fora disso é mapear por cima de outra coisa do processo do host, e o
    // kernel do Switch rejeitaria. É a mesma razão por que se usa
    // MAP_FIXED_NOREPLACE — um endereço errado tem de ser um erro visível,
    // não uma corruption silenciosa.
    if (!mem_->owns(dst) || !mem_->owns(dst + size - 1))
        return Result::Invalid;

    // A memória dentro da reserva já é do processo e já é acessível. O
    // "mapeamento" que o Switch faria (traduzir físico -> virtual) aqui é um
    // no-op, porque o guest e o host partilham o espaço de endereçamento.
    ++map_memory_calls_;
    return Result::Success;
}



Result SvcDispatcher::svc_map_shared_memory(SvcContext& ctx) {
    // Mesma semântica que MapMemory num guest nativo: "partilhar" memória
    // entre processos dentro do mesmo espaço é um no-op, porque já é a mesma
    // memória física.
    return svc_map_memory(ctx);
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
        return Result::NotInitialized;

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
        for (const GuestRegion& r : regions_) {
            if (r.size == 0)
                continue;
            if (query >= r.base && query < r.base + r.size) {
                // O **bloco**, não a página.
                //
                // O Eden (`k_page_table_base.cpp:904`, `QueryInfoImpl`)
                // devolve o `KMemoryBlock` que contém o endereço — e o
                // bloco é a região, não a página de 4 KiB. A medição
                // confirma: com a página, o `rtld` do jogo percorria o
                // espaço do processo a 4 KiB de cada vez (3999
                // interrogações e a varredura não terminava); com a
                // região, são 10 interrogações — uma por região — e o
                // `rtld` encontra os módulos.
                info.base_address = r.base;
                info.size         = r.size;
                info.state        = r.state;
                info.permission   = r.permission;
                break;
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
        return Result::Invalid;

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
        return Result::Invalid;

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

    // O handle tem de existir. Só há uma thread de guest — a principal — e o
    // Eden dá-lhe um handle na sua tabela (medido no guest: 0x81ff).
    const std::uint32_t principal = main_thread_handle();
    const bool pseudo = (handle == kCurrentThreadHandle);
    if (eden_attached() && principal != 0 && handle != principal && !pseudo)
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
    if (eden_attached() && principal != 0 && handle != principal && !pseudo)
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

Result SvcDispatcher::svc_signal_process_wide_key(SvcContext& ctx) {
    const std::uint64_t cv_key = ctx.x[0] & ~std::uint64_t{3};
    const auto count = static_cast<std::int32_t>(ctx.x[1]);

    // Semântica medida no Eden, `svc/svc_condition_variable.cpp:50-57`:
    // `SignalProcessWideKey` chama `KProcess::SignalConditionVariable` com a
    // chave alinhada a `sizeof(u32)` e o número de threads a acordar.
    //
    // Este motor não tem fila de espera de condition variables: tem uma
    // thread guest, o `WaitProcessWideKeyAtomic` ainda não é implementado, e
    // nenhuma thread pode estar à espera. Sinalizar zero waiters é um no-op.
    //
    // O SVC do Eden é `void`; o wrapper não escreve um resultado de volta em
    // x0. Preservamos x0, em vez de inventar um ResultSuccess que a ABI não
    // devolve. Se surgir o primeiro waiter, `WaitProcessWideKeyAtomic` tem de
    // ser implementado antes de este no-op deixar de ser correcto.
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[svc] SignalProcessWideKey key=0x%llx count=%d x2=0x%llx "
                     "x3=0x%llx x4=0x%llx (sem waiters; no-op)\n",
                     static_cast<unsigned long long>(cv_key), count,
                     static_cast<unsigned long long>(ctx.x[2]),
                     static_cast<unsigned long long>(ctx.x[3]),
                     static_cast<unsigned long long>(ctx.x[4]));
    }
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

} // namespace g610
