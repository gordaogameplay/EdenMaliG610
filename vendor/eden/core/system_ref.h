// Substitui `core/core.h` nos ficheiros que só precisam de chegar ao
// KernelCore. Ver vendor/eden/README.md.
//
// `Core::System` é a raiz do Eden e arrasta o VFS, o NCA e o resto do
// emulador. A camada de IPC não usa nada disso — usa três métodos, e é isso
// que fica declarado aqui.
//
// ## O que a medição mudou
//
// A primeira versão desta classe declarava `Kernel()`, `ServiceManager()` e
// `RunServer()` como **virtuais puros**. Compilava. E na ligação dava:
//
//     undefined reference to `Core::System::Kernel()'
//     undefined reference to `Core::System::ServiceManager()'
//
// A causa está em `core.h` do Eden, e é uma linha:
//
//     [[nodiscard]] Kernel::KernelCore& Kernel();          // core.h:300
//     [[nodiscard]] Service::SM::ServiceManager& ServiceManager();  // core.h:331
//
// **Não são virtuais.** São métodos normais, e por isso têm de existir como
// símbolos. Ao declará-los virtuais puros, criámos uma classe que o Eden não
// é: os nomes ficam certainemente errados, o compilador não reclama, e a
// falha só aparece na ligação, no fim, com um nome de função que parece
// certo.
//
// ## O ciclo, resolvido pela mesma via
//
// `KernelCore` recebe `Core::System&` no construtor, e o `System` é quem
// guarda o `KernelCore`. Com métodos **não virtuais** não há problema nenhum:
// o `System` guarda um ponteiro, e o `EdenSystem` chama `ligar()` depois de o
// `KernelCore` existir. É mais simples do que a estratégia do `unique_ptr` que
// a versão virtual obrigava.
//
// Este ficheiro mostra, em seis linhas de interface, a dimensão do problema:
// **duas linhas separam a camada de sistema do emulador**. Não é o
// `Core::System` que é grande — é tudo o que ele arrasta atrás de si.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

// `u8`, `u32` e `u64` vem de ca. Nao sao redefinidos aqui: o Eden ja os
// declara, e um `typedef` repetido com o mesmo tipo e' legal mas atrapalha a
// ler a partir da segunda ocorrencia em diante.
#include "vendor/eden/common/common_types.h"
#include "vendor/eden/core/reporter.h"  // `Core::Reporter`, o do Eden

namespace Kernel {
class KernelCore;
} // namespace Kernel

namespace Service {
class ServerManager;
namespace SM {
class ServiceManager;
} // namespace SM
} // namespace Service

// O `Reporter` é o do Eden (`core/reporter.h`, 87 linhas), usado tal e qual.
//
// A primeira versão declarava aqui um `Core::Reporter` nosso, com a mesma
// assinatura. Compilava até `service.cpp` incluir o `reporter.h` do Eden — e
// ai dava "redefinition of class Core::Reporter".
//
// Duas classes com o mesmo nome no mesmo namespace não se fundem: declarar
// a segunda é um erro, não uma especialização. E a lição vale para
// todo o resto desta costura: **quando o Eden já tem a classe, usa-se a do
// Eden.** Só se escreve uma nossa quando o que falta é o *comportamento*,
// não o *tipo* — que é o caso do `KernelCore` e do `KProcess`, e não é o
// caso do `Reporter`.
namespace Core {

class System {
public:
    System() = default;

    // Declarado aqui, definido em `src/eden_bridge.cpp`.
    //
    // Os membros `servidores_` são `unique_ptr<Service::ServerManager>` e o tipo
    // só está declarado à frente — que é o que permite a este cabeçalho não
    // puxar `server_manager.h`. O preço é que o destructor implícito gerado
    // aqui precisa de um `sizeof` de um tipo incompleto.
    //
    // Declarar o destructor e movê-lo para um `.cpp` é o mesmo movimento que
    // fez falta no `SvcDispatcher`: o `sizeof` passa a acontecer num sítio
    // onde o tipo está completo, e não em todos os que incluem este ficheiro.
    virtual ~System();

    // Cópia e movimento: apagados, e por três razões ao mesmo tempo.
    //
    // Um `System` copiado teria duas possessões do mesmo `ServerManager` — e
    // uma delas a apagar um `KPort` que a outra ainda usa.
    //
    // Um `System` movido teria o mesmo problema meio resolvido: os dois
    // ponteiros (`kernel_`, `sm_`) são crus e não se movem sozinhos.
    //
    // E, na prática, nenhum dos dois é preciso: o `System` vive dentro de um
    // `unique_ptr` e nunca sai de lá. Apagar é escrever no papel o que a
    // estrutura já é.
    //
    // **O movimento tem de ser apagado à mão.** Apagar a cópia não chega: o
    // construtor de movimento continua a ser gerado, e é a geração dele que
    // exige o `sizeof` do `Service::ServerManager` — o mesmo erro do
    // destructor, por outra rota.
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    System(System&&) = delete;
    System& operator=(System&&) = delete;

    // ── os três métodos que a camada de IPC pede ──────────────────────────
    //
    //   Kernel()          -- o `HLERequestContext` e o `KResourceLimit` sobem
    //                         daqui, e com eles quase todo o IPC
    //   ServiceManager()  -- sm.cpp:309 e sm.cpp:316; o registo de serviços,
    //                         de onde saem as portas nomeadas
    //   RunServer()       -- sm.cpp:321; entrega a posse do ServerManager
    //
    // Três de dezanove. Os outros dezasseis são memória, NCA, VFS, áudio,
    // vídeo, e as coisas que emitem SVCs — que é o emulador.

    // **Não virtual**, como no Eden (core.h:300). Ver a nota em cima: ser
    // virtual aqui muda o mangling e produz um símbolo que não existe.
    //
    // **Definidos no `.cpp`, e nao aqui.** Define-los dentro da classe faz com
    // que sejam `inline`, e uma funcao `inline` so e' emitida no binario se o
    // compilador a precisar. Com `-O2` ele inlineia e nao emite — e entao
    // outro ficheiro que a use sem a poder inlinear fica a procura de um
    // simbolo que ninguem escreveu:
    //
    //     undefined reference to `Core::System::Kernel()'
    //
    // Uma funcao de classe definida no cabecalho e' `inline` **por defeito**.
    // Passar a escreve-la fora da classe, sem a palavra `inline`, e o que
    // resolve — e e' a razao de o Eden poder ter estes metodos como metodos
    // normais: esta classe nao e' a dele.
    Kernel::KernelCore& Kernel();
    const Kernel::KernelCore& Kernel() const;
    Service::SM::ServiceManager& ServiceManager();
    const Service::SM::ServiceManager& ServiceManager() const;

    //
    // **Declarado aqui, definido em `src/eden_bridge.cpp`.** A primeira versão
    // tinha o corpo aqui e não compilava em três ficheiros.
    //
    // O corpo guarda `server_manager` em `servidores_`, e essa operação
    // chama `default_delete<ServerManager>::operator()`, que faz
    // `sizeof(ServerManager)`. **O corpo de uma função inline é verificado
    // mesmo que ninguém a chame** — a verificação não espera por um uso. E
    // aqui o tipo está só declarado à frente, precisamente para este
    // cabeçalho não puxar `server_manager.h`.
    //
    // É a mesma coisa que o destructor, a cópia e o movimento, e pela mesma
    // razão. A regra que sai daqui, e que vale para a ponte toda:
    //
    //     **um `unique_ptr` de tipo incompleto obriga a que tudo o que o
    //     toca seja declarado, não definido.**
    void RunServer(std::unique_ptr<Service::ServerManager>&& server_manager);

    // Consome o evento de porto/sessão pendente sem bloquear numa thread
    // host. Definido em `src/eden_bridge.cpp`, onde ServerManager está
    // completo.
    Result ProcessPendingSynchronously();

    // ── os dois que o `service.cpp` usa no caminho do "não implementado" ──

    // `[[nodiscard]] bool IsPoweredOn() const` no Eden (core.h:219). Usado em
    // `service.cpp:135` para não escrever a resposta se a emulação estiver a
    // fechar.
    //
    // Aqui a resposta é sempre `true`, e não por preguiça: **esta máquina está
    // ligada**. O guest está a executar código ARM, o `SendSyncRequest` está a
    // ser atendido por esta chamada, e não existe caminho em que a emulação
    // esteja a desligar — o processo acaba quando o guest chama
    // `ExitProcess`, e nesse momento já não há resposta para escrever.
    bool IsPoweredOn() const { return true; }

    // O registo de funções não implementadas (`core/reporter.h:46`).
    //
    // No Eden isto escreve um relatório em disco. Aqui escreve na **nossa
    // telemetria**, que é a mesma coisa com um sítio melhor: o produto
    // principal deste projecto é saber o que o jogo pede e o que ainda não
    // está feito.
    //
    //
    // O `Reporter` é declarado FORA desta classe, no namespace `Core`, porque
    // aninhado teria de ser implementado dentro de `eden_bridge.cpp` com o
    // nome `System::Reporter` — e um nome de classe que só existe dentro de
    // outra é uma coisa que se cobra mais tarde.
    //
    // **Só devolve: não cria.** A primeira versão criava preguiçosamente com
    // `make_unique<Reporter>()` — e o construtor do `Reporter` do Eden
    // (`reporter.h:31`) pede um `System&`. Um `System` a construir-se a si
    // próprio não é uma coisa que se possa escrever.
    //
    // A solução é a mesma do `KernelCore` e do `ServiceManager`: quem tem as
    // peças é o `EdenSystem`, e entrega-as ao `ligar()` no fim do construtor.
    Reporter& GetReporter() { return *reporter_; }

    // Liga as duas pontas depois de ambas existirem.
    //
    // Separado do construtor porque não pode estar nele: o `KernelCore`
    // precisa de um `Core::System&`, e portanto o `System` tem de existir
    // antes de o `KernelCore` nascer — o que significa que, no momento em que
    // nasce, ainda não há `KernelCore` para lhe dar.
    void ligar(Kernel::KernelCore& kernel, Service::SM::ServiceManager& sm,
               Reporter& reporter) {
        kernel_ = &kernel;
        sm_ = &sm;
        reporter_ = &reporter;
    }

    // O `RunServer` acima **não corre nada**, e é de propósito.
    //
    // No Eden, `RunServer` arranca as threads que gerem as portas e esperam
    // por pedidos (`ServerManager::LoopProcess`). Aqui não há thread onde
    // correr: os pedidos são atendidos a síncrono, de dentro do
    // `SendSyncRequest`, que é como o motor nativo corre.
    //
    // Guardar a posse continua a ser obrigatório — se o `ServerManager`
    // morresse, o `KPort` da porta "sm:" ficava pendurado e o primeiro
    // `ConnectToNamedPort` do jogo devolveria lixo. Por isso o membro chama-se
    // `servidor_` e não `descartar`.
    const std::vector<std::unique_ptr<Service::ServerManager>>& servidores() const noexcept {
        return servidores_;
    }

private:
    Kernel::KernelCore* kernel_ = nullptr;
    Reporter* reporter_ = nullptr;
    Service::SM::ServiceManager* sm_ = nullptr;
    std::vector<std::unique_ptr<Service::ServerManager>> servidores_;
};

} // namespace Core
