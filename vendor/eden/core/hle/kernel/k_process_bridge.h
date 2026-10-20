// A PONTE. Onde o motor nativo se liga ao sistema do Eden.
//
// ## O que este ficheiro substitui
//
// `k_process.h` e `k_thread.h` do Eden.
//
// ## Porquê
//
// A camada de IPC do Eden pede ao processo **quatro coisas**. Só quatro, e
// estão em `ipc_helpers.h`, `hle_ipc.cpp`, `server_manager.cpp` e `sm.cpp`:
//
//     GetOwnerProcess()->GetHandleTable()
//     GetOwnerProcess()->GetProcessId()
//     GetCurrentProcess(...).GetResourceLimit()
//     GetCurrentMemory(...)  ->  ReadCString(), Read<T>(), Write<T>()
//
// O resto do `k_process.h` — page tables, reserva de recursos, metadata do
// programa, interfaces de CPU — não é tocado por nada da camada de IPC.
//
// ## O que acontece se não se cortar
//
// Medido. A primeira tentativa foi vendorizar o fecho tal e qual, e o fecho
// foi de 6 para **130 ficheiros e 25.000 linhas**, arrastando:
//
//     core/cpu_manager.h        core/hle/kernel/k_scheduler.h
//     core/hle/kernel/physical_core.h
//     core/hle/kernel/k_page_table*.h    (dez ficheiros)
//     core/device_memory.h     common/fiber.h
//     core/arm/arm_interface.h
//
// ou seja, o emulador inteiro — que é exactamente o que este projecto NÃO
// quer. A CPU corre nativamente; a única tradução é Maxwell → Valhall.
//
// ## A regra
//
// O processo aqui **é o nosso motor nativo**, não o do Eden. Por isso este
// ficheiro não finge ser o `KProcess` do Eden: é uma versão do que o IPC
// precisa, sustentada pela nossa `GuestMemory` e pela nossa thread. E diz
// isso em cada tipo, para que ninguém cometa o erro de achar que o resto do
// processo está aqui.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "vendor/eden/common/common_types.h"   // Vaddr, Handle

#include "ia/guest_memory.hpp"

namespace Kernel {

// Declarado a frente de proposito: se a ponte includesse `kernel.h`, e
// `kernel.h` includisse a ponte, o include cycle deixaria `KThread` sem
// definicao em qualquer ponto onde os dois fossem usados em conjunto.
class KernelCore;
class KProcess;

// A tabela e os limites so'ao usados por REFERENCIA, e sao definicoes
// completas que includes esta ponte. Declarados a frente e' o que quebra o
// ciclo:
//
//     k_process_bridge.h -> k_handle_table.h -> k_process_bridge.h
//
// Com `#pragma once` num ciclo desse genre, um dos dois fica com o corpo
// vazio e o compilador diz que o tipo nao existe — sem nenhuma pista de
// que a causa e' um ciclo. Ja aconteceu duas vezes aqui.
class KHandleTable;
class KResourceLimit;

// O Eden usa `Vaddr` para um endereco do guest. Esse typedef nao esta em
// nenhum dos ficheiros que o fecho copiou — vem de um pacote externo que
// nao esta' aqui. Em vez de depender dele, a ponte usa `std::uint64_t`, que
// e' o que `Vaddr` e': o endereco e' um u64, e o espaco de 39 bits e' uma
// convencao do enderecamento, nao do tipo.
//
// Menos dependencias, e a mesma semantica. Um typedef que nao esta no
// repositorio e' um typedef que muda de lugar sem ninguem dar por isso.
using EnderecoGuest = std::uint64_t;

// ── a memória ──────────────────────────────────────────────────────────────
//
// O Eden tem `core/memory.h`, com um `Memory` que sabe ler e escrever a
// memória do guest. O nosso é o `GuestMemory`, que faz o mesmo sobre a
// reserva mapeada. Este adaptador é a Cola entre os dois nomes.
//
// Não é uma herança: `Memory` no Eden é uma classe com muitas operações
// (páginas, protecções, translation) que nós não temos porque não temos
// page tables — a memória do guest é um bloco contíguo nosso. O que fica
// abaixo é **a parte de que a camada de IPC usa**, e nada mais.
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

    // `ReadCString` — usado pelo `sm:` para ler o nome do porto.
    //
    // O limite é o mesmo do Eden (`KObjectName::NameLengthMax`): um nome de
    // serviço que não acaba é um nome invalido, e ler sem limite um buffer do
    // guest é ler fora da reserva.
    std::string ReadCString(EnderecoGuest address) const {
        constexpr std::size_t kMax = 33;
        char buf[kMax];
        std::string err;
        if (!mem_.read(address, buf, kMax, err))
            return {};
        buf[kMax - 1] = '\0';
        return std::string(buf);
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

    // O Eden tambem le/escreve objectos com tamanho fixo; os servicos usam
    // este par para os handles e para os pedidos de I/O.
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
// QUATRO acessos. E so. Cada um tem uma linha a dizer de onde vem, porque
// um `KProcess` com um quinto membro que ninguem pediu e' um `KProcess` que
// ja' virou emulador.
//
//   GetHandleTable()   <- a tabela do Eden, vendorizada tal e qual
//   GetProcessId()     <- o nosso PID
//   GetResourceLimit() <- o Eden, vendorizado tal e qual
//
class KProcess {
public:
    KProcess(KHandleTable& handles, KResourceLimit& limits, u64 pid,
             ::ia::GuestMemory& mem)
        : handles_(handles), limits_(limits), pid_(pid), memory_(mem) {}

    KHandleTable& GetHandleTable() { return handles_; }
    const KHandleTable& GetHandleTable() const { return handles_; }

    KResourceLimit& GetResourceLimit() { return limits_; }
    const KResourceLimit& GetResourceLimit() const { return limits_; }

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
// A thread do guest é a NOSSA thread principal: o motor e' um loop de traps
// sincrono, nao ha `pthread_create`, e o guest corre em `NativeEngine::run`.
//
// E' por isso que `GetCurrentThread` e' uma unica instancia com setas —
// nao ha tabela de threads para consultar, e fingir que ha seria mentir sobre
// o que este motor e'.
class KThread {
public:
    // A lista de quem espera por esta thread. O Eden usa-a para o
    // escalonamento e para o adiar; aqui e' um tipo que existe para o
    // `k_light_condition_variable.h` compilar, e que fica sempre vazia.
    // Nao e' uma coisa que "falta": nao ha nada para esperar, porque o
    // guest corre noutra thread e quem espera aqui e' o proprio motor.
    struct WaiterList {};

    explicit KThread(KProcess& process) : process_(process) {}

    // `DisableDispatch` desliga a troca de contexto enquanto se segura um
    // lock. Sem scheduler nao ha troca de contexto: a thread do guest corre
    // sempre, e o host que cede e' o nosso motor, nao o Eden.
    void DisableDispatch(KernelCore&) {}

    KProcess& GetOwnerProcess() const { return process_; }
    u64 GetThreadId() const { return thread_id_; }
    void SetThreadId(u64 id) { thread_id_ = id; }

    u64 GetTlsAddress() const { return tls_address_; }
    void SetTlsAddress(u64 a) { tls_address_ = a; }

private:
    KProcess& process_;
    u64 thread_id_ = 0;
    u64 tls_address_ = 0;
};

// ── a ponte ────────────────────────────────────────────────────────────────
//
// Um conjunto, posto pelo nosso motor antes de o guest arrancar. E' o
// equivalente aos globais `g_current_process` / `g_current_thread` que o
// Eden mantem dentro do `KernelCore::Impl`.
//
// Sao dois ponteiros e nao um objeto com estado, porque o que se quer e'
// aceder a "o processo" e "a thread", e um owner pointer e' a forma mais
// honesta de o dizer.
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

inline KProcess* GetCurrentProcessPointer(KernelCore&) {
    return Bridge::processo();
}

inline KProcess& GetCurrentProcess(KernelCore&) {
    return *Bridge::processo();
}

inline KThread* GetCurrentThreadPointer(KernelCore&) {
    return Bridge::thread();
}

inline KThread& GetCurrentThread(KernelCore&) {
    return *Bridge::thread();
}

inline Memory& GetCurrentMemory(KernelCore&) {
    return Bridge::processo()->GetMemory();
}

// ── o que o scheduler_existiria de fazer, e nao faz ───────────────────────
//
// `KScopedDisableDispatch` (k_thread.h:981-993):
//
//     explicit KScopedDisableDispatch(KernelCore& kernel) : m_kernel{kernel} {
//         if (m_kernel.IsShuttingDown()) { return; }
//         GetCurrentThread(kernel).DisableDispatch(kernel);
//     }
//
// No Eden, serve para o handle table nao ser mexido a meio de uma troca de
// contexto. Aqui nao ha troca de contexto — o guest corre na thread
// principal e o motor e' sincrono — por isso e' um no-op com o nome certo.
//
// E' a primeiraadaptacao onde se REMOVE comportamento. Cada uma delas esta
// aqui nomeada e comentada, porque "funciona porque o scheduler nao esta la"
// e' uma explicacao que ninguem da para o ano seguinte.
class KScopedDisableDispatch {
public:
    explicit KScopedDisableDispatch(KernelCore&) {}
};

} // namespace Kernel