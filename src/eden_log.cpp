// Substitui `common/logging.cpp` do Eden.
//
// ## O que o código vendorizado realmente pede
//
// **Dois símbolos**, medidos com `nm` sobre a biblioteca ligada:
//
//     U Common::Log::FmtLogMessageImpl
//     U Common::Log::Stop
//
// E o `logging.cpp` do Eden são 420 linhas que puxam `common/fs.cpp` — que por
// sua vez puxa `GetEdenPath`, `CreateDir`, `IOFile`, `RenameFile`, e um
// directório de relatórios em `~/.local/share/eden`.
//
// ## Porquê trocar em vez de vendorizar
//
// O `logging.cpp` do Eden escreve num **ficheiro**. Nós temos telemetria, que
// é melhor, e um motor que não tem directório de relatórios — não os
// escribemos para algum sítio, e não há thread de log para parar.
//
// Escrever 420 linhas de código de servidor de logs para acabar a imprimir
// duas funções em `stderr` seria vendorizar o emulador outra vez, por um
// caminho diferente. O `FmtLogMessageImpl` é a **fronteira** entre o código do
// Eden e o nosso: é o ponto exacto onde o Eden quer falar com o mundo, e nós
// decidimos com quem ele fala.
//
// ## O que se perde
//
// As entradas do Eden deixam de ir para `eden.log` e passam a ir para o
// `stderr`, que é o mesmo sítio onde sai o resto da telemetria nossa. O
// formato é diferente e o `Class`/`Level` do Eden não chega a ser filtrado
// por `eden.toml` — não há configurador.
//
// **O que não se perde:** o conteúdo. A mensagem é a mesma, com o nível e a
// classe do Eden, e é isso que interessa para descobrir o que o jogo pede.
#include <cstdio>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <string>

#include "vendor/eden/common/logging.h"

namespace Common::Log {

namespace {

// Um mutex por linha de log.
//
// O Eden usa um `SpinLock` porque os seus logs vêm de várias threads do
// emulador. Cá as mensagens vêm do `SendSyncRequest`, que corre na thread do
// guest — uma só. O mutex não é por isso, é por causa das threads de
// telemetria, que também escrevem no mesmo `stderr`.
std::mutex& trinco() {
    static std::mutex m;
    return m;
}

// O nome da classe, montado com o **mesmo ficheiro** que monta o enum.
//
// O `logging.h:79-83` gera o `enum class Class` a partir de
// `common/log_classes.inc`, com `CLS(A)` para as classes de topo e `SUB(A, B)`
// para as sub-classes. Este tabela é montada do mesmo ficheiro, com as mesmas
// duas macros — logo tem exactamente as mesmas entradas, na mesma ordem, e não
// pode ficar desactualizada quando o Eden acrescenta uma.
//
// A primeira versão desta função era um `switch` escrito à mão com seis
// casos. Funcionava, e era mentira: `log_classes.inc` tem 104 linhas e havia
// classes que o meu `switch` não conhecia. Um `switch` parallelo a uma tabela
// alheia é uma tabela que se engana sem avisar.
const char* nome_da_classe(Class c) {
    static const char* const nomes[] = {
#define SUB(A, B) #A "::" #B,
#define CLS(A) #A,
#include "vendor/eden/common/log_classes.inc"
#undef CLS
#undef SUB
    };
    const auto i = static_cast<std::size_t>(c);
    return i < sizeof(nomes) / sizeof(nomes[0]) ? nomes[i] : "?";
}

char nivel_em_caractere(Level l) {
    switch (l) {
    case Level::Trace: return 'T';
    case Level::Debug: return 'D';
    case Level::Info:  return 'I';
    case Level::Warning: return 'W';
    case Level::Error: return 'E';
    case Level::Critical: return 'C';
    default: return '?';
    }
}

} // namespace

void FmtLogMessageImpl(Class log_class, Level log_level, const char* /*filename*/,
                       unsigned int /*line_num*/, const char* /*function*/,
                       fmt::string_view format, const fmt::format_args& args) {
    // `fmt::vformat` monta a mensagem com os argumentos. É o mesmo motor que o
    // Eden usa, portanto `{:#x}` e os resto saem iguais — e isso importa,
    // porque uma mensagem de log com o `{:#x}` diferente da do Eden é uma
    // mensagem em que não se pode confiar.
    std::string texto;
    fmt::vformat_to(std::back_inserter(texto), format, args);

    std::lock_guard<std::mutex> lk(trinco());
    std::fprintf(stderr, "[eden:%c] %s: %s\n", nivel_em_caractere(log_level),
                 nome_da_classe(log_class), texto.c_str());
}

// Não há thread de log para parar — o `logging.cpp` do Eden arranca uma em
// `Start()` e junta-a em `Stop()`.
//
// Vira um no-op com o nome certo, pela mesma razão que `KScopedDisableDispatch`
// e `KScopedSchedulerLock` viram: a função existe porque alguém a chama, e o
// que ela prometia não se aplica a este motor.
void Stop() {}

} // namespace Common::Log