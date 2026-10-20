// Substitui `core/hle/kernel/k_synchronization_object.cpp` do Eden.
//
// Ver `vendor/eden/core/hle/kernel/k_process_bridge.h` para a ponte.
//
// ## A medida que decidiu o conteúdo
//
// Antes de escrever uma linha, tirei o `.cpp` do Eden da compilação e li o que
// o linker pedia (`nm` sobre `libia_core.a`, depois do build):
//
//     U Kernel::KSynchronizationObject::KSynchronizationObject(KernelCore&)
//     U Kernel::KSynchronizationObject::~KSynchronizationObject()
//     U Kernel::KSynchronizationObject::Finalize(KernelCore&)
//     U Kernel::KSynchronizationObject::NotifyAvailable(KernelCore&, Result)
//     typeinfo for Kernel::KSynchronizationObject
//
// **Quatro funções. E `Wait` não está na lista.** Nenhum dos 22 ficheiros
// vendorizados que compilam chama `KSynchronizationObject::Wait`.
//
// Isto muda o que este ficheiro é. Não é «o `Wait` do Eden com as esperas
// removidas» — é um ficheiro de 180 linhas em que 100 são a implementação de
// `Wait`, e que ninguém chama. Escrever as quatro funções que são chamadas é
// o ficheiro inteiro.
//
// ## O que ficou de fora, e porquê
//
// `KSynchronizationObject::Wait` dorme a thread. Usa
// `KScopedSchedulerLockAndSleep`, que é o escalonador, e põe a thread numa
// fila à espera de alguém a sinalizar. **Não existe nada disto num motor de
// execução nativa**: o guest corre numa thread, de forma síncrona, dentro de um
// `SendSyncRequest`, e regressa antes de haver outra thread para acordar.
//
// Deixá-la declarada e sem definição é o estado correcto, e é verificável: se
// algum dia alguém a chamar, a falha aparece na ligação com o nome certo. Um
// `Wait` nosso que devolvesse «a condição foi sinalizada» sem que ninguém
// tivesse sinalizado nada seria pior — seria uma espera que passa a dizer que
// passou.
//
// ## As quatro, uma a uma
//
// O construtor e o destructor são os do Eden, palavra por palavra
// (`k_synchronization_object.cpp:149,151`): `KAutoObjectWithList{kernel}`, e
// nada mais.
//
// `Finalize` chama `OnFinalizeSynchronizationObject()` e `KAutoObject::Finalize`
// — também igual ao Eden. É uma cascata de finalização, e é código que
// depende do estado do objecto, não de quem espera por ele.
//
// `NotifyAvailable` percorre `m_thread_list_head` a notificar quem espera. A
// lista está **sempre vazia** — e isso não é uma aproximação: é preenchida por
// `Wait`, que ninguém chama. A função é a do Eden com o `for` a não iterar
// nenhuma vez, porque não há nós.
//
// `GetWaitingThreadsForDebugging` devolve uma lista vazia, pela mesma razão.
// Não a pediu o linker, mas é a função que um diagnóstico chamaria, e um
// diagnóstico que devolve threads que não existem é pior do que um que devolve
// nenhuma.
#include <vector>

#include "vendor/eden/core/hle/kernel/k_synchronization_object.h"
// `KThread` tem de estar COMPLETO para `NotifyAvailable`: a lista de nós é
// `ThreadListNode`, que é um `KThread*`, e desreferenciá-lo precisa da classe.
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"
// Os Results do kernel: `ResultInvalidHandle` e `ResultTimedOut`. O
// `k_synchronization_object.h` do Eden inclui o `result.h` — que tem os
// Results do **servico** — mas nao o `svc_results.h`, que e' onde o kernel
// os tem.
//
// **A diferenca apareceu ao separar a historia.** Com o
// `k_synchronization_object.cpp` listado no build, o `svc_results.h`
// entrava por outro caminho e o ficheiro compilava. So, o include
// falta.
#include "vendor/eden/core/hle/kernel/svc_results.h"

namespace Kernel {

// Idêntico ao Eden (`k_synchronization_object.cpp:149,151`). A herança vem de
// `KAutoObjectWithList`, que dá a lista de objectos com dono — que o
// `KernelCore::ObjectListContainer` da ponte usa no `Destroy`.
KSynchronizationObject::KSynchronizationObject(KernelCore& kernel)
    : KAutoObjectWithList{kernel} {}

KSynchronizationObject::~KSynchronizationObject() = default;

void KSynchronizationObject::Finalize(KernelCore& kernel) {
    // Igual ao Eden (`k_synchronization_object.cpp:71-74`).
    this->OnFinalizeSynchronizationObject();
    KAutoObject::Finalize(kernel);
}

// Igual ao Eden (`k_synchronization_object.cpp:153-165`), com o `for` a não
// iterar: `m_thread_list_head` é preenchido em `Wait`, e `Wait` não é chamado.
//
// **Não é um atalho.** Se esta função passar a iterar, é porque alguém
// implementou a espera — e nesse momento as duas coisas precisam de ser
// revistas juntas. A lista e o `Wait` são a mesma feature.
void KSynchronizationObject::NotifyAvailable(KernelCore& kernel, Result result) {
    for (auto* cur_node = m_thread_list_head; cur_node != nullptr; cur_node = cur_node->next) {
        cur_node->thread->NotifyAvailable(kernel, this, result);
    }
}

std::vector<KThread*> KSynchronizationObject::GetWaitingThreadsForDebugging(KernelCore&) const {
    // Lista vazia, e a lista de nós é sempre vazia. Ver a nota de cima.
    std::vector<KThread*> threads;
    for (auto* cur_node = m_thread_list_head; cur_node != nullptr; cur_node = cur_node->next) {
        threads.emplace_back(cur_node->thread);
    }
    return threads;
}

// ── `Wait` ─────────────────────────────────────────────────────────────────
//
// **Uma medição minha caducou.** Quando escrevi a nota do topo deste ficheiro,
// li a lista de símbolos indefinidos e concluí que `Wait` não era chamado por
// nada. Era verdade *naquele momento*, com os 22 ficheiros que existiam.
//
// Depois entrei `multi_wait.cpp`, que é o `MultiWait` do Eden — e o
// `MultiWait::WaitAny` chama precisamente `KSynchronizationObject::Wait`. O
// símbolo passou a ser pedido.
//
// A lição não é sobre o `Wait`: é que **uma medição vale para a árvore de
// ficheiros que se mediu, e não para a seguinte.** Quando se acrescenta
// código, as medições que ficaram obsoletas e nenhuma das ferramentas
// avisa. Por isso que este ficheiro reimplemente `Wait` em vez de o deixar
// por definir: fica correcta com o que existe hoje, e a próxima medição
// confirma.
//
// ## O que `Wait` faz aqui
//
// Duas fases, e as duas são o que o Eden faz quando não há contenção:
//
// 1. Percorre os objectos e devolve o **primeiro que já esteja sinalizado**,
//    com o índice e `ResultSuccess`. É o caminho rápido do Eden
//    (`k_synchronization_object.cpp:76-148`), e é o que acontece quando o
//    pedido foi atendido antes de a thread chegar a esperar.
//
// 2. Se nenhum estiver sinalizado, **não há para quem esperar** e devolve-se
//    `ResultTimedOut`.
//
// O passo 2 é a resposta honesta, e não uma preguiça: a thread do guest nunca
// está à espera de nada, porque um pedido de serviço é atendido dentro do
// `SendSyncRequest` e a thread não pára. Devolver `ResultSuccess` aqui seria
// dizer «alguém acordou esta thread» sem ninguém ter acordado ninguém — e o
// chamador, o `MultiWait::WaitAny`, trataria isso como um evento.
//
// `timeout` é ignorado. O motor não tem relógio de espera para advancing, e um
// timeout que não passa nunca seria uma espera eterna sob outra forma.
Result KSynchronizationObject::Wait(KernelCore& kernel, s32* out_index,
                                    KSynchronizationObject** objects, const s32 num_objects,
                                    s64 /*timeout*/) {
    if (objects == nullptr || num_objects <= 0) {
        return ResultInvalidHandle;
    }

    // A fase 1: o Eden também varre a lista antes de dormir
    // (`k_synchronization_object.cpp:80-90`), e é o que evita dormir quando o
    // evento já aconteceu.
    for (s32 i = 0; i < num_objects; ++i) {
        KSynchronizationObject* obj = objects[i];
        if (obj == nullptr)
            continue;
        if (obj->IsSignaled(kernel)) {
            if (out_index != nullptr)
                *out_index = i;
            return ResultSuccess;
        }
    }

    // A fase 2. Ver a nota acima.
    return ResultTimedOut;
}

} // namespace Kernel