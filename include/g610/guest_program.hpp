// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — programa guest: ExeFS multi-módulo + espaço de endereçamento.
//
// Um jogo de Switch não é um binário. O ExeFS tem vários NSO e o arranque é
// uma coreografia:
//
//   rtld  →  main  →  sdk  →  subsdk0..9
//
// O `rtld` (runtime linker) é o entry point real. Ele mapeia a memória do
// processo, resolve os imports do `main` a partir da `sdk`, e só depois
// chama o entry point do `main`. Isto não é dedução: o `main` do Pokémon
// Shield tem **zero** SVC, e o patch estático encontra 62 SVC na `sdk` e 4 no
// `rtld`. Um programa sem SVC não tem sistema de serviços — logo o `main`
// não é o início.
//
// Carregar só o `main` e correr o seu entry point não pode funcionar: o
// primeiro SVC que o código precisasse de fazer não existiria, e o primeiro
// acesso ao heap não teria memória.
//
// ---------------------------------------------------------------------------
// O QUE ESTE FICHEIRO FAZ E O QUE NÃO FAZ
// ---------------------------------------------------------------------------
// Faz:
//   - carregar os NSO do ExeFS na ordem correcta,
//   - validar que cada um é um NSO e que a imagem cabe,
//   - mapear o espaço de endereçamento do guest no base certo,
//   - correr o patch estático sobre o .text de cada módulo.
//
// Não faz (e é onde o trabalho continua):
//   - resolver imports da sdk (é o que o rtld faz, e precisa de memória do
//     processo, que por sua vez precisa de SVC),
//   - implementar os SVC que o rtld precisa (SetHeapSize, ControlMemory,
//     MapMemory...),
//   - correr. `NativeEngine::run` existe e está validado com um guest
//     sintético; falta-lhe um programa real para correr.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "g610/guest_context.hpp"
#include "g610/guest_memory.hpp"
#include "g610/nso_arguments.hpp"
#include "g610/nso_loader.hpp"
#include "g610/patcher.hpp"
#include "g610/svc_dispatcher.hpp"
#include "g610/telemetry.hpp"

namespace g610 {

// Um módulo do ExeFS, carregado e pronto a ser copiado para o guest.
struct GuestModule {
    LoadedModule nso;

    // Onde vai ficar no espaço do guest. `nso.base` é o base que o loader
    // escolheu para este tipo de espaço de endereçamento; módulos diferentes
    // podem receber bases diferentes (o Eden faz exactamente isso — a sdk é
    // colocada longe do main para não colidir com o heap).
    std::uint64_t vaddr_base = 0;

    // O .text está escrito (o patcher já passou) e pronto a executar.
    bool patched = false;

    const char* name() const {
        static const char* kNames[3] = {"rtld", "main", "sdk"};
        return kNames[static_cast<int>(kind)];
    }

    enum Kind : std::uint8_t { kRtld = 0, kMain = 1, kSdk = 2 };
    Kind kind = kMain;
};

// ---------------------------------------------------------------------------
// GuestProgram
// ---------------------------------------------------------------------------
class GuestProgram {
public:
    GuestProgram() = default;
    explicit GuestProgram(tel::Telemetry& tel) : tel_(&tel) {}

    // Carrega um ExeFS. `exefs_dir` é a pasta com `rtld`, `main`, `sdk`.
    //
    // Não executa. Deixa a imagem pronta para ser copiada para o espaço do
    // guest, que `map_into()` faz.
    bool load(const std::string& exefs_dir, std::string& error);

    // Argumentos que o rtld recebe no arranque.
    //
    // O Switch passa-os num bloco no fim da `.data` do rtld (ver
    // nso_arguments.hpp). Sem isso o rtld lê lixo da stack, a canary falha, e
    // chama `svc #ReturnFromException` — foi o primeiro SVC que pedimos ao
    // rtld real, e o sintoma não apontava para a causa.
    void set_program_args(std::vector<std::string> args) {
        program_args_ = std::move(args);
    }

    // Escreve o bloco de argumentos no fim da `.data` do rtld.
    //
    // Tem de ser chamado ANTES de `map_into()`, porque a imagem do módulo é
    // copiada para o espaço do guest em bloco. A alternativa — escrever
    // depois, no espaço do guest — também funciona, e é mais simples de
    // testar; esta fica aqui porque é a ordem em que o Eden faz.
    bool append_arguments_to_rtld(std::string& error);

    // Mapeia o espaço de endereçamento do guest e copia as imagens.
    //
    // `heap_reserve` é o tamanho a reservar para o heap que o `rtld` vai
    // pedir via SetHeapSize. Reservar já é o que permite ao `rtld` correr:
    // sem essa memória, o primeiro `MapMemory` falha e o arranque morre antes
    // de tocar em código do jogo.
    bool map_into(std::uint64_t heap_reserve, std::string& error);

    // Depois de mapear: aplica o patch estático a cada `.text` e volta a
    // fechar as permissões em R+X. Separado de `map_into` porque o patcher
    // precisa de escrita, e o guest não.
    bool patch_all(std::string& error);

    // O entry point do programa: o do `rtld`, se existir. É o único que pode
    // correr — o do `main` é chamado pelo rtld, não pelo host.
    std::uint64_t entry_point() const;

    // Endereço da pilha do guest (topo), pronto a passar a `run()`.
    std::uint64_t stack_top() const noexcept { return stack_top_; }

    const std::vector<GuestModule>& modules() const noexcept { return modules_; }
    GuestMemory& memory() noexcept { return mem_; }
    const GuestMemory& memory() const noexcept { return mem_; }

    // As regiões do espaço de endereçamento, como o kernel do Switch as
    // descreveria. É isto que o `svcQueryMemory` devolve ao rtld.
    //
    // A regra que decide se um módulo é encontrado está no `rtld` real, não
    // numa especificação: uma região só é aceite se `state == Code` e
    // `permission == Read|Execute`, e só depois de lido o cabeçalho `MOD0`
    // que aponta para `.dynamic`, `.bss` e o objecto de módulo.
    //
    // Por isso os `.text` são Code/R-X e os restantes segmentos são
    // Normal/R e Normal/RW. Um R-X a mais faria o rtld tentar interpretar
    // headers de módulo onde não os há.
    std::vector<GuestRegion> memory_regions() const;

    // Diagnóstico para o `doctor`.
    std::string summary() const;

    // O relatório do patcher, acumulado de todos os módulos.
    //
    // Existe para responder a uma pergunta que o relatório por módulo não
    // responde: **quantos `MSR TPIDR_EL0` há no programa**. O `MRS` é
    // reescrito (um bit) e o `MSR` não é, e a diferença entre os dois decide
    // se o registo `TPIDR_EL0` do host pode servir de TLS do guest — que é a
    // questão que o abort do `SdkMutexType::TryLock` levantou.
    const PatchReport& patch_report() const noexcept { return patch_report_; }

private:
    bool load_one(const std::string& path, GuestModule::Kind kind,
                  std::string& error);
    bool plan_layout(std::uint64_t heap_reserve, std::string& error);

    // Preenche a ThreadLocalRegion da thread principal. Somos o kernel: é
    // esta a função que entrega a thread principal montada, antes de o guest
    // arrancar. Ver a implementação para o porque de não ser o guest a fazê-lo.
    bool init_main_thread_tls(std::string& error);

    std::vector<GuestModule> modules_;
    GuestMemory   mem_;
    tel::Telemetry* tel_ = nullptr;
    std::uint64_t stack_top_ = 0;

    // O relatório do patcher, somado de todos os módulos. Ver
    // `patch_report()`.
    PatchReport patch_report_{};

    // Onde cada módulo vai, e onde o heap começa. Calculado em plan_layout()
    // e guardado para que o diagnóstico e a cópia usem os mesmos números.
    std::uint64_t heap_base_ = 0;
    std::uint64_t heap_size_ = 0;
    std::uint64_t code_end_ = 0;  // fim do último módulo carregado
    std::uint64_t stack_base_ = 0;
    std::uint64_t stack_size_ = 0;
    std::vector<std::string> program_args_;
};

// Layout do espaço do guest.
//
// Números que não são arbitrários: cada região tem de caber o que lá vai, e
// nenhuma pode sobrepor outra. A verificação em `plan_layout()` é o que
// garante isso — foi ela que apanhou a primeira versão deste ficheiro, onde a
// região de código de 64 MiB engolia a pilha.
//
// A ordem de arranque dos módulos (rtld → main → sdk) e o facto de o `main`
// não ter SVC estão em `GuestProgram::load`.
//
//   code   0x8000_0000   rtld + main     (o code_address do Switch, 39-bit)
//   stack  0x8400_0000   8 MiB
//   sdk    0x8500_0000   a biblioteca de serviços
//   heap   0x9000_0000   128 MiB, pedido pelo rtld via SetHeapSize
namespace layout {

// Os módulos são empilhados sequencialmente a partir daqui, como o Eden faz
// (deconstructed_rom_directory.cpp:195-217: `code_size` cresce a cada
// `LoadModule`). O fim real é calculado em `plan_layout`, não fixado aqui:
// com três módulos de 40 MB, uma região de 64 MiB chegaria, mas é uma
// coincidência do jogo actual, não uma propriedade do formato.
inline constexpr std::uint64_t kCodeBase  = 0x0000'0000'8000'0000ull;

// A região de alias do processo.
//
// O Eden, para um endereço de 39 bits, reserva **64 GiB** de alias
// (`k_address_space_info.cpp:36`). Isso são páginas virtuais, que o Eden
// mapeia a preguiçoso; nós somos **nativos**, e 64 GiB seria mais memória
// virtual do que a máquina tem. Logo reservamos uma fracção — o que interessa
// ao SDK é que a região seja válida e esteja no nosso espaço.
//
// Abaixo do código, como no Switch, onde o alias fica antes da região de
// código.
inline constexpr std::uint64_t kAliasBase = 0x0000'0000'6000'0000ull;
//
// O tamanho não é redondo de propósito: termina **exactamente** em
// `kCodeBase`. Um buraco entre a região de alias e o código faz a varrura de
// módulos do rtld parar a meio — ele avança `base + size` e, ao cair num
// endereço não descrito, recebe "Free" e desiste. Foi o que aconteceu na
// primeira versão, com 256 MiB.
inline constexpr std::uint64_t kAliasSize = 0x2000'0000ull;  // 512 MiB

inline constexpr std::uint64_t kStackBase = 0x0000'0000'8400'0000ull;
inline constexpr std::uint64_t kStackSize = 0x0080'0000ull;   // 8 MiB

inline constexpr std::uint64_t kSdkBase   = 0x0000'0000'8500'0000ull;
inline constexpr std::uint64_t kSdkSize   = 0x0400'0000ull;   // 64 MiB

inline constexpr std::uint64_t kHeapBase  = 0x0000'0000'9000'0000ull;
inline constexpr std::uint64_t kHeapSize  = 0x0800'0000ull;   // 128 MiB

// O topo da pilha tem de estar alinhado a 16 bytes (AAPCS64).
constexpr std::uint64_t kStackTop() noexcept { return kStackBase + kStackSize; }

// Onde vive o TLS do guest.
//
// Não é uma escolha do linker: é o valor que o motor escreve em
// `TPIDR_EL0` enquanto o guest corre. O SDK do jogo lê o seu bloco de TLS
// com `mrs x8, tpidrro_el0; ldr x8, [x8, #0x1F8]` (medido no `sdk`, offset
// 0x32f900). Como o guest corre NATIVO no mesmo CPU que o host, esse registo
// pertence à thread do Linux — e o `ldr` seguinte acerta num endereço do
// host, que não está no espaço do guest, e dá fault.
//
// Trocar o registo em vez de descodificar a instrução é o que mantém o
// motor sem emulação: o `mrs` continua a ser executado pelo CPU, e o valor
// que aparece é o do guest.
inline constexpr std::uint64_t kTlsBase = 0x0000'0000'8800'0000ull;
inline constexpr std::uint64_t kTlsSize = 0x0001'0000ull;  // 64 KiB

// O resto do bloco de TLS da thread principal, que o guest não escreve.
//
//   kTlsBase + 0x000 .. +0x200   nn::svc::ThreadLocalRegion
//   kTlsStaticBase .. +0x4000     .tdata + .tbss da thread (variante 1)
//
// Os objectos `Thread*` e `struct _reent*` que a ThreadLocalRegion aponta
// também têm de existir: o SDK desreferencia-os sem os verificar.
inline constexpr std::uint64_t kTlsStaticBase = 0x0000'0000'8810'0000ull;
inline constexpr std::uint64_t kTlsStaticSize = 0x0000'4000ull;
inline constexpr std::uint64_t kThreadObject   = 0x0000'0000'8820'0000ull;
inline constexpr std::uint64_t kReentBase      = 0x0000'0000'8820'1000ull;

// `THREADVARS_MAGIC` — "!TV$". `internal.h` do libnx, linha 6.
inline constexpr std::uint32_t kThreadVarsMagic = 0x21545624u;

// Handle da thread principal. É o que o SDK usa em mutexes, e tem de ser um
// handle plausível: 0 é o handle inválido, e o SDK trata-o como falha.
inline constexpr std::uint32_t kMainThreadHandle = 0x0000'0001u;

// O topo de cada região, arredondado a página.
constexpr std::uint64_t page_align_up(std::uint64_t v) noexcept {
    return (v + 0xFFF) & ~static_cast<std::uint64_t>(0xFFF);
}

} // namespace layout

} // namespace g610