// A PONTE. Onde o motor nativo se liga ao sistema do Eden.
//
// Ver `k_kernel_core_bridge.h` para o `KernelCore` mínimo e a nota sobre a
// ordem dos includes, que é a parte que custou tempo.
//
// ## O que este ficheiro substitui
//
// `k_process.h` e `k_thread.h` do Eden.
//
// ## Porquê
//
// A camada de IPC do Eden pede ao processo **quatro coisas**, e só quatro
// (`ipc_helpers.h`, `hle_ipc.cpp`, `server_manager.cpp`, `sm.cpp`):
//
//     GetOwnerProcess()->GetHandleTable()
//     GetOwnerProcess()->GetProcessId()
//     GetCurrentProcess(...).GetResourceLimit()
//     GetCurrentMemory(...)  ->  ReadCString(), Read<T>(), Write<T>()
//
// O resto do `k_process.h` — page tables, reserva de recursos, metadata do
// programa, interfaces de CPU — não é tocado por nada da camada de IPC.
//
// Vendorizar o fecho tal e qual foi o erro: levou-o de 6 ficheiros para 130 e
// 25.000 linhas, arrastando `cpu_manager`, `k_scheduler`, `page_table` (dez),
// `fiber`, `device_memory` e `arm_interface`. Ou seja, o emulador inteiro —
// que é o que este projecto NÃO quer. A causa era **um** `#include`.
//
// ## A regra da cadeia
//
// Este ficheiro é o topo. Inclui o `KernelCore` mínimo, o
// `KSynchronizationObject` do Eden (que é de onde vem a herança e os
// class-tokens), e **declara à frente** `KHandleTable` e `KResourceLimit` —
// usados só por referência. Os `.cpp` do Eden incluem este ficheiro; este
// ficheiro não inclui nenhum deles.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "ia/guest_memory.hpp"
#include "vendor/eden/common/common_types.h"   // Vaddr, Handle
#include "vendor/eden/core/hle/kernel/k_kernel_core_bridge.h"
#include "vendor/eden/core/hle/kernel/k_synchronization_object.h"

namespace Kernel {

class KProcess;
class KThread;

// A tabela e os limites só são usados por REFERÊNCIA. Declarados à frente de
// propósito: incluí-los aqui fecha o ciclo
//
//     k_process_bridge.h -> k_handle_table.h -> k_process_bridge.h
//
// e com `#pragma once` um dos dois fica com o corpo vazio.
class KHandleTable;
class KResourceLimit;

// O Eden usa `Vaddr` para um endereço do guest. Esse typedef não está em
// nenhum dos ficheiros que o fecho copiou — vem de um pacote externo que não
// está aqui. Em vez de depender dele, a ponte usa `std::uint64_t`, que é o que
// `Vaddr` é: o endereço é um u64, e o espaço de 39 bits é uma convenção do
// endereçamento, não do tipo. Menos dependências, a mesma semântica.
using EnderecoGuest = std::uint64_t;

// ── a memória ──────────────────────────────────────────────────────────────
//
// O Eden tem um `Memory` que sabe ler e escrever a memória do guest. O nosso
// é o `GuestMemory`, que faz o mesmo sobre a reserva mapeada. Este adaptador
// é a cola entre os dois nomes.
//
// Não é uma herança: o `Memory` do Eden tem operações que nós não temos
// (páginas, protecções, tradução) porque não temos page tables — a memória
// do guest é um bloco contíguo nosso. O que fica abaixo é **a parte de que a
// camada de IPC usa**, e nada mais.
class Memory {
public:
    explicit Memory(::ia::GuestMemory& mem) : mem_(mem) {}

    template <typename T>
    T Read(EnderecoGuest address) const {
        T v{};
        std::string err;
        if (!mem_.read(address, &v, sizeof(T), err))
            return T{};
        return v;
    }

    template <typename T>
    void Write(EnderecoGuest address, T value) {
        std::string err;
        (void)mem_.write(address, &value, sizeof(T), err);
    }

    // Usado pelo `sm:` para ler o nome do porto. O limite é o mesmo do Eden
    // (`KObjectName::NameLengthMax`): um nome que não acaba é um nome
    // inválido, e ler sem limite memória do guest é ler fora da reserva.
    std::string ReadCString(EnderecoGuest address) const {
        return ReadCString(address, 33);
    }

    std::string ReadCString(EnderecoGuest address, std::size_t max_length) const {
        if (max_length == 0 || max_length > 256)
            return {};
        char buf[256];
        std::string err;
        if (!mem_.read(address, buf, max_length, err))
            return {};
        buf[max_length - 1] = '\0';
        return std::string(buf);
    }

    bool ReadBytes(EnderecoGuest address, void* dest, std::size_t size) const {
        std::string err;
        return mem_.read(address, dest, size, err);
    }
    bool WriteBytes(EnderecoGuest address, const void* src, std::size_t size) {
        std::string err;
        return mem_.write(address, src, size, err);
    }

private:
    ::ia::GuestMemory& mem_;
};

// ── o processo ─────────────────────────────────────────────────────────────
//
// QUATRO acessos. E só. A herança de `KSynchronizationObject` não é
// enfeite: é o que faz `KScopedAutoObject<KAutoObject>` aceitar um
// `KProcess*`, e é de onde vem `KERNEL_AUTOOBJECT_TRAITS` — o class-token que
// o `DynamicCast` do Eden usa.
//
// A herança tem um preço: `KSynchronizationObject` chama `KAutoObject(kernel)`
// no construtor, e portanto um `KProcess` só pode nascer com um
// `KernelCore&` à mão. É a cadeia de montagem completa em
// `src/eden_bridge.cpp`.
class KProcess final : public KSynchronizationObject {
    KERNEL_AUTOOBJECT_TRAITS(KProcess, KSynchronizationObject);

public:
    KProcess(KernelCore& kernel, KHandleTable& handles, KResourceLimit& limits, u64 pid,
             ::ia::GuestMemory& mem)
        : KSynchronizationObject(kernel), handles_(handles), limits_(limits), pid_(pid),
          memory_(mem) {}

    KHandleTable& GetHandleTable() const { return handles_; }


    // PONTEIRO, como no Eden (k_process.h:316). O codigo do servico usa
    // `->` em cadeia (`...GetResourceLimit()->Reserve(...)`), e uma
    // referencia la da "unable to call member function using '->'".
    KResourceLimit* GetResourceLimit() const { return &limits_; }

    u64 GetProcessId() const { return pid_; }

    Memory& GetMemory() { return memory_; }

private:
    KHandleTable& handles_;
    KResourceLimit& limits_;
    u64 pid_;
    Memory memory_;
};

// ── a thread ───────────────────────────────────────────────────────────────
//
// A thread do guest é a NOSSA thread principal: o motor é um loop de traps
// síncrono, não há `pthread_create`, e o guest corre em `NativeEngine::run`.
//
// Por isso `GetCurrentThread` é uma instância única com setas, e não uma
// tabela de threads para consultar. Fingir que há tabela seria mentir sobre o
// que este motor é.
class KThread final : public KSynchronizationObject {
    KERNEL_AUTOOBJECT_TRAITS(KThread, KSynchronizationObject);

public:
    KThread(KernelCore& kernel, KProcess& process) : KSynchronizationObject(kernel),
                                                    process_(process) {}

    // A lista de quem espera por esta thread. O Eden usa-a para o
    // escalonamento; aqui é um tipo que existe para o
    // `k_light_condition_variable.h` compilar, e fica sempre vazia. Não é uma
    // coisa que "falta" — não há nada para esperar, porque o guest corre noutra
    // thread e quem espera aqui é o próprio motor.
    struct WaiterList {};

    // **PONTEIRO, não referência.** O Eden devolve `KProcess*` e usa
    // `auto* const cur_process = cur_thread->GetOwnerProcess();` — que com uma
    // referência não deduz nada e dá "unable to deduce 'auto* const'".
    // A assinatura tem de bater com a do Eden; não é um detalhe.
    KProcess* GetOwnerProcess() const { return &process_; }

    u64 GetThreadId() const { return thread_id_; }
    void SetThreadId(u64 id) { thread_id_ = id; }

    EnderecoGuest GetTlsAddress() const { return tls_address_; }
    void SetTlsAddress(EnderecoGuest a) { tls_address_ = a; }

private:
    KProcess& process_;
    u64 thread_id_ = 0;
    EnderecoGuest tls_address_ = 0;
};

// ── a ponte ────────────────────────────────────────────────────────────────
//
// Um conjunto, posto pelo nosso motor antes de o guest arrancar. É o
// equivalente aos "current" que o Eden guarda dentro do `KernelCore::Impl`.
//
// Dois ponteiros, e não um objecto com estado: o que se quer é aceder a "o
// processo" e "a thread", e um owner pointer é a forma mais honesta de dizer
// isso.
class Bridge {
public:
    static void instalar(KProcess* processo, KThread* thread) {
        g_processo = processo;
        g_thread = thread;
    }
    static void limpar() {
        g_processo = nullptr;
        g_thread = nullptr;
    }
    static KProcess* processo() { return g_processo; }
    static KThread* thread() { return g_thread; }
    static bool instalado() { return g_processo != nullptr && g_thread != nullptr; }

private:
    static inline KProcess* g_processo = nullptr;
    static inline KThread* g_thread = nullptr;
};

inline KProcess* GetCurrentProcessPointer(KernelCore&) { return Bridge::processo(); }
inline KProcess& GetCurrentProcess(KernelCore&) { return *Bridge::processo(); }
inline KThread* GetCurrentThreadPointer(KernelCore&) { return Bridge::thread(); }
inline KThread& GetCurrentThread(KernelCore&) { return *Bridge::thread(); }
inline Memory& GetCurrentMemory(KernelCore&) { return Bridge::processo()->GetMemory(); }

} // namespace Kernel