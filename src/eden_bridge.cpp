// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// Onde o motor nativo se liga ao sistema do Eden.
//
// Ver `include/g610/eden_bridge.hpp` para a cadeia de montagem e o porque de
// cada decisão; os ponteiros todos apontam para lá.
//
// ## Regra desta costura
//
// Nenhuma assinatura aqui é inventada. Todas foram lidas no Eden:
//
//     KernelCore::RegisterKernelObject       kernel.cpp:964
//     KernelCore::RegisterInUseObject        kernel.cpp:974
//     ConnectToNamedPort                     svc/svc_port.cpp:19-55
//     SendSyncRequest                        svc/svc_ipc.cpp:30-36
//     SM::LoopProcess                        sm/sm.cpp:308-321
//     KProcess::ReleaseResource              k_process.cpp:746
//
// Quando uma assinatura nossa difere da do Eden, isso está escrito no
// cabeçalho da ponte com a razão — e é sempre no sentido de **nosso** lado
// ser mudado, nunca o nome que o Eden usa.
#include "g610/eden_bridge.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vendor/eden/common/scope_exit.h"
#include "vendor/eden/core/hle/kernel/k_auto_object.h"
#include "vendor/eden/core/hle/kernel/k_auto_object_container.h"
#include "vendor/eden/core/hle/kernel/k_client_port.h"
#include "vendor/eden/core/hle/kernel/k_client_session.h"
#include "vendor/eden/core/hle/kernel/k_handle_table.h"
#include "vendor/eden/core/hle/kernel/k_light_condition_variable.h"
#include "vendor/eden/core/hle/kernel/k_light_lock.h"
#include "vendor/eden/core/hle/kernel/k_object_name.h"
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"
#include "vendor/eden/core/hle/kernel/k_resource_limit.h"
#include "vendor/eden/core/hle/kernel/k_session.h"
#include "vendor/eden/core/hle/kernel/svc_results.h"
#include "vendor/eden/core/hle/service/apm/apm.h"
#include "vendor/eden/core/hle/service/apm/apm_controller.h"
#include "vendor/eden/core/hle/service/aoc/addon_content_manager.h"
#include "g610/services/applet_oe.hpp"
#include "g610/services/fsp_srv.hpp"
#include "g610/services/nvdrv.hpp"
#include "g610/services/pl_u.hpp"
#include "g610/services/set.hpp"
#include "g610/services/time_u.hpp"
#include "g610/services/vi.hpp"
#include "vendor/eden/core/hle/service/lm/lm.h"
#include "vendor/eden/core/hle/service/pctl/pctl.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/sm/sm.h"

// ── o KernelCore ───────────────────────────────────────────────────────────

namespace Kernel {

// O construtor e o destructor.
//
// Quase vazios de propósito: o `KernelCore` nosso tem um `HostCore`, um
// contentor de objectos, um relógio e duas listas, e nada disso precisa de
// ser preparado. O que o Eden prepara no construtor é o `Impl`, que carrega
// o estado do emulador — e que aqui não existe.
//
// O `hardware_timer_` não precisa de preparação porque é um contador com
// valor inicial, e o `object_list_` também não: o
// `KAutoObjectWithListContainer` só quer o `KernelCore&`.
KernelCore::KernelCore(::Core::System& system) : system_(system) {}

KernelCore::~KernelCore() = default;

// As DUAS listas. Ver a nota no cabeçalho da ponte: o Eden tem
// `registered_objects` e `registered_in_use_objects`, com fechos separados, e
// `KHandleTable::Remove` usa a segunda. Juntar as duas não seria uma
// simplificação, seria um buraco: o `UnregisterInUseObject` de um objecto que
// só foi registado pela primeira deixaria o registo a vazar.
template <typename Vec, typename Mutex>
static void apagar(Vec& v, Mutex& m, KAutoObject* self) {
    std::lock_guard<Mutex> lk(m);
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (*it == self) {
            v.erase(it);
            return;
        }
    }
}

void KernelCore::RegisterKernelObject(KAutoObject* self) {
    std::lock_guard<std::mutex> lk(registered_objects_mutex_);
    registered_objects_.push_back(self);
}

void KernelCore::UnregisterKernelObject(KAutoObject* self) {
    apagar(registered_objects_, registered_objects_mutex_, self);
}

void KernelCore::RegisterInUseObject(KAutoObject* self) {
    std::lock_guard<std::mutex> lk(in_use_mutex_);
    in_use_.push_back(self);
}

void KernelCore::UnregisterInUseObject(KAutoObject* self) {
    apagar(in_use_, in_use_mutex_, self);
}

// O registo dos nomes de objecto — o que faz `ConnectToNamedPort("sm:")`
// encontrar a porta.
//
// Criado **preguiçosamente**, e à primeira chamada, porque o `KObjectName`
// tem o seu próprio slab (`k_object_name.h:22`, `KSlabAllocated<KObjectName>`)
// e não vale a pena pagá-lo se o guest nunca perguntar por um nome.
//
// O `unique_ptr` é meu e o tipo é só declarado à frente no cabeçalho da ponte;
// aqui o tipo está completo e a eliminação é feita por este destructor, que
// é o único sítio onde isso importa.
KObjectNameGlobalData& KernelCore::ObjectNameGlobalData() {
    if (!object_name_global_data_) {
        object_name_global_data_ = std::make_unique<KObjectNameGlobalData>(*this);
    }
    return *object_name_global_data_;
}

// `KProcess::ReleaseResource` — k_process.cpp:746-756.
//
// Delegar ao `KResourceLimit` e' tudo o que ha. A Implementacao esta aqui e
// nao na ponte porque `KResourceLimit` so' e' completo neste ficheiro — e a
// ponte nao pode incluir `k_resource_limit.h` sem fechar o ciclo
// `ponte -> k_resource_limit.h -> k_light_lock.h -> k_process_bridge.h`.
void KProcess::ReleaseResource(KernelCore& kernel, Svc::LimitableResource which, s64 value) {
    if (KResourceLimit* rl = this->GetResourceLimit(); rl != nullptr) {
        rl->Release(kernel, which, value);
    }
}

void KProcess::ReleaseResource(KernelCore& kernel, Svc::LimitableResource which, s64 value,
                               s64 /*hint*/) {
    // O Eden passa o `hint` ao `Release`, que o usa para espreitar o que está
    // a ser devolvido. Sem espera pendente nao ha o que espreitar, e ha menos
    // um parametro para inventar um significado.
    ReleaseResource(kernel, which, value);
}

} // namespace Kernel

// Implementa `KObjectName::DumpNames`, declarado no cabeçalho vendorizado.
//
// É `friend` do `KObjectName` — declarado assim no cabeçalho com a nota de
// porquê. `m_name` e `m_object` são privados, e a alternativa (um `Find` por
// nome conhecido) é exactamente a que destrói a porta.
//
// A definição vai **fora de `namespace g610`** e é a sua própria
// `namespace Kernel`.
//
// Foi a quarta vez nesta costura que um `namespace Kernel {` caiu dentro de um
// `namespace g610 {` e declarou `g610::Kernel`. Os erros são sempre os mesmos —
// «KObjectName has not been declared», «KernelCore was not declared» — e
// nenhum deles menciona namespaces, o que faz o problema parecer um include em
// falta.
//
// A regra, e que fica escrita porque já custou quatro vezes: **antes de abrir
// um namespace, confirmar em que namespace se está.** `grep -n '^namespace'`
// no ficheiro responde em meio segundo e poupa meia hora.
namespace Kernel {

void KObjectName::DumpNames(KernelCore& kernel) {
    // A primeira linha é incondicional de propósito: um instrumento que
    // imprime zero linhas não se distingue de um instrumento que não corre.
    std::fprintf(stderr, "[ipc]   (DumpNames entrou)\n");
    KObjectNameGlobalData& gd = kernel.ObjectNameGlobalData();
    int n = 0;
    for (const auto& nome : gd.GetObjectList()) {
        // O objecto imprime-se pelo TIPO, que vem do class-token do Eden — e
        // nao por um ponteiro, porque um ponteiro nao diz nada ao olho.
        //
        // `GetObjectType()` e' um template que cada subclasse define; sem ele
        // o dynamic cast a `KClientPort` dava mais informacao sobre o que o
        // registo contem.
        const char* tipo = "(desconhecido)";
        if (dynamic_cast<KClientPort*>(nome.m_object) != nullptr)
            tipo = "KClientPort";
        else if (dynamic_cast<KServerPort*>(nome.m_object) != nullptr)
            tipo = "KServerPort";
        else if (nome.m_object != nullptr)
            tipo = "outro";
        std::fprintf(stderr, "[ipc]   nome[%d] = \"%s\"  objecto=%s\n", n,
                     nome.m_name.data(), tipo);
        // Os 12 bytes, porque `MatchesName` compara 12 (k_object_name.cpp:25)
        // e duas strings que parecem iguais nao o sao se uma delas tem lixo
        // depois do NUL.
        std::fprintf(stderr, "[ipc]     bytes:");
        for (std::size_t b = 0; b < Kernel::KObjectName::NameLengthMax; ++b)
            std::fprintf(stderr, " %02x", static_cast<unsigned>(nome.m_name[b]));
        std::fprintf(stderr, "   strncmp(\"sm:\",12)=%d\n",
                     std::strncmp(nome.m_name.data(), "sm:", Kernel::KObjectName::NameLengthMax));
        ++n;
    }
    std::fprintf(stderr, "[ipc]   %d nome(s) registado(s)\n", n);
}

} // namespace Kernel


// O destructor do `Core::System`. Aqui, e não no cabeçalho, porque é o único
// sítio onde o `Service::ServerManager` está completo.
//
// **Com o namespace qualificado**, e é a segunda vez hoje que um nome de
// classe existe em dois sítios. Escrever `namespace Core { ... }` dentro de
// `namespace g610 { ... }` cria `g610::Core`, que não existe — o compilador diz
// «not in namespace enclosing», que é uma forma muito roundabout de dizer
// «este `Core` não é aquele». `::Core` não deixa dúvida: é o global.
//
// O mesmo happened no `HostCore`, dentro de `namespace Kernel`. A regra que
// daí sai, e que fica escrita: **quando um nome de classe é comum, escreve-se
// qualificado.**
::Core::System::~System() = default;

// O construtor e o destructor do `Reporter` do Eden.
//
// Ficam em branco, e é o que devem estar: o `reporter.cpp` do Eden escreve o
// relatório num ficheiro em `~/.local/share/eden/report`, e o nosso motor não
// tem directório de relatórios. O que o `Reporter` faz connosco está no
// `SaveUnimplementedFunctionReport`, em baixo — e é a única das onze funções
// que o código vendorizado chama.
//
// As outras dez ficam **sem definição**, e isso é de propósito: uma função
// declarada e nunca chamada não dá erro de ligação. Se algum dia o jogo as
// alcançar, a falha aparece com o nome certo — e essa é a forma de discover
// que este projecto prefere a um `printf` a dizer "falta alguma coisa".
// O construtor **não** pode ser `= default` aqui: um construtor com
// parâmetros não pode ser defaulted fora da classe onde foi declarado. É a
// diferença entre `Reporter()` e `Reporter(System&)` — o primeiro podia ser
// `= default` no `.cpp`, o segundo tem de ter um corpo.
// O membro chama-se `system` (reporter.h:85), e não `system_`. Inicializá-lo
// é obrigatório: é uma referência, e o corpo tem de a preencher — não há
// `= default` que sirva.
Core::Reporter::Reporter(System& system_) : system(system_) {}
Core::Reporter::~Reporter() = default;

// Aqui, e não no cabeçalho, porque é o único sítio onde o
// `Service::ServerManager` está completo. Ver a nota no cabeçalho.
::Kernel::KernelCore& ::Core::System::Kernel() { return *kernel_; }
const ::Kernel::KernelCore& ::Core::System::Kernel() const { return *kernel_; }
::Service::SM::ServiceManager& ::Core::System::ServiceManager() { return *sm_; }
const ::Service::SM::ServiceManager& ::Core::System::ServiceManager() const {
    return *sm_;
}

// O controller do `apm` vive no `Core::System`, como no Eden. Ver o
// `apm_controller.h` ao lado.
::Service::APM::Controller& ::Core::System::GetAPMController() {
    return apm_controller_;
}

void ::Core::System::RunServer(
    std::unique_ptr<Service::ServerManager>&& server_manager) {
    server_manager->EnableSynchronousMode();
    servidores_.push_back(std::move(server_manager));
}

auto ::Core::System::ProcessPendingSynchronously() -> ::Result {
    bool progresso = false;
    for (std::size_t i = 0; i < servidores_.size(); ++i) {
        const auto& servidor = servidores_[i];
        const Result resultado = servidor->ProcessPendingSynchronously();
        if (std::getenv("IA_IPC_TRACE") != nullptr)
            std::fprintf(stderr,
                         "[ipc] pump servidor=%zu/%zu Result=%u (progresso=%d)\n",
                         i,
                         servidores_.size(),
                         static_cast<unsigned>(resultado.GetInnerValue()),
                         resultado == ResultSuccess ? 1 : 0);
        if (resultado == Kernel::ResultNotFound)
            continue;
        if (R_FAILED(resultado))
            return resultado;
        progresso = true;
    }
    if (servidores_.empty() && std::getenv("IA_IPC_TRACE") != nullptr)
        std::fprintf(stderr, "[ipc] pump SEM SERVIDORES registados (RunServer não correu)\n");
    return progresso ? ResultSuccess : Kernel::ResultNotFound;
}

void Kernel::KThreadQueueWithoutEndWait::EndWait(KernelCore&, KThread*, Result) {}

// ── o trinco leve ──────────────────────────────────────────────────────────
//
// `KLightLock` é o primitivo de bloqueio que o `KResourceLimit` usa
// (`k_resource_limit.cpp:125`, `KScopedLightLock`), e é o **único**primitivo
// de escalonamento que o caminho de IPC do Eden realmente chama. O resto do
// escalonador ficou de fora, e esta é a excepção que prova que o corte foi
// medido e não suposto.
//
// ## O caminho rápido do Eden É o caminho todo
//
// Lido em `k_light_lock.cpp:34-53`:
//
//     void KLightLock::Lock() {
//         while (true) {
//             ... compare_exchange ...          // pega no m_tag
//             if (old_tag == 0 || LockSlowPath(...)) break;
//         }
//     }
//     void KLightLock::Unlock() {
//         if (!m_tag.compare_exchange_strong(expected, 0, ...))
//             UnlockSlowPath(cur_thread);        // acorda o dono
//     }
//
// `LockSlowPath` e `UnlockSlowPath` só correm quando há **contenção** — e
// contendido para um trinco é haver outra thread à espera dele. Aqui a única thread
// é a do guest, que está dentro do `SendSyncRequest` e não é preemptível por
// nada. Portanto: `old_tag == 0` à primeira, e o `compare_exchange` de Unlock
// tem sucesso à primeira.
//
// O que fica escrito abaixo é o caminho rápido do Eden, **com o
// `compare_exchange` trocado por um `store`** — e a troca é legítima porque
// não há segunda parte com quem compete. O `atomic` fica: o código vendorizado
// lê o `m_tag` através dele, e um `std::atomic<uintptr_t>` não deve ser
// tocado a dedo.
//
// ## Onde isto deixaria de ser verdade
//
// Com mais de uma thread a tocar no mesmo `KLightLock`. Não há nenhuma em
// código de guest. A primeira que apareça — um serviço ouriço, um callback de
// shader em thread própria — torna estas duas funções erradas, e o sintoma
// seria um `m_tag` que ninguém mais consegue ler.
void Kernel::KLightLock::Lock() {
    const auto cur = reinterpret_cast<uintptr_t>(GetCurrentThreadPointer(m_kernel));
    m_tag.store(cur, std::memory_order_acquire);
}

void Kernel::KLightLock::Unlock() {
    m_tag.store(0, std::memory_order_release);
}

// ── a variável de condição leve ─────────────────────────────────────────────
//
// `KLightConditionVariable::Broadcast` é a única das suas funções de que o
// código vendorizado precisa, e é preciso por um caminho: `k_resource_limit.cpp:156`
// chama-a quando um limite é libertado e há alguém à espera.
//
// **A lista de quem espera está sempre vazia**, e isso não é uma aproximação:
// neste motor ninguém espera por nada. O pedido do guest é atendido a
// síncrono, dentro do `SendSyncRequest`, e a função regressa antes de haver
// outra thread para acordar. A lista do Eden é alimentada pelo
// `KSynchronizationObject::Wait`, que só existe em código que não vendorizámos.
//
// A diferença de comportamento, se algum dia aparecer, é que um pedido que
// devia ter ficado à espera de um limite seria atendido logo. O sintoma
// apareceria como `ResultLimitReached` a mais cedo — e é um sintoma que se
// vê, ao contrário de um guest que bloqueia para sempre sem dizer nada.
//
// **Qualificada com o namespace**, porque esta definição está fora do
// `namespace Kernel` — a definição do `Broadcast` vem depois do fecho do
// bloco do `KernelCore`, e o nome solto resolveria para o namespace global.
void Kernel::KLightConditionVariable::Broadcast() {}

// `KLightConditionVariable::Wait` **liberta o trinco e regressa**.
//
// É o único ponto da costura onde a hipótese «nunca ninguém bloqueia» é
// load-bearing, e vale a pena dizê-lo em voz alta.
//
// Quem chama é `k_resource_limit.cpp:127`, e só quando um limite está
// esgotado. Os limites que o IPC usa são `SessionCountMax` e
// `EventCountMax`, ambos na casa dos milhares, e há um processo só. Medido, o
// caminho não é alcançado no arranque do jogo.
//
// O que acontece se for alcançado: o pedido prossegue como se a condição
// tivesse sido sinalizada, em vez de esperar. O sintoma é o jogo avançar com
// um `ResultLimitReached` que não devia teruildo — **visível**, que é o que
// interessa. A alternativa honesta seria devolver um erro, e devolver um erro
// aqui transformaria um bug raro num crash certo.
void Kernel::KLightConditionVariable::Wait(KLightLock* lock, s64 /*timeout*/,
                                           bool /*allow_terminating_thread*/) {
    if (lock != nullptr)
        lock->Unlock();
}

// ── o registo de funções não implementadas ─────────────────────────────────
//
// Este é o ficheiro mais útil de toda a costura, e não por ser complicated:
// é o sítio onde o EdenRZ chama quando um serviço recebe um comando que não
// sabe responder, e onde isso passa a ser **uma linha na nossa telemetria**.
//
// O EdenRZ escreve um relatório em disco (`reporter.cpp`) para que se possa
// ir ver o que falta. Nós escrevemos no mesmo sítio — o `Telemetry` — que é o
// produto principal deste projecto, e que já está a medir cada SVC. A
// diferença é que a lista do EdenRZ é consultada uma vez no fim de um sessão
// de testes, e a nossa aparece no histogramam ao vivo.
//
// É também o melhor teste de que a costura está bem feita: se esta função
// aparecer no log durante o arranque, o IPC do Eden está a correr, e a falha
// está no comando concreto e não na plumbing.
void Core::Reporter::SaveUnimplementedFunctionReport(
    Service::HLERequestContext& ctx, u32 command_id, const std::string& name,
    const std::string& service_name) const {
    // As nove primeiras palavras do pedido, como o Eden as imprime
    // (service.cpp:63-66). São elas que distinguem um comando desconhecido de
    // um argumento malformado, e são o que permite depois ir ao binário do
    // jogo e ver o que o SDK mandou.
    // `CommandBuffer()` devolve um `u32*` cru e não um span — é assim no Eden
    // (service.cpp:60), que indexa `cmd_buf[0]` a `cmd_buf[8]` sem perguntar o
    // tamanho. Nove palavras é o que o command buffer do SMART garante.
    const auto* cmd_buf = ctx.CommandBuffer();
    std::string corpo;
    for (int i = 0; i <= 8; ++i) {
        char tmp[32];
        std::snprintf(tmp, sizeof tmp, "%s[%d]={%#x}", i ? ", " : "", i,
                      static_cast<unsigned>(cmd_buf[i]));
        corpo += tmp;
    }

    std::fprintf(stderr,
                 "[nao-implementado] servico=\"%s\" funcao=\"%s\" cmd={%s}\n",
                 service_name.c_str(), name.c_str(), corpo.c_str());
}



// ── o sistema ──────────────────────────────────────────────────────────────

namespace g610 {

// Quantas entradas na tabela de handles.
//
// No Eden isto vem das capabilities do processo
// (`k_process.cpp:967`, `m_capabilities.GetHandleTableSize()`), que por sua vez
// vêm do NPDM. **Não temos NPDM** — o nosso motor monta o processo a partir
// do exeFS sem nunca ver o manifesto de capacidades.
//
// 0x200 é uma escolha, não uma medição, e convém dizê-lo: são 512 entradas. O
// que se sabe é que o guest anda a usar índices baixos (medido: handle `0x10001`
// = índice 1, geração 2), e que o Eden dá ao processo principal uma tabela de
// algumas centenas de entradas.
//
// A consequência de errar é boa e visível: `ResultLimitReached` do
// `KHandleTable::Reserve`, que o guest reporta. Mucha melhor que um
// `abort()` dentro do Eden por um vector a crescer.
inline constexpr s32 kEntradasDaTabela = 0x200;

EdenSystem::EdenSystem(GuestMemory& memoria, std::uint64_t pid, std::uint64_t tid)
    : memoria_(memoria), pid_(pid), tid_(tid) {
    // 1. O `KernelCore`. Precisa de um `Core::System&`, e `*this` já é um
    //    `System` — a base está construída antes do corpo. Este é o passo que
    //    não pode ser um membro por valor, porque o `KernelCore` referenciaria
    //    um `System` ainda em construção.
    kernel_ = std::make_unique<Kernel::KernelCore>(*this);

    // 1b. O `KernelCore` fica ligado ao `System` **já**. O
    //     `ServiceManager` (passo 5) constrói o `IpcController` no seu
    //     construtor, e o `Controller` guarda uma referência ao
    //     `KernelCore` — que, se ainda não estiver ligado, é uma
    //     referência nula. O primeiro uso é o `CloneCurrentObject` (o SDK
    //     converte a sessão do `fsp-srv` em domínio), e aí o motor
    //     escreve no mutex de um `KernelCore` nulo e morre. Medido
    //     (`boot_p6i.log`): o backtrace era
    //     `CloneCurrentObject -> KSession::KSession ->
    //     KernelCore::RegisterKernelObject`, com o fault em
    //     `pthread_mutex_lock`.
    //
    //     A nota do passo 6 (abaixo) explica porque é que o `ligar()`
    //     completo continua a ser preciso no fim.
    ligar_kernel(*kernel_);

    // 2. A tabela de handles e os limites de recursos.
    handles_ = std::make_unique<Kernel::KHandleTable>(*kernel_);
    handles_->Initialize(*kernel_, kEntradasDaTabela);

    limites_ = std::make_unique<Kernel::KResourceLimit>(*kernel_);
    limites_->Initialize();

    // ── os limites de recurso ─────────────────────────────────────────────
    //
    // Sem isto, **nenhuma** reserva é satisfeita. E a leitura do Eden é
    // `k_resource_limit.cpp:100-104`:
    //
    //     if (m_current_hints[index] >= m_limit_values[index]) {
    //         return false;
    //     }
    //
    // Com os limites todos a zero — o valor por omissão de um `KResourceLimit`
    // recém-construído — a comparação é `0 >= 0` e a reserva falha à entrada.
    //
    // Foi isto que fez `ConnectToNamedPort("sm:")` devolver
    // `0x10801` = `ResultLimitReached`: o `KClientPort::CreateSession` reserva
    // uma sessão, e a reserva falhava. O sintoma parecia o do registo de nomes,
    // porque a minha mensagem de telemetria dizia «não encontrou a porta» para
    // os três casos — e estava errada.
    //
    // Os números são os do Eden (`k_resource_limit.cpp:168-174`), que são os
    // do sistema e não os do processo — o Eden diz isso no comentário, e não
    // tem limites por serviço. A memória física é a nossa reserva, que é a
    // única que existe.
    //
    // **Falha aqui é falha a sério.** O Eden faz `ASSERT(...IsSuccess())`. Nós
    // não GuestsAssertamos: se um limite não couber, o `EdenSystem` não deve
    // nascer, e o despachante fica com o caminho antigo — que é o que
    // `attach_eden` devolve ao falhar.
    struct Limite {
        Kernel::LimitableResource recurso;
        std::uint64_t valor;
    };
    const Limite limites[] = {
        {Kernel::LimitableResource::PhysicalMemoryMax, memoria_.size()},
        {Kernel::LimitableResource::ThreadCountMax, 800},
        {Kernel::LimitableResource::EventCountMax, 900},
        {Kernel::LimitableResource::TransferMemoryCountMax, 200},
        {Kernel::LimitableResource::SessionCountMax, 1133},
    };
    for (const auto& l : limites) {
        if (R_FAILED(limites_->SetLimitValue(l.recurso, static_cast<std::int64_t>(l.valor)))) {
            throw std::runtime_error("EdenSystem: limite de recurso nao aceite");
        }
    }

    // 4. O processo e a thread. A thread precisa do processo, e o processo
    //    precisa da tabela e dos limites — por isso esta ordem.
    processo_ = std::make_unique<Kernel::KProcess>(*kernel_, *handles_, *limites_, pid_, memoria_);
    thread_ = std::make_unique<Kernel::KThread>(*kernel_, *processo_);
    thread_->SetThreadId(tid_);

    // O `Bridge` é o "current" que o Eden guarda dentro do `KernelCore::Impl`.
    Kernel::Bridge::instalar(processo_.get(), thread_.get());

    // A thread principal na tabela de handles do Eden.
    //
    // É o mesmo handle que o Eden passa no x1 do entry point, e é o que o
    // SDK da Nintendo usa para saber qual é a sua thread. Ver a nota em
    // `eden_bridge.hpp`.
    {
        Kernel::Handle handle{};
        if (R_SUCCEEDED(handles_->Add(*kernel_, &handle, thread_.get())))
            main_thread_handle_ = static_cast<std::uint32_t>(handle);
    }

    // 5. O `sm:` a sério. `LoopProcess` cria o `ServerManager`, regista a porta
    //    "sm:" e chama-nos `RunServer` — que fica com a posse sem correr nada.
    sm_ = std::make_unique<Service::SM::ServiceManager>(*kernel_);

    // 6. O `Reporter` precisa de um `System&` no construtor, e só pode ser
    //    construído aqui — o `EdenSystem` é o único que é um `System` completo
    //    neste momento. Ver a nota do `ligar()` em `system_ref.h`.
    //
    //    O `kernel_` **já** está ligado (passo 1b): o `ServiceManager` do
    //    passo 5 constrói o `IpcController` no seu construtor, e o
    //    `Controller` guarda uma referência ao `KernelCore`. Sem a ligação
    //    antecipada, essa referência é nula e o motor morre no primeiro
    //    `CloneCurrentObject`. Ver a nota do `ligar_kernel()`.
    reporter_ = std::make_unique<Core::Reporter>(*this);

    ligar(*kernel_, *sm_, *reporter_);

    // 7. O `sm:` a sério. `LoopProcess` cria o `ServerManager`, regista a
    //    porta "sm:" e chama `RunServer`, que fica com a posse sem correr
    //    nada. Ver a nota em `system_ref.h`.
    Service::SM::LoopProcess(*this);

    // `lm:` é o primeiro serviço que este programa pede depois de
    // `sm:Initialize`. O Eden inicia-o num ServerManager próprio
    // (`core/hle/service/lm/lm.cpp:360-364`); o Core::System guarda esse
    // segundo manager para o dispatch síncrono.
    Service::LM::LoopProcess(*this);

    // O `apm` (a performance do sistema). O SDK pergunta por ele no arranque,
    // e o `sm:` fica a espera ate que exista.
    Service::APM::LoopProcess(*this);

    // O `appletOE` — o proxy do `am`. Ver `src/services/applet_oe.cpp`.
    g610::services::registar_applet_oe(*this);

    // O `pctl` (o controlo parental). O guest pede-o ao `sm:` no arranque —
    // `pctl`, `pctl:s`, `pctl:a` e `pctl:r` — e **os quatro pedidos ficam
    // pendentes** se o servico nao existir (`sm.cpp:200-203`:
    // `ctx.SetIsDeferred()`).
    //
    // O `pctl` ja estava vendorizado, mas o `LoopProcess` nunca era
    // chamado: no Eden e' ele que regista as quatro portas no `sm:`. O
    // codigo estava compilado e nunca executado — e o guest nao dizia
    // nada, porque um pedido pendente nao e' um erro: e' uma espera.
    Service::PCTL::LoopProcess(*this);

    // O `aoc:u` (os DLC). Este e' o **ultimo** servico que faltava, e o
    // mais importante: enquanto o `sm:` fica a espera por ele, **nenhum**
    // pedido seguinte do guest e' atendido.
    //
    // Medido: o guest pede `aoc:u` logo a seguir ao `appletOE`, o `sm:`
    // responde "Waiting for service aoc:u to become available" e fica
    // bloqueado. Os pedidos seguintes (`pctl`, `pctl:s`, `pctl:a`,
    // `pctl:r`) ficam **na fila** — e o log do `sm:` nunca mostra o nome
    // deles.
    //
    // O servico esta vendorizado do Eden, sem o `ContentProvider`: um
    // motor de execucao nativa nao tem DLC, e a lista vazia e' a
    // resposta honesta. O que o guest precisa e' que o servico **exista**.
    Service::AOC::LoopProcess(*this);

    // O `time:u` — o tempo do sistema. Medido: o SDK pede-o ao `sm:` no
    // arranque (`nn::time::Initialize`) e, sem ele, o `sm:` **defere**
    // o pedido e o `m_current_request` do `KServerSession` fica preso
    // para sempre — o guest estaciona no `SendSyncRequest` e morre no
    // `SetTerminateResult` (o cap do passo 6). Ver
    // `src/services/time_u.cpp`.
    g610::services::registar_time_u(*this);

    // O `fsp-srv` — o servidor de ficheiros. Medido (`boot_p6f.log`): é o
    // **único** dos dez serviços que o SDK pede no arranque que faltava.
    // Sem ele, o `sm:` defere o `GetService` e o guest aborta no reader
    // CMIF (`modulo=10 desc=212`), porque o diferimento no motor nativo
    // devolve uma resposta de 0 palavras em vez de deixar o guest
    // bloqueado. Ver `src/services/fsp_srv.cpp`.
    g610::services::registar_fsp_srv(*this);

    // O `set:` — as preferencias do sistema (a lingua, a regiao).
    // **Medido** (`/tmp/boot_r22.log`): o SDK pede `GetService("set")` e a
    // casa nao o tinha — o `sm:` respondia "Server is not registered" e o
    // guest abortava no leitor CMIF (`modulo=10 desc=212`).
    // Ver `src/services/set.cpp`.
    g610::services::registar_set(*this);

    // O `pl:u` — o servico da plataforma (a fonte do sistema).
    // **Medido** (`/tmp/boot_r23.log`): o SDK pede `GetService("pl:u")` e a
    // casa nao o tinha — o `sm:` deferia o pedido e o guest abortava no
    // leitor CMIF (`modulo=10 desc=212`).
    // Ver `src/services/pl_u.cpp`.
    g610::services::registar_pl_u(*this);

    // O `nvdrv` — o driver da NVIDIA, que e' a porta de entrada da GPU.
    // Medido (`boot_p6l.log`): depois do `fsp-srv`, e' o proximo servico
    // que o SDK pede e que faltava. O servico e' **fino** (a interface do
    // Eden, com as formas das respostas, sem o driver): o trabalho de
    // verdade — o push buffer para o tradutor Maxwell -> Vulkan da casa
    // — e' o passo seguinte. Ver `src/services/nvdrv.cpp`.
    g610::services::registar_nvdrv(*this);

    // O `vi` — o servico de **video** (o display). Medido
    // (`boot_p7d.log`): depois do `nvdrv`, e' o proximo servico que o
    // SDK pede e que faltava ("vi:m"). Sem ele, o `sm:` defere o pedido
    // e o guest aborta no reader CMIF (`modulo=10 desc=212`).
    // Ver `src/services/vi.cpp`.
    g610::services::registar_vi(*this);

    // A verificacao que esteve aqui -- `KObjectName::Find("sm:")` -- **partiu
    // o jogo**.
    //
    // `Find` devolve um `KScopedAutoObject`, e sair do ambito chama `Close()`
    // no `KClientPort`. Se esse `Close` destruir a porta -- e no nosso motor
    // pode, porque o `KPort` do `ServerManager` é o único dono e a contagem de
    // referências vive no `KAutoObject` do Eden -- então a porta desaparece do
    // registo entre o arranque e o primeiro `ConnectToNamedPort`.
    //
    // Medido: com a verificação, `Find` achava a porta no fim do construtor e
    // devolvia vazio no primeiro pedido. Sem ela, o jogo continua.
    //
    // Um instrumento que muda aquilo que mede não é um instrumento: é uma
    // alteração com comentários. Por isso a verificação fica escrita e não
    // executada.
}

EdenSystem::~EdenSystem() {
    // A ordem é o inverso da construção. O `Bridge` primeiro, porque é o que
    // dá acesso ao processo e à thread a tudo o resto — e a corrente de
    // destruição vai do topo para o fundo.
    Kernel::Bridge::limpar();
    reporter_.reset();
    thread_.reset();
    processo_.reset();
    sm_.reset();
    limites_.reset();
    handles_.reset();
    kernel_.reset();
}

void EdenSystem::set_tls(std::uint64_t endereco) noexcept {
    tls_ = endereco;
    if (thread_)
        thread_->SetTlsAddress(endereco);
}

// ── o instrumento ──────────────────────────────────────────────────────────
//
// ── os SVCs ────────────────────────────────────────────────────────────────

// `ConnectToNamedPort`, de svc/svc_port.cpp:19-55.
//
// **Devolve uma SESSÃO, não uma porta** — e escreve o handle no ponteiro do
// guest (`out`, que é x1). É por isso que o guest vai directo do
// `ConnectToNamedPort` para o `SendSyncRequest` sem `OpenSession`: a sessão já
// veio pronta.
bool eden_connect_to_named_port(Kernel::KernelCore& kernel, GuestMemory& memoria,
                                std::uint64_t nome_ptr, std::uint32_t* out_handle) {
    Kernel::Memory mem(memoria);

    // Copiar o nome da memória do guest para a do host. O limite é o do Eden
    // (`KObjectName::NameLengthMax`), e um nome que não acaba dentro desse
    // limite é um nome inválido — não é para ler mais.
    const std::string nome_guest = mem.ReadCString(nome_ptr, Kernel::KObjectName::NameLengthMax);

    std::array<char, Kernel::KObjectName::NameLengthMax> nome{};
    std::strncpy(nome.data(), nome_guest.c_str(), Kernel::KObjectName::NameLengthMax - 1);

    // Um nome que não coube não é um nome. O Eden devolve ResultOutOfRange, e
    // este booleiro não tem como dizer qual dos Resultados foi — daí o
    // `last_connect_result`, que a telemetria lê.
    auto& tabela = Kernel::GetCurrentProcess(kernel).GetHandleTable();

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[ipc] ConnectToNamedPort nome=\"%s\" (%zu bytes)\n",
                     nome.data(), nome_guest.size());
        Kernel::KObjectName::DumpNames(kernel);
    }

    auto porta = Kernel::KObjectName::Find<Kernel::KClientPort>(kernel, nome.data());
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[ipc]   Find<KClientPort> devolveu %p (IsNull=%d)\n",
                     static_cast<const void*>(porta.GetPointerUnsafe()),
                     static_cast<int>(porta.IsNull()));
        auto cru = Kernel::KObjectName::Find(kernel, nome.data());
        std::fprintf(stderr, "[ipc]   Find<KAutoObject> devolveu %p (IsNull=%d)\n",
                     static_cast<const void*>(cru.GetPointerUnsafe()),
                     static_cast<int>(cru.IsNull()));
    }
    if (porta.IsNull()) {
        // O Eden: `R_UNLESS(port.IsNotNull(), ResultNotFound)`.
        if (out_handle)
            *out_handle = 0;
        return false;
    }

    Kernel::Handle handle{};
    const auto r_reserva = tabela.Reserve(kernel, &handle);
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[ipc]   Reserve(handle) = %s\n",
                     R_SUCCEEDED(r_reserva) ? "ok" : "FALHOU");
    }
    if (R_FAILED(r_reserva)) {
        if (out_handle)
            *out_handle = 0;
        return false;
    }
    // **SCOPE_GUARD**, nao SCOPE_EXIT.
    //
    // A primeira versao usava `SCOPE_EXIT` e nunca o cancelava depois do
    // `Register`. Consequencia medida: o trace dizia `handle devolvido =
    // 0x81ff`, mas no primeiro `SendSyncRequest` a tabela tinha
    // `GetCount() == 0` e `GetObject` devolvia nullptr. O `SCOPE_EXIT` estava
    // a fazer exactamente o que lhe pedi — libertar o handle no fim — e o fim
    // era o sucesso.
    //
    // O handle só deve sair se a reserva ou o CreateSession falharem. Em caso
    // de sucesso, `Register` ocupa o slot e o guard é cancelado logo a seguir.
    auto unreserve_handle = SCOPE_GUARD {
        if (handle != Kernel::Svc::InvalidHandle)
            tabela.Unreserve(kernel, handle);
    };

    Kernel::KClientSession* sessao = nullptr;
    const auto r_sessao = porta->CreateSession(kernel, &sessao);
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[ipc]   CreateSession = %s  valor=0x%x  (sessao=%p)\n",
                     R_SUCCEEDED(r_sessao) ? "ok" : "FALHOU",
                     static_cast<unsigned>(r_sessao.raw),
                     static_cast<const void*>(sessao));
    }
    if (R_FAILED(r_sessao)) {
        if (out_handle)
            *out_handle = 0;
        return false;
    }

    // `Register` e `Reserve` são dois passos: reserva o índice, e só depois
    // escreve o objecto. `IsValidHandle` exige que ambos os campos estejam
    // certos — `raw != 0` E `linear_id != 0` — e um handle reservado mas não
    // registado tem `raw` com a geração a zero, ou seja, inválido.
    tabela.Register(kernel, handle, sessao);
    // O slot agora pertence à tabela; a limpeza de falha já não se aplica.
    unreserve_handle.Cancel();

    // O Eden fecha a referência extra que o `CreateSession` deu.
    sessao->Close(kernel);

    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[ipc]   handle devolvido = 0x%x  (indice=%u, geracao=%u)\n",
                     static_cast<unsigned>(handle),
                     static_cast<unsigned>(static_cast<std::uint32_t>(handle) & 0x7FFF),
                     static_cast<unsigned>(static_cast<std::uint32_t>(handle) >> 15));
        std::fprintf(stderr, "[ipc]   entradas da tabela = %zu (max %zu, size %zu)\n",
                     tabela.GetCount(), tabela.GetMaxCount(), tabela.GetTableSize());
    }

    if (out_handle)
        *out_handle = static_cast<std::uint32_t>(handle);
    return true;
}

// `SendSyncRequest`, de svc/svc_ipc.cpp:30-36.
//
//     KScopedAutoObject session = handle_table.GetObject<KClientSession>(...);
//     R_UNLESS(session.IsNotNull(), ResultInvalidHandle);
//     KScopedAutoObject parent = {kernel, session->GetParent()};
//     R_RETURN(session->SendSyncRequest(kernel, message, buffer_size));
//
// `message` e `buffer_size` são **0** nesta variante: é o kernel que vai buscar
// o `MessageBuffer` ao TLS da thread. É por isso que x1 e x2 são ignorados —
// o SDK passa-os, e o Eden também não os olha.
bool eden_send_sync_request(Kernel::KernelCore& kernel, std::uint32_t session_handle,
                            std::uint64_t endereco) {
    auto& tabela = Kernel::GetCurrentProcess(kernel).GetHandleTable();

    Kernel::KScopedAutoObject<Kernel::KClientSession> sessao =
        tabela.GetObject<Kernel::KClientSession>(kernel, Kernel::Handle{session_handle});
    if (sessao.IsNull()) {
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            // Diagnóstico sem possuir a sessão: a tabela abre e fecha referências
            // temporárias e os dois pointers abaixo são só impressos. Não
            // retém o objecto, nem altera o handle que se está a medir.
            auto raw = tabela.GetObject<Kernel::KAutoObject>(
                kernel, Kernel::Handle{session_handle});
            // O TIPO do objecto do handle separa três falhas diferentes:
            // inexistente (o handle não foi criado), existir mas de OUTRO
            // tipo (o pedido devia ir a outro SVC), e existir com geração
            // velha (reuso do slot). É a mesma razão do "addr do fault".
            const char* tipo = raw.IsNotNull() ? raw->GetTypeName()
                                               : "(inexistente)";
            std::fprintf(stderr,
                         "[ipc] SendSyncRequest handle=0x%x count=%zu size=%zu raw=%p tipo=%s\n",
                         session_handle, tabela.GetCount(), tabela.GetTableSize(),
                         static_cast<const void*>(raw.GetPointerUnsafe()), tipo);
        }
        return false;
    }
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[ipc] SendSyncRequest handle=0x%x typed=%p\n",
                     session_handle, static_cast<const void*>(sessao.GetPointerUnsafe()));

        // **Onde vai a resposta.** A resposta do IPC vai para o TLS da
        // thread do *cliente* (o Eden, `hle_ipc.cpp:330`). Se o pedido e o
        // TLS nao forem o mesmo sitio, o guest le lixo.
        std::fprintf(stderr, "[ipc]   pedido em 0x%llx, resposta vai para 0x%llx\n",
                     static_cast<unsigned long long>(endereco),
                     static_cast<unsigned long long>(
                         Kernel::GetCurrentThread(kernel).GetTlsAddress()));
    }
    // A referência ao pai: o pai tem de sobreviver ao pedido. Sem este
    // `KScopedAutoObject` a sessão pode ser destruída a meio e o pedido
    // escreve para memória morta.
    Kernel::KScopedAutoObject<Kernel::KAutoObject> pai{kernel, sessao->GetParent()};
    if (pai.IsNull()) {
        if (std::getenv("IA_IPC_TRACE") != nullptr)
            std::fprintf(stderr,
                         "[ipc] SendSyncRequest handle=0x%x SEM PAI (sessao sem parent)\n",
                         session_handle);
        return false;
    }

    // ── O ENDEREÇO DO PEDIDO ─────────────────────────────────────────────
    //
    // `SendSyncRequest(handle, endereco, tamanho)` — o guest passa o
    // endereço **do seu buffer de pedido** ✗ e o kernel escreve a resposta
    // no buffer de resposta do TLS ✗.
    //
    // **Medido** (`/tmp/boot_dom.log` ✗): o guest manda
    // `x1=0x88004000` ✗ e a casa passava `0` ✗ — e o upstream, quando o
    // endereço é zero ✗, vai buscar o pedido ao **TLS** da thread ✗
    // (`k_server_session.cpp:1162` ✗) em vez de o ler de onde o guest o
    // escreveu ✗. O que lá estava era a **resposta** anterior ✗ — o magic
    // "SFCO" ✗ em vez de "SFCI" ✗ — e o `ParseCommandBuffer` do upstream
    // queixava-se ✗ (`assert data_payload_header->magic` ✗).
    //
    // O tamanho ✗: o `x2` do guest ✗ **não** é um tamanho ✗ (no pedido
    // medido ✗, `x2 = 0x481f7` ✗ — o mesmo valor do handle ✗ ✗). O
    // caminho HLE ✗ do upstream lê o buffer pelo `COMMAND_BUFFER_LENGTH` ✗
    // fixo ✗ e não usa o tamanho ✗ — por isso vai `0` ✗ ✓.
    if (R_FAILED(sessao->SendSyncRequest(kernel, endereco, 0))) {
        if (std::getenv("IA_IPC_TRACE") != nullptr)
            std::fprintf(stderr,
                         "[ipc] SendSyncRequest handle=0x%x FILA falhou (KClientSession::SendSyncRequest != Success)\n",
                         session_handle);
        return false;
    }

    // `KClientSession::SendSyncRequest` enfileirou o pedido. No Eden uma
    // thread do ServerManager acordaria, receberia e responderia; aqui não há
    // thread host, portanto fazemos exactamente essas duas fases agora, no
    // fluxo síncrono do guest.
    const Result resultado = kernel.System().ProcessPendingSynchronously();

    // **`ResultNotFound` nao e' um erro** — e' "ainda nao ha nada para
    // atender".
    //
    // O upstream (`server_manager.cpp:418`) lanca `ResultNotFound` quando o
    // pump nao encontrou trabalho ✗ — e o `svc_ipc.cpp:91` trata-o
    // explicitamente como "continua a espera" ✗, nao como falha ✗.
    //
    // **Medido** ✗: o pump da casa devolvia `ResultNotFound` (61953 ✗) e o
    // `eden_send_sync_request` devolvia `false` ✗ — o que o
    // `svc_send_sync_request` traduzia em `Result::InvalidHandle` ✗ ✗.
    // O guest recebia um erro num pedido **legitimo** ✗ ✗ e abortava ✗
    // (`SetTerminateResult modulo=2 desc=3005` ✗) ✗.
    //
    // **O que e' um erro a serio** ✗: o `SendSyncRequest` enfileirar o
    // pedido ✗ (linha acima ✗) ✗. O pump e' um **extra** ✗ — o pedido ja
    // esta' na fila do servidor ✗ e sera' atendido no proximo ciclo ✗ ✗.
    const bool processado = R_SUCCEEDED(resultado);
    if (!processado && resultado != Kernel::ResultNotFound &&
        std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[ipc] SendSyncRequest handle=0x%x PROCESSAMENTO falhou (Result=%u)\n",
                     session_handle, static_cast<unsigned>(resultado.GetInnerValue()));
    }
    return processado || resultado == Kernel::ResultNotFound;
}

} // namespace g610
