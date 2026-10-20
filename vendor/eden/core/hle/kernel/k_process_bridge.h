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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "g610/guest_memory.hpp"
#include "vendor/eden/common/common_types.h"   // Vaddr, Handle
#include "vendor/eden/core/memory_bridge.h"          // `Core::Memory::Memory`, a nossa
#include "vendor/eden/core/hle/kernel/k_page_table_bridge.h"  // a pagina unica
#include "vendor/eden/core/hle/kernel/k_kernel_core_bridge.h"
#include "vendor/eden/core/hle/kernel/k_synchronization_object.h"

namespace Kernel {

class KProcess;
class KThread;

// Copiado de `core/hle/kernel/memory_types.h:13-14` do Eden.
//
// `k_server_session.cpp:719` usa `PageSize` para calcular o tamanho maximo de
// uma copia rapida, e o Eden nao o declara em nenhum dos headers que
// vendorizámos -- vem de `memory_types.h`, que nao foi vendorizado.
//
// Os valores sao do Eden, nao nossos: `PageBits = 12` da a pagina de 4 KiB que
// o Switch usa. Um valor diferente aqui seria uma invencao que so apareceria
// como um calculo de tamanho errado, muito longe da causa.
constexpr std::size_t PageBits{12};
constexpr std::size_t PageSize{1 << PageBits};

// Declarada aqui porque `KThread::BeginWait` a recebe como parametro, e a
// classe vem **depois** no ficheiro. Um ponteiro para um tipo incompleto e'
// valido numa declaracao -- e e' por isso que a declaracao chega.
class KThreadQueue;

// Copiado de `k_thread.h:108-116` do Eden, enumeradores e tudo.
//
// Não há aqui um `k_thread.h` para vir buscar, e o enum entra como **parâmetro**
// de um método (`SetWaitReasonForDebugging`), o que obriga a que exista um tipo
// com este nome e estes enumeradores. Copiar o enum é mais barato que vendorizar
// o ficheiro inteiro, e é o que fica a menos divergir: se o Eden acrescentar um
// enumerador, este ficheiro deixa de compilar — que é o aviso correcto.
enum class ThreadWaitReasonForDebugging : u32 {
    None,            ///< Thread is not waiting
    Sleep,           ///< Thread is waiting due to a SleepThread SVC
    IPC,             ///< Thread is waiting for the reply from an IPC request
    Synchronization, ///< Thread is waiting due to a WaitSynchronization SVC
    ConditionVar,    ///< Thread is waiting due to a WaitProcessWideKey SVC
    Arbitration,     ///< Thread is waiting due to a SignalToAddress/WaitForAddress SVC
    Suspended,       ///< Thread is waiting due to process suspension
};

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
// **A classe vive em `vendor/eden/core/memory_bridge.h`, no namespace
// `Core::Memory`**, e aqui fica um `using` para não mudar o resto da ponte.
//
// O nome e o namespace não são nossos: `hle_ipc.h:427` tem um membro do tipo
// `Core::Memory::Memory&`, e é esse nome que o código vendorizado escreve.
// Ver a nota longa em `memory_bridge.h`.
//
// O `using` é, em vez de mover a classe para aqui, a diferença entre uma
// classe que o Eden reconhece e uma classe que o nosso código reconhece.
using Memory = ::Core::Memory::Memory;

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
             ::g610::GuestMemory& mem)
        : KSynchronizationObject(kernel), handles_(handles), limits_(limits), pid_(pid),
          memory_(mem) {}

    KHandleTable& GetHandleTable() const { return handles_; }


    // PONTEIRO, como no Eden (k_process.h:316). O codigo do servico usa
    // `->` em cadeia (`...GetResourceLimit()->Reserve(...)`), e uma
    // referencia la da "unable to call member function using '->'".
    KResourceLimit* GetResourceLimit() const { return &limits_; }

    u64 GetProcessId() const { return pid_; }

    // O `ProgramId` — o id do **titulo** (o NCA), que no Eden vem do
    // `k_process.h` e e' lido pelo `Core::System::GetApplicationProcessProgramID`
    // (`core.cpp:729-731`).
    //
    // **Este motor nao tem o id do NCA.** O `ProgramId` e' uma coisa do
    // emulador de ficheiros (vem do `.nca` que o rtld carrega), e um
    // motor de execucao nativa carrega o programa ja decomposto, sem
    // metadados de titulo.
    //
    // O `pctl` usa-o para encher a `ApplicationInfo` do estado — e essa
    // leitura e' **opcional no Eden tambem** (`if (program_id != 0)`).
    // Com zero, o Eden nao preenche a `ApplicationInfo`, e o `pctl`
    // comporta-se como se o jogo nao tivesse classificacao parental — que
    // e' o caso normal.
    u64 GetProgramId() const { return 0; }

    // O tamanho do buffer de ponteiros, que no Eden e' um campo do `KProcess`
    // (k_process.h:104, 262-268) e nao da thread. Le-o o `Controller` do `sm`
    // (sm_controller.cpp:78), que e' construido no construtor do
    // `ServiceManager` -- ou seja, sempre.
    //
    // O valor por omissao e' o do Eden, 0x8000. Nao e' uma medicao para o
    // nosso caso: e' o mesmo numero, e mudar a hora de meter um diferente
    // seria uma diferenca sem motivo. Quem o muda e' o proprio Eden, a
    // inicializar o processo.
    u32 GetPointerBufferSize() const { return pointer_buffer_size_; }
    void SetPointerBufferSize(u32 size) { pointer_buffer_size_ = size; }

    // `KSynchronizationObject::IsSignaled` e' virtual puro
    // (k_synchronization_object.h:35), e o Eden implementa-o em cada
    // subclasse. Aqui o processo nunca e' sinalizado -- nao ha thread para
    // esperar por nada -- e a resposta honesta e' `false`.
    //
    // Devolver `true` seria perigoso: `KSynchronizationObject::Wait` trataria
    // o processo como pronto e devolveria na hora, e um pedido que devia ter
    // esperado passaria sem ter esperado.
    bool IsSignaled(KernelCore&) const override { return false; }

    // `KLightSession::Close` chama isto (k_light_session.cpp:81). No Eden
    // devolve o recurso reservado ao `KResourceLimit` (k_process.cpp:746-756).
    // Aqui faz o mesmo — a Implementacao esta no `eden_bridge.cpp`, porque
    // `KResourceLimit` so esta completo la: incluir `k_resource_limit.h` na
    // ponte fecharia o ciclo
    // `ponte -> k_resource_limit.h -> k_light_lock.h -> ponte`.
    void ReleaseResource(KernelCore& kernel, Svc::LimitableResource which, s64 value);
    void ReleaseResource(KernelCore& kernel, Svc::LimitableResource which, s64 value, s64 hint);

    Memory& GetMemory() { return memory_; }

    // A page table do processo. No Eden e' uma tabela de verdade
    // (`k_process.h:524`); aqui e' a ponte de pagina unica, e a razao e' uma
    // so: **o endereco do guest e' o endereco do host**. Ver
    // `k_page_table_bridge.h`.
    //
    // Fica DEPOIS de `memory_` na declaracao, e por isso e' inicializada
    // depois dela -- a lista de membros e' a ordem de construcao, e a page
    // table precisa da memoria para existir.
    KProcessPageTable& GetPageTable() { return page_table_; }

private:
    KHandleTable& handles_;
    KResourceLimit& limits_;
    u64 pid_;
    u32 pointer_buffer_size_ = 0x8000;
    Memory memory_;
    // A memoria, e nao a tabela de handles: e' a `Memory` que o `GuestMemory`
    // nosso envolve, e e' ela que a page table devolve pelo `GetMemory()`.
    KProcessPageTable page_table_{memory_};
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

    // A lista de quem espera por esta thread.
    //
    // No Eden é uma lista intrusiva de `KThread`, e o
    // `ThreadQueueImplForKLightServerSession` tira de lá a thread que acordou
    // (`k_light_server_session.cpp:29-30`).
    //
    // **Aqui é uma lista a sério** — um `std::vector<void*>` com os mesmos
    // nomes — e não um tipo vazio. A diferença é que, se alguma vez algo lhe
    // empurrar uma thread, o `erase` a remove a sério. Um `struct {}` com
    // operações vazias compilaria igual e seria uma mentira com pernas de
    // não: o `erase` apagaria uma thread que se metesse na fila e o código
    // continuaria a acreditar que não há ali ninguém.
    //
    // Num motor de execução nativa não há ninguém a empurrar nada. A lista
    // fica vazia, e fica vazia de facto.
    struct WaiterList {
        // **Guarda `KThread*`, não `void*`.**
        //
        // A primeira versão guardava `void*`, e o compilador queixou-se em
        // `k_light_server_session.cpp:178`: `cannot convert void** to
        // KThread*`. A lista é iterada com `std::addressof(*head)`, e
        // desreferenciar um `void*` não dá uma `KThread&` — dá um erro.
        //
        // Um vector de `void*` é mais simples de escrever e é uma lista sem
        // tipo: guarda coisas, não threads. Como só há um tipo de coisa que
        // entra nesta lista, guardá-lo é o mesmo trabalho com um tipo a menos
        // de erro.
        using value_type = KThread;

        // O iterador **desreferencia para `KThread&`**, e não para
        // `KThread*`.
        //
        // É a diferença entre um `std::vector` e uma lista intrusiva, e
        // `k_light_server_session.cpp:178` depende dela:
        //
        //     m_current_request = std::addressof(*head);
        //
        // Com o iterador do `vector`, `*head` é um `KThread*`, e
        // `std::addressof` de um `KThread*` é um `KThread**`. O erro é
        // "cannot convert KThread** to KThread*" — que não diz nada sobre
        // iteradores, e é por isso que custou tempo.
        //
        // Com `IntrusiveList` do Eden, `*head` já é a thread.
        class iterator {
        public:
            using base = std::vector<KThread*>::iterator;

            iterator() = default;
            explicit iterator(base it) : it_(it) {}

            KThread& operator*() const { return **it_; }
            KThread* operator->() const { return *it_; }

            iterator& operator++() {
                ++it_;
                return *this;
            }
            iterator operator++(int) {
                iterator antes = *this;
                ++it_;
                return antes;
            }

            bool operator==(const iterator& o) const { return it_ == o.it_; }
            bool operator!=(const iterator& o) const { return it_ != o.it_; }

            base interno() const { return it_; }

        private:
            base it_{};
        };

        iterator begin() { return iterator(elementos_.begin()); }
        iterator end() { return iterator(elementos_.end()); }

        // Constante: `*head` dá `const KThread&`, como deve ser.
        class const_iterator {
        public:
            using base = std::vector<KThread*>::const_iterator;

            const_iterator() = default;
            explicit const_iterator(base it) : it_(it) {}

            const KThread& operator*() const { return **it_; }
            const KThread* operator->() const { return *it_; }

            const_iterator& operator++() {
                ++it_;
                return *this;
            }
            bool operator==(const const_iterator& o) const { return it_ == o.it_; }
            bool operator!=(const const_iterator& o) const { return it_ != o.it_; }

        private:
            base it_{};
        };
        const_iterator begin() const { return const_iterator(elementos_.begin()); }
        const_iterator end() const { return const_iterator(elementos_.end()); }

        void push_back(KThread& thread) { elementos_.push_back(std::addressof(thread)); }

        iterator iterator_to(KThread& thread) {
            auto it = std::find(elementos_.begin(), elementos_.end(), std::addressof(thread));
            return iterator(it == elementos_.end() ? elementos_.end() : it);
        }

        // O `erase` tem de voltar ao iterador do `vector` antes de apagar.
        // Comparar o iterador adaptador com `elementos_.end()` daria "no
        // match for operator!=(WaiterList::iterator, vector<KThread*>::iterator)",
        // que é um erro escrito por um lado e por outro do mesmo tipo — o que
        // é sempre um sinal de que falta uma conversão, e a conversão que
        // falta chama-se `interno()`.
        void erase(iterator it) {
            auto interno = it.interno();
            if (interno != elementos_.end())
                elementos_.erase(interno);
        }

        bool empty() const { return elementos_.empty(); }
        std::size_t size() const { return elementos_.size(); }

    private:
        std::vector<KThread*> elementos_;
    };

    // ── a espera ──────────────────────────────────────────────────────────
    //
    // Estes métodos existem porque o `KLightServerSession` os chama
    // (`k_light_server_session.cpp:56-75`), e o `KLightServerSession` existe
    // porque o `KClientPort` o menciona.
    //
    // **E é aqui que a costura quase caiu.** A resposta natural era puxar o
    // `k_thread.h` do Eden e ficar com os métodos todos — e o `k_thread.h` tem
    // 1001 linhas e arrasta o `k_scheduler`, as filas e os temporizadores.
    //
    // Em vez disso ficam aqui, e o que sobrevive é a **pergunta que cada um
    // faz**, não o seu comportamento:
    //
    //     IsWaitCancelled()      -> nunca se cancela      -> false
    //     IsCancellable()        -> nunca é cancelável    -> false
    //     IsTerminationRequested -> nunca é terminado    -> false
    //     GetWaitResult()        -> nunca se esperou      -> sucesso
    //     BeginWait/EndWait      -> nunca há quem espere
    //
    // Cada resposta é a que o Eden daria **se ninguém esperasse**, e é isso que
    // acontece: um pedido de serviço é atendido dentro do `SendSyncRequest` e
    // a thread do guest não pára nunca.
    void BeginWait(KernelCore& /*kernel*/, KThreadQueue* /*queue*/) {}
    void EndWait(KernelCore& /*kernel*/, Result /*wait_result*/) {}
    void CancelWait(KernelCore& /*kernel*/, Result /*wait_result*/, bool /*cancel_timer_task*/) {}
    void NotifyAvailable(KernelCore& /*kernel*/, KSynchronizationObject* /*obj*/, Result) {}
    void WaitCancel(KernelCore& /*kernel*/) {}

    Result GetWaitResult() const { return ResultSuccess; }
    bool IsWaitCancelled() const { return false; }
    void ClearWaitCancelled() {}
    bool IsCancellable() const { return false; }
    void SetCancellable() {}
    void ClearCancellable() {}
    void SetWaitReasonForDebugging(ThreadWaitReasonForDebugging /*reason*/) {}
    bool IsTerminationRequested() const { return false; }
    s32 GetNumKernelWaiters() const { return 0; }

    u32* GetLightSessionData() const { return light_session_data_; }
    void SetLightSessionData(u32* data) { light_session_data_ = data; }

    // `KSynchronizationObject::IsSignaled` é virtual puro
    // (`k_synchronization_object.h:35`) e o Eden implementa-o em cada
    // subclasse. Sem isto o `KThread` fica **abstracto** e o
    // `make_unique<KThread>` dá "invalid new-expression of abstract class
    // type" — que não diz que falta um método, e é por isso que vale a pena
    // deixar o método no mesmo sítio do `KProcess` e não no fim do ficheiro.
    //
    // Mesma resposta que no `KProcess`: `false`. A thread do guest nunca está
    // sinalizada, porque nada a sinaliza — um pedido de serviço é atendido
    // dentro do `SendSyncRequest`, e a thread não pára nunca.
    bool IsSignaled(KernelCore&) const override { return false; }

    // **PONTEIRO, não referência.** O Eden devolve `KProcess*`
    // (`k_thread.h:320`) e o código usa `auto* const cur_process = ...`, que
    // com uma referência não deduce nada. Ver a nota no `KProcess` sobre
    // assinaturas: uma assinatura diferente da do Eden dá "unable to deduce",
    // e não dá nenhuma pista de que a assinatura é que está errada.
    KProcess* GetOwnerProcess() const { return &process_; }

    u64 GetThreadId() const { return thread_id_; }
    void SetThreadId(u64 id) { thread_id_ = id; }

    EnderecoGuest GetTlsAddress() const { return tls_address_; }
    void SetTlsAddress(EnderecoGuest a) { tls_address_ = a; }

private:
    KProcess& process_;
    u64 thread_id_ = 0;
    u32* light_session_data_ = nullptr;
    EnderecoGuest tls_address_ = 0;
};

// O escalonador, em nome próprio.
//
// `k_readable_event.cpp:30` tem um `ASSERT(KScheduler::IsSchedulerLockedByCurrentThread(kernel))`
// — e sem este tipo o ficheiro não compila.
//
// **A pergunta não tem resposta neste motor, e a resposta é sempre `true`.**
// A pergunta é «esta thread tem o bloqueio do escalonador?», e o bloqueio do
// escalonador é o nosso `KScopedSchedulerLock`, que é um no-op. Um no-op que
// está sempre tomado é um no-op que está sempre tomado: a asserção que ele
// protege não tem conteúdo num motor sem troca de contexto, e passa — como
// uma asserção correcta passaria.
class KScheduler {
public:
    static bool IsSchedulerLockedByCurrentThread(KernelCore&) { return true; }
};

// ── a fila de espera ───────────────────────────────────────────────────────
//
// `KThreadQueue` é a base de que o `KLightServerSession` deriva a sua fila de
// pedidos (`k_light_server_session.cpp:19-32`). É a **única** vez que a camada
// de IPC do Eden precisa da fila de esperas do escalonador, e o que precisa
// é a interface — três virtuais e um construtor.
//
// A implementação são no-ops, pela razão que vale para o `Broadcast` e para o
// `Wait`: **neste motor não há thread que espere**. O `KThread::WaiterList` da
// ponte é um tipo vazio que existe para o código compilar, e esta fila é a
// mesma coisa com mais métodos.
//
// Se algum dia aparecer uma segunda thread que espere por um pedido de serviço,
// estas três funções passam a estar erradas — e o sintoma é um pedido que
// nunca é atendido. É o sítio a mexer, e está marcado.
class KThreadQueue {
public:
    // O Eden inicializa aqui o `m_hardware_timer` (`k_thread_queue.h:18`),
    // que é o timer de hardware usado para os tempos limite de espera.
    // Aqui **não há** esse membro: medido, `SetHardwareTimer` não é chamado
    // por nada que vendorizámos, e o `KThreadQueue` do Eden — que é quem o
    // tinha — ficou de fora. Um membro que ninguém escreve e ninguém lê é um
    // membro que está a pedir para ser confundido com o timer que o
    // `KernelCore` realmente tem, o `HardwareTimer()`.
    explicit KThreadQueue(KernelCore& kernel) {}
    virtual ~KThreadQueue() = default;

    virtual void NotifyAvailable(KernelCore&, KThread*, KSynchronizationObject*, Result) {}
    virtual void EndWait(KernelCore&, KThread*, Result) {}
    virtual void CancelWait(KernelCore&, KThread*, Result, bool) {}
};

class KThreadQueueWithoutEndWait : public KThreadQueue {
public:
    explicit KThreadQueueWithoutEndWait(KernelCore& kernel) : KThreadQueue(kernel) {}
    void EndWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result) override final;
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

// `GetInteger` extrai o valor de um `TypedAddress` (`common/typed_address.h`).
//
// **A nossa `KThread::GetTlsAddress` devolve um `std::uint64_t`, não um
// endereço tipado** — a ponte usa `std::uint64_t` para não depender do pacote
// externo de onde vem `Vaddr` (ver a nota no topo deste ficheiro). Por isso
// esta sobrecarga existe, e é a identidade.
//
// Duas sobrecargas e não uma genérica: uma genérica aceitaria `uint64_t` e
// `TypedAddress` com o mesmo corpo, e o corpo de uma tem de ser diferente do
// corpo da outra — `TypedAddress` precisa de `GetValue()`.
inline std::uint64_t GetInteger(std::uint64_t address) { return address; }
inline std::uint64_t GetInteger(Common::ProcessAddress address) { return address.GetValue(); }

inline KProcess* GetCurrentProcessPointer(KernelCore&) { return Bridge::processo(); }
inline KProcess& GetCurrentProcess(KernelCore&) { return *Bridge::processo(); }
inline KThread* GetCurrentThreadPointer(KernelCore&) { return Bridge::thread(); }
inline KThread& GetCurrentThread(KernelCore&) { return *Bridge::thread(); }
inline Memory& GetCurrentMemory(KernelCore&) { return Bridge::processo()->GetMemory(); }

} // namespace Kernel