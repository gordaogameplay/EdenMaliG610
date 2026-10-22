// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — programa guest multi-módulo.
#include "g610/guest_program.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>

#include "g610/patcher.hpp"

namespace fs = std::filesystem;

namespace g610 {

namespace {

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// Arredonda para cima a um múltiplo do tamanho de página.
std::uint64_t page_align_up(std::uint64_t v) {
    return (v + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
}

} // namespace

bool GuestProgram::load_one(const std::string& path, GuestModule::Kind kind,
                            std::string& error) {
    GuestModule m;
    m.kind = kind;

    NsoLoader loader;
    if (!loader.load(path, m.nso, error)) {
        error = path + ": " + error;
        return false;
    }

    // O NPDM é comum ao programa, mas o carrega uma vez e propaga-se a todos
    // os módulos: o espaço de endereçamento é uma propriedade do processo,
    // não de cada binário. Sem isto, um módulo sem NPDM leria address_space
    // 0 e receberia uma base de 32 bits.
    const std::string npdm = NsoLoader::find_npdm(path);
    if (!npdm.empty()) {
        LoadedModule meta;
        if (loader.load_npdm(npdm, meta, error)) {
            m.nso.has_64_bit       = meta.has_64_bit;
            m.nso.address_space_type = meta.address_space_type;
            m.nso.stack_size       = meta.stack_size;
        }
    }

    if (m.nso.image.empty()) {
        error = path + ": imagem vazia";
        return false;
    }

    modules_.push_back(std::move(m));
    return true;
}

bool GuestProgram::load(const std::string& exefs_dir, std::string& error) {
    modules_.clear();

    std::error_code ec_dir;
    if (!fs::is_directory(exefs_dir, ec_dir)) {
        error = "ExeFS não é um directório: " + exefs_dir;
        return false;
    }

    // A ordem NÃO é alfabética e NÃO é a ordem em que o jogo os menciona: é a
    // ordem de arranque. O rtld mapeia a memória e resolve os imports do main
    // a partir da sdk — sem ele e sem ela, o main não arranca.
    struct Wanted { const char* file; GuestModule::Kind kind; };
    //
    // A ordem é a do Eden (`deconstructed_rom_directory.cpp:191-193`):
    // rtld, main, subsdk0..subsdk9, sdk. O `rtld` vai para o fim do
    // espaço (ver `plan_layout`), mas a ordem RELATIVA do main, dos
    // sub-SDKs e da sdk é a mesma.
    static constexpr Wanted kOrder[] = {
        {"rtld",    GuestModule::kRtld},
        {"main",    GuestModule::kMain},
        {"subsdk0", GuestModule::kSubsdk0},
        {"subsdk1", GuestModule::kSubsdk1},
        {"subsdk2", GuestModule::kSubsdk2},
        {"subsdk3", GuestModule::kSubsdk3},
        {"subsdk4", GuestModule::kSubsdk4},
        {"subsdk5", GuestModule::kSubsdk5},
        {"subsdk6", GuestModule::kSubsdk6},
        {"subsdk7", GuestModule::kSubsdk7},
        {"subsdk8", GuestModule::kSubsdk8},
        {"subsdk9", GuestModule::kSubsdk9},
        {"sdk",     GuestModule::kSdk},
    };

    for (const auto& w : kOrder) {
        const fs::path p = fs::path(exefs_dir) / w.file;
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            // rtld e sdk podem não existir em títulos homesbrew. O main
            // é obrigatório — sem ele não há programa.
            if (w.kind == GuestModule::kMain) {
                error = "falta o 'main' em " + exefs_dir;
                return false;
            }
            continue;
        }
        if (!load_one(p.string(), w.kind, error))
            return false;
    }

    if (modules_.empty()) {
        error = "nenhum módulo carregado de " + exefs_dir;
        return false;
    }

    // O bloco de argumentos vai no fim da imagem do rtld. Sem ele, o rtld lê
    // lixo da stack e chama `svc #ReturnFromException` logo no arranque.
    if (!append_arguments_to_rtld(error))
        return false;

    error.clear();
    return true;
}

bool GuestProgram::plan_layout(std::uint64_t heap_reserve, std::string& error) {
    // O layout é fixo e verificado, não derivado dos tamanhos dos módulos.
    //
    // "Colocar cada módulo a seguir ao anterior" seria mais elegante e
    // ERRADO: o heap tem de estar longe do código (o linker assume que o
    // heap cresce e não invade o executável), e a sdk tem de estar longe do
    // heap pelo mesmo motivo. Um layout sobreposto não dá erro na carga —
    // dá corruption silenciosa muito mais tarde, num SVC que escreve por
    // cima de uma instrução. É o pior tipo de bug, por isso a sobreposição é
    // verificada e o número é concreto.
    heap_size_  = std::max<std::uint64_t>(heap_reserve, layout::kHeapSize);
    heap_base_  = layout::kHeapBase;
    stack_size_ = layout::kStackSize;
    stack_base_ = layout::kStackBase;
    stack_top_  = layout::kStackTop();

    // Os módulos são carregados SEQUENCIALMENTE, cada um no fim do anterior.
    //
    // Fonte: deconstructed_rom_directory.cpp:195-217. `code_size` começa a 0 e
    // `LoadModule` devolve o novo acumulado:
    //
    //     code_size = *tentative_next_load_addr;
    //
    // Ou seja: rtld em code_address, main logo a seguir ao fim do rtld, sdk
    // logo a seguir ao fim do main. Sem gaps entre eles.
    //
    // A primeira versão deste motor inventava um layout com regiões separadas
    // e uma folga de 32 KiB entre o rtld e o main. Funcionava no papel e
    // estava errado: os módulos contêm ponteiros absolutos uns para os outros
    // calculados para esse layout, e movê-los produz leituras de memória
    // alheia. O sintoma foi o rtld carregar endereços do HOST nos seus
    // registos (x19 = 0xffffd7db6f20) sem nenhuma trap pelo caminho.
    // **O `main` (o executavel do jogo) e' linkado para `kCodeBase`**
    // (0x8000'0000), e o `rtld` (o `ldr`) assume isso.
    //
    // Medido: o `rtld` tem **nove ocorrencias de 0x80000000** nos seus
    // dados (o segmento 1, a partir do offset 0x2000) — e' a tabela de
    // onde ele le o endereco do `main`.
    //
    // E o **delta medido** e' de 0x5000: o `adrp` do jogo, que e'
    // relativo ao PC, dava um endereco 0x5000 acima do linkado. A
    // string que o jogo usa na sua assercao esta, no guest, em
    // 0x81b06601 — e o linkador a puser em 0x81b01601.
    //
    // A causa: o `rtld` ocupava 0x5000 (a imagem **do ficheiro**, 0x3DA6,
    // mais o `.bss`, alinhado a 0x1000) quando devia ocupar 0x4000 (o
    // **fim do ultimo segmento**, 0x3280, mais o `.bss` — que e' o que o
    // Eden mede: `nso.cpp:131`, `PageAlignSize(codeset.memory.size() +
    // bss_size)`). A diferenca de uma pagina empurrava o `main` 0x1000
    // para a frente — e o resto do delta vinha do `rtld` estar a
    // piscar o sitio do `main`.
    //
    // O `rtld` e' linkado em **PC-relativo** (o `ldr` e' do sistema, e
    // o linkador nao presume onde ele fica), por isso pode estar em
    // qualquer sitio. O `main` e' linkado para um sitio **fixo**, e o
    // `ldr` le-o de uma tabela.
    //
    // **O `rtld` vai para DEPOIS de todos** — e' o entry point, mas o
    // entry point e' um **endereco**, nao uma ordem.
    std::uint64_t cursor = layout::kCodeBase;
    for (auto& m : modules_) {
        if (m.kind == GuestModule::kRtld)
            continue;   // o `rtld` e' colocado no fim
        const std::uint64_t size =
            static_cast<std::uint64_t>(m.nso.image.size());

        // O `.bss` conta. Não é um detalhe de arredondamento: o `.bss` não
        // ocupa espaço no ficheiro, mas ocupa espaço na memória logo a seguir
        // ao fim da imagem. Se o cursor avançar só pela imagem, o `.bss` do
        // `main` (0x3444f0 bytes medidos) escreve por cima da `sdk` — que fica
        // a seguir — e o rtld passa a ler símbolos que mudam de valor quando
        // outro módulo os escreve.
        //
        // O `svcQueryMemory` denunciava-o: a região de dados do `main`
        // terminava muito depois de a `sdk` já ter começado, e a tabela tinha
        // um "BURACO" em vez de uma sobreposição porque as regiões foram
        // ordenadas por base.
        // **O fim do ultimo segmento**, e nao o tamanho da imagem.
        //
        // O Eden (`nso.cpp:131`):
        //
        //     u32 image_size = PageAlignSize(u32(codeset.memory.size()) +
        //                                        nso_header.segments[2].bss_size);
        //
        // E `codeset.memory.size()` e' `last_segment.location +
        // last_segment.size` — o fim do **ultimo segmento**. A imagem
        // (`module.nso.image`) e' maior: tem as secoes de simbolos
        // atras, **dois megabytes** no `main` deste jogo.
        //
        // A diferenca de uma pagina (ou de varias) empurrava o `sdk`
        // para a frente — e o `sdk` e' linkado **junto com o `main`**,
        // logo tem de ficar exactamente onde o linkador o puser.
        const std::uint64_t fim_do_codigo =
            m.nso.segments[2].vaddr + m.nso.segments[2].size;
        const std::uint64_t footprint = fim_do_codigo + m.nso.bss_size;
        const std::uint64_t aligned = layout::page_align_up(footprint);

        m.vaddr_base = cursor;
        cursor += aligned;
    }
    // O `rtld` vai para o fim: e' linkado em PC-relativo, e o `main` e' que
    // tem o endereco fixo.
    for (auto& m : modules_) {
        if (m.kind != GuestModule::kRtld)
            continue;
        const std::uint64_t fim_do_codigo =
            m.nso.segments[2].vaddr + m.nso.segments[2].size;
        const std::uint64_t footprint = fim_do_codigo + m.nso.bss_size;
        const std::uint64_t aligned = layout::page_align_up(footprint);
        m.vaddr_base = cursor;
        cursor += aligned;
    }
    code_end_ = cursor;


    // Verificação de sobreposição.
    struct Span { std::uint64_t lo, hi; const char* what; };
    const Span spans[] = {
        {layout::kAliasBase, layout::kAliasBase + layout::kAliasSize, "alias"},
        {layout::kCodeBase,  code_end_,                              "code"},
        {stack_base_,         stack_top_,                            "stack"},
        {layout::kTlsBase,   layout::kTlsBase + layout::kTlsSize,     "tls"},
        {heap_base_,         heap_base_ + heap_size_,                "heap"},
    };
    for (std::size_t i = 0; i < std::size(spans); ++i)
        for (std::size_t j = i + 1; j < std::size(spans); ++j)
            if (spans[i].lo < spans[j].hi && spans[j].lo < spans[i].hi) {
                error = std::string("layout do guest sobreposto: ") + spans[i].what +
                        " [" + hex(spans[i].lo) + ", " + hex(spans[i].hi) + ") e " +
                        spans[j].what + " [" + hex(spans[j].lo) + ", " +
                        hex(spans[j].hi) + ")";
                return false;
            }

    // Módulos diferentes não podem partilhar espaço. Isto não é teórico: a
    // primeira versão punha rtld e main na mesma base e o .text do main
    // escrevia por cima do rtld logo no primeiro carregamento.
    for (std::size_t i = 0; i < modules_.size(); ++i)
        for (std::size_t j = i + 1; j < modules_.size(); ++j) {
            const std::uint64_t ai = modules_[i].vaddr_base;
            const std::uint64_t bi = ai + modules_[i].nso.image.size();
            const std::uint64_t aj = modules_[j].vaddr_base;
            const std::uint64_t bj = aj + modules_[j].nso.image.size();
            if (ai < bj && aj < bi) {
                error = std::string("módulos sobrepostos: ") +
                        std::string(modules_[i].name()) + " [" + hex(ai) + ", " +
                        hex(bi) + ") e " + std::string(modules_[j].name()) +
                        " [" + hex(aj) + ", " + hex(bj) + ")";
                return false;
            }
        }

    // Cada módulo tem de caber na sua região. O `main` medido tem 40 MB de
    // imagem; a região de código tem 64 MB — cabe, com 24 MB de folga para o
    // código que o rtld gera (relocações, tabelas de import).
    for (const auto& m : modules_) {
        const std::uint64_t lo = m.vaddr_base;
        const std::uint64_t hi = lo + m.nso.image.size();
        // A base tem de estar alinhada: um mmap desalinhado arredonda para
        // baixo e a imagem aterraria deslocada em todos os segmentos.
        if (lo % kGuestPageSize != 0) {
            error = std::string("base do módulo ") + std::string(m.name()) +
                    " desalinhada a 4096: " + hex(lo);
            return false;
        }
    }

    error.clear();
    return true;
}

bool GuestProgram::append_arguments_to_rtld(std::string& error) {
    for (auto& m : modules_) {
        if (m.kind != GuestModule::kRtld)
            continue;

        const NsoArgumentBlock blk = program_args_.empty()
            ? NsoArgumentBlock::empty()
            : NsoArgumentBlock::build(program_args_);

        // O bloco vai no fim da `.data` do rtld, e a `.data` cresce para
        // incluir o `.bss`. Ou seja: no fim da imagem, depois do bss. É onde
        // o Eden o põe (nso.cpp:123-126), e é onde o rtld o procura.
        const std::size_t old_size = m.nso.image.size();
        m.nso.image.resize(old_size + blk.size(), 0);
        std::memcpy(m.nso.image.data() + old_size, blk.data(), blk.size());

        // O header tem de reflectir a imagem nova, caso alguém volte a
        // calculá-lo.
        break;
    }
    error.clear();
    return true;
}

bool GuestProgram::map_into(std::uint64_t heap_reserve, std::string& error) {
    if (!plan_layout(heap_reserve, error))
        return false;

    // Uma reserva única que cobre tudo: o `owns()` passa a ser uma comparação
    // só, e um módulo nunca fica a meio de uma reserva.
    const std::uint64_t lo = layout::kAliasBase;
    // A reserva tem de cobrir o heap E a regiao da stack (que fica depois
    // dele). O `MAP_NORESERVE` faz o resto: 43 GiB de espaco virtual sao
    // gratis, as paginas so' existem quando o guest as toca.
    const std::uint64_t hi = std::max(heap_base_ + heap_size_, stack_top_);
    (void)code_end_;
    const std::uint64_t span = page_align_up(hi - lo);

    // RWX por agora: o patcher tem de escrever no .text. `patch_all()`
    // volta a fechar em R+X depois. A janela em que o guest tem escrita
    // sobre o seu próprio código é entre map_into() e patch_all(), e não
    // existe guest a correr aí dentro.
    if (!mem_.map(lo, span, Prot::Read | Prot::Write | Prot::Exec, error))
        return false;

    for (auto& m : modules_) {
        if (!mem_.write(m.vaddr_base, m.nso.image.data(),
                        m.nso.image.size(), error)) {
            error = std::string("módulo ") + m.name() + ": " + error;
            return false;
        }

        // **O cabecalho MOD0** — e' o que o rtld do jogo inspeciona
        // (`ldr w8, [base, #4]` — o offset — e depois a magic
        // "MOD0", 0x30444F4D, no offset).
        //
        // Se a magic nao bater, o rtld fica em `b,` (a espera
        // eterna, em `0x8446170c`) — e o jogo nunca mais sai de
        // la. E' o sintoma do "limite de traps".
        {
            std::uint8_t cab[12]{};
            std::string erro;
            if (mem_.read(m.vaddr_base, cab, sizeof cab, erro)) {
                std::fprintf(stderr,
                             "[mod] %-8s base=0x%llx cab=%02x %02x %02x %02x | "
                             "%02x %02x %02x %02x | '%c%c%c%c'\n",
                             m.name(),
                             (unsigned long long)m.vaddr_base, cab[0], cab[1],
                             cab[2], cab[3], cab[4], cab[5], cab[6], cab[7],
                             cab[8], cab[9], cab[10], cab[11]);

                // **O valor PRE-relocacao** — o slot que o rtld vai
                // relocalizar (o global `0x824B8232`, que e' o
                // offset `0x24B5CA0` dentro do modulo).
                //
                // **E' o que diz se a imagem esta' boa**: o rtld
                // escreve `addend + bias` — e o `addend` tem de ser um
                // deslocamento limpo (0x808e), nao lixo.
                if (m.name() == std::string("main") &&
                    m.nso.image.size() > 0x24B5CA0 + 8) {
                    std::uint64_t antes = 0;
                    std::memcpy(&antes, m.nso.image.data() + 0x24B5CA0, 8);
                    std::fprintf(stderr,
                                 "[mod] main  global 0x24B5CA0 (imagem) = 0x%llx\n",
                                 (unsigned long long)antes);
                }
            }
        }
    }

    if (!init_main_thread_tls(error))
        return false;

    error.clear();
    return true;
}

// Preenche a ThreadLocalRegion da thread principal, como o kernel faria.
//
// Isto NÃO é conteúdo do guest. É o kernel a entregar a thread principal já
// montada, e a prova de que é o kernel está no libnx:
//
//     static void _EntryWrap(...) {          // threadCreate — GUEST
//         tv->magic = THREADVARS_MAGIC;  tv->tls_tp = ...;
//     }
//     void __libnx_init_thread(void) {       // thread PRINCIPAL
//         getThreadVars()->thread_ptr = &g_mainThread;   // <-- só isto
//     }
//
// A thread principal não passa por `_EntryWrap`. O libnx escreve-lhe um único
// campo, o que só faz sentido se `magic`, `reent` e `tls_tp` já lá estiverem
// postos — e o único que os pode por ANTES de o guest arrancar é o kernel.
//
// E o nosso medido confirma pelo lado do guest: quando o SDK do jogo falha,
// `tls_tp` ainda está a zero. Se fosse o guest a preenchê-lo, já o tinha
// feito — o SDK já tinha corrido meia-PAGE de código.
//
// Formato: `nn::svc::ThreadLocalRegion`, 0x200 bytes, confirmado no switchbrew
// e no `internal.h` do libnx. `tls_tp` vai em +0x1F8 porque é aí que o
// compilador AArch64 lê o thread pointer — a sequência `mrs; ldr #0x1F8` que
// medimos no sdk (offset 0x32f840).
bool GuestProgram::init_main_thread_tls(std::string& error) {
    // O bloco estático de TLS da thread. Variante 1 do AArch64: o TCB (dois
    // ponteiros) vem primeiro, e os dados (.tdata) e o resto (.tbss) depois.
    //
    // `getTlsStartOffset()` do libnx devolve `max(__tls_align, 16)` e o tp
    // aponta para `dados - esse_offset`. Não sabemos `__tls_align` do SDK da
    // Nintendo, por isso usamos o mínimo garantido (16, o TCB) e tratamos
    // isto como hipótese a refinar pelo próximo fault, não como facto. Está
    // dito aqui porque publishing um número como se fosse medido seria
    // repetir o erro das máscaras de bits.
    constexpr std::uint64_t kTcbSize  = 16;
    const std::uint64_t static_tls = layout::kTlsStaticBase;
    const std::uint64_t tp          = static_tls + kTcbSize;

    struct Thr {
        std::uint32_t magic;
        std::uint32_t handle;
        std::uint64_t thread_ptr;
        std::uint64_t reent;
        std::uint64_t tls_tp;
    };
    static_assert(sizeof(Thr) == 0x20, "a ThreadLocalRegion tem 0x20 bytes no fim");

    Thr t{};
    t.magic      = layout::kThreadVarsMagic;
    t.handle     = layout::kMainThreadHandle;
    t.thread_ptr = layout::kThreadObject;
    t.reent      = layout::kReentBase;
    t.tls_tp     = tp;

    const std::uint64_t tv = layout::kTlsBase + 0x200 - sizeof(Thr);
    if (!mem_.write(tv, &t, sizeof(t), error))
        return false;

    // O handle da thread, em TLS+0x110, é escrito **depois**, por quem
    // conhece o handle verdadeiro: `SvcDispatcher::set_eden_tls`.
    //
    // Ver a nota longa em `SvcDispatcher::set_eden_tls` — o handle tem de ser
    // o da tabela de handles do Eden, e esse só existe depois de o sistema ser
    // montado. Escrever aqui um número diferente seria dar ao loader um handle
    // que não existe na tabela.

    // A região de TLS entregue ao guest tem de estar a zeros à partida. Não
    // é pprint: é o contrato — se houvesse lixo, o SDK ia ler um `magic`
    // errado e recusar a thread em vez de inicializar.
    //
    // O bloco já está dentro da reserva (que é um `mmap` anónimo, logo zeros),
    // por isso não se mapeia nada aqui. Zera-se à mesma, para que isto não
    // dependa de ser anónimo.
    if (!mem_.owns(static_tls) ||
        !mem_.owns(static_tls + layout::kTlsStaticSize - 1)) {
        error = "o bloco de TLS estático (" + hex(static_tls) +
                ") está fora da reserva do guest";
        return false;
    }
    std::vector<std::uint8_t> zeros(layout::kTlsStaticSize, 0);
    if (!mem_.write(static_tls, zeros.data(), zeros.size(), error))
        return false;

    // Os objectos que a ThreadLocalRegion aponta. O SDK desreferencia-os sem
    // os verificar, por isso têm de existir.
    std::vector<std::uint8_t> obj(0x2000, 0);
    if (!mem_.write(layout::kThreadObject, obj.data(), 0x1000, error))
        return false;
    if (!mem_.write(layout::kReentBase, obj.data(), 0x1000, error))
        return false;

    return true;
}

bool GuestProgram::patch_all(std::string& error) {
    if (tel_ == nullptr) {
        error = "GuestProgram sem telemetria: use GuestProgram(tel)";
        return false;
    }
    tel::Telemetry& tel = *tel_;

    for (auto& m : modules_) {
        if (m.nso.segments[kNsoText].size == 0)
            continue;

        // Escreve o .text pela porta do GuestMemory, não pelo vector: o patch
        // tem de cair na memória que o guest vai executar, e não numa cópia.
        std::vector<std::uint8_t>& text_storage = m.nso.image;
        const std::uint64_t text_off = m.nso.segments[kNsoText].vaddr;

        StaticPatcher patcher(tel);
        PatchReport pr{};
        if (!patcher.patch(text_storage, static_cast<std::size_t>(text_off),
                           static_cast<std::size_t>(m.nso.text_size()), pr, error)) {
            error = std::string("módulo ") + std::string(m.name()) + ": " + error;
            return false;
        }

        // Copia o texto patchado de volta para a memória do guest.
        if (!mem_.write(m.vaddr_base + text_off, text_storage.data() + text_off,
                        m.nso.text_size(), error)) {
            error = std::string("módulo ") + std::string(m.name()) + ": " + error;
            return false;
        }

        m.patched = pr.saw_any_svc || pr.hints_nop > 0;

        // Acumula o relatório de todos os módulos. Um módulo sem SVC (o `main`
        // do jogo tem zero) não apaga o que o outro viu: o relatório é do
        // **programa**, não do ficheiro.
        patch_report_.words_scanned += pr.words_scanned;
        patch_report_.svc_patched += pr.svc_patched;
        patch_report_.hints_nop += pr.hints_nop;
        patch_report_.mrs_seen += pr.mrs_seen;
        patch_report_.tpidrro_to_tpidr += pr.tpidrro_to_tpidr;
        patch_report_.msr_seen += pr.msr_seen;
        patch_report_.exclusives_seen += pr.exclusives_seen;
        if (!patch_report_.saw_any_svc && pr.saw_any_svc) {
            patch_report_.saw_any_svc = true;
            patch_report_.first_svc = pr.first_svc;
        }
    }

    error.clear();
    return true;
}

std::uint64_t GuestProgram::entry_point() const {
    // O rtld é o entry point real. O `main` tem zero SVC (medido): é código
    // que só corre depois de o rtld ter preparado tudo, e o seu entry point
    // não é o ponto de entrada do programa.
    for (const auto& m : modules_) {
        if (m.kind == GuestModule::kRtld)
            return m.vaddr_base + m.nso.segments[kNsoText].vaddr;
    }
    for (const auto& m : modules_) {
        if (m.kind == GuestModule::kMain)
            return m.vaddr_base + m.nso.segments[kNsoText].vaddr;
    }
    return 0;
}

std::vector<GuestRegion> GuestProgram::memory_regions() const {
    // As regiões que o `svcQueryMemory` tem de descrever, por ordem de base.
    //
    // Duas invariantes, ambas impostas pelo `rtld` real:
    //
    //   1. Cobrem o espaço SEM BURACOS. O rtld avança o endereço a interrogar
    //      para `base + size` (binário, offset 0x594). Se essa soma cair num
    //      buraco, recebe "Free" com base=0 e size=0, e a comparação
    //      `base + size > actual` falha: a varredura acaba ali. Uma região
    //      faltante não é "um módulo a menos", é "nenhum módulo depois
    //      deste".
    //
    //   2. A primeira começa em 0, porque a primeira interrogação é ao
    //      endereço 0 (offset 0x4a4: `mov x20, xzr`).
    //
    // A segunda leva a uma escolha que quero deixar explícita. A nossa
    // reserva começa em `layout::kCodeBase` (0x8000'0000) e nada abaixo
    // disso é mapeado — é o que `mmap` dá. Mas o rtld começa a interrogar em
    // 0 e, sem nada lá, nunca sai do lugar. Descrevemos [0, kCodeBase) como
    // uma região estática e só de leitura: ela existe para a varredura arrancar
    // e não satisfaz os dois campos que o rtld exige (Code, R-X), por isso não
    // produz candidatos a módulo. Não é memória que tenhamos — é o mínimo
    // que torna a resposta honesta. Está dito aqui porque é uma fiction
    // deliberada, e fiction silenciosa é como nascem os bugs difíceis.
    std::vector<GuestRegion> out;

    using G = GuestRegion;
    out.push_back({0, layout::kAliasBase, G::kStatic, G::kReadOnly});

    // A região de alias. `GetInfo(AliasRegionAddress)` devolve o início
    // DELA, e o SDK aloca dali — se devolvêssemos 0, o `AddressSpaceAllocator`
    // ficava com uma região nula e desreferenciava-a. Foi o que aconteceu
    // antes de isto existir: `svc Break(x0=0)`.
    // A regiao de alias e' **LIVRE**.
    //
    // No Switch a regiao de alias e' o sitio onde o SDK aloca: o
    // `nn::os::detail::AddressSpaceAllocator::AllocateSpace` procura um
    // bloco **Free** para alocar, e so' o Eden devolve os blocos livres com
    // o tamanho certo (`k_memory_block_manager.h:129`, `FindBlock`: a
    // arvore de blocos contem blocos `Free`).
    //
    // Descreve-la como `Normal` (mapeada) tirava ao SDK todo o espaco
    // livre: ele percorria o espaco do processo a perguntar `svcQueryMemory`
    // e recebia sempre um bloco mapeado, sem nenhum tamanho livre onde
    // alocar. Medido: o SDK chegou a fazer **527** interrogacoes
    // `svcQueryMemory` a procurar espaco (as que caem fora do mapeado
    // devolviam `size=0`) e depois lancou uma excecao sem quem a apanhe.
    out.push_back({layout::kAliasBase, layout::kAliasSize, G::kFree,
                   G::kNone});

    // Cada módulo em três regiões, à medida dos segmentos. Os gaps entre
    // segmentos (medidos: 3328 bytes no `main`) ficam absorbidos na região
    // anterior, para que não haja buracos.
    //
    // **Pela ordem das bases, nao pela ordem da lista.** A lista tem o
    // `rtld` primeiro (e' o entry point) mas ele e' linkado em PC-relativo
    // e fica no FIM do codigo - a sua base e' a mais alta de todas.
    // Percorrer a lista por ordem dava ao `rtld` um `module_end` MENOR que
    // a sua base, e a regiao saia invertida (`0x834d5000 .. 0x80000000`).
    // Com o tamanho a dar a volta em `uint64`, essa regiao casava com quase
    // tudo, e o `svcQueryMemory` respondia com a regiao errada.
    //
    // A medicao: com a lista por ordem, o `rtld` (0x834d2000) ficava
    // dentro da regiao de dados do `sdk` (0x833af000..0x834d6000) e o seu
    // proprio `.text` nunca era descrito como `Code`.
    std::vector<std::size_t> por_base;
    por_base.reserve(modules_.size());
    for (std::size_t i = 0; i < modules_.size(); ++i)
        por_base.push_back(i);
    std::sort(por_base.begin(), por_base.end(),
              [this](std::size_t a, std::size_t b) {
                  return modules_[a].vaddr_base < modules_[b].vaddr_base;
              });

    for (std::size_t i = 0; i < por_base.size(); ++i) {
        const GuestModule& m  = modules_[por_base[i]];
        const LoadedModule& n = m.nso;

        const std::uint64_t b = m.vaddr_base;

        // Onde acaba este módulo: no início do seguinte, ou no fim do código
        // se for o último.
        //
        // É esta absorção que fecha os buracos. O `plan_layout` alinha cada
        // módulo a uma página, e essa página deenchimento não pertence a
        // segmento nenhum — mas se não for descrita, o rtld recebe "Free"
        // nesse endereço e a varrura acaba ali. Foi exactamente o que se viu:
        // cinco interrogações, a quinta no enchimento, e o rtld passou
        // directamente a complainar de símbolos sem nunca ter visto a `sdk`.
        const std::uint64_t module_end =
            (i + 1 < por_base.size()) ? modules_[por_base[i + 1]].vaddr_base : code_end_;

        const std::uint64_t text_lo = b + n.segments[0].vaddr;
        const std::uint64_t ro_lo   = b + n.segments[1].vaddr;
        const std::uint64_t data_lo = b + n.segments[2].vaddr;

        // O `.text` vai até ao fim do gap que o precede, para que a região
        // seguinte comece exactamente onde esta acaba.
        out.push_back({text_lo, ro_lo - text_lo, G::kCode, G::kReadExec});
        out.push_back({ro_lo, data_lo - ro_lo, G::kNormal, G::kReadOnly});

        // A `.data` inclui o `.bss` (que não ocupa espaço no ficheiro mas
        // ocupa memória) e estica-se até ao fim do módulo, absorvendo o
        // enchimento de alinhamento.
        out.push_back({data_lo, module_end - data_lo, G::kNormal, G::kReadWrite});
    }

    // A região da stack: 2 GiB, como no Eden
    // (`k_address_space_info.cpp:56`). E **só a stack da thread principal
    // é mapeada** — o resto da região é `Free`, porque no Eden é de lá que o
    // SDK aloca as stacks das threads.
    //
    // Isto não é um detalhe: o `nn::os::detail::AddressSpaceAllocator::
    // AllocateSpace` do SDK escolhe um endereço aleatório dentro da região
    // da stack, pergunta o `svcQueryMemory` e **exige um bloco com estado
    // `Free`** (medido no `sdk`, offset 0x32c13c-0x32c140: o estado vem do
    // `MemoryInfo` que o SVC devolve). Medido: com a região inteira
    // marcada como `Stack`, o SDK gazava 512 endereços e falhava — e
    // lançava uma excepção que ninguém apanhou.
    //
    // O tamanho da stack da thread principal vem do NPDM
    // (`main_thread_stack_size`, offset 0x1C): **1 MiB**, medido. E fica no
    // TOPO da região, que é onde o kernel a põe (e onde o `sp` inicial do
    // guest aponta).
    {
        const std::uint64_t tamanho =
            modules_.empty() ? 0 : modules_.front().nso.stack_size;
        const std::uint64_t regiao = stack_top_ - stack_base_;
        if (tamanho > 0 && tamanho < regiao)
            out.push_back({stack_base_, regiao - tamanho, G::kFree, G::kNone});
        if (tamanho > 0)
            out.push_back({stack_top_ - tamanho, tamanho, G::kStack,
                           G::kReadWrite});
        else if (regiao > 0)
            out.push_back({stack_base_, regiao, G::kStack, G::kReadWrite});
    }

    // O TLS da thread principal. Estado `ThreadLocal` (`svc_types.h:33`).
    {
        const std::uint64_t tls_fim = layout::kReentBase + 0x1000;
        if (tls_fim > layout::kTlsBase)
            out.push_back({layout::kTlsBase, tls_fim - layout::kTlsBase,
                           G::kThreadLocal, G::kReadWrite});
    }

    // O heap. `SetHeapSize` devolve a base daqui, e o SDK aloca a partir
    // dela.
    if (heap_size_ != 0)
        out.push_back({heap_base_, heap_size_, G::kNormal, G::kReadWrite});

    std::sort(out.begin(), out.end(),
              [](const GuestRegion& a, const GuestRegion& b) { return a.base < b.base; });
    return out;
}

std::string GuestProgram::summary() const {
    std::string out = "  módulos     : " + std::to_string(modules_.size()) + "\n";
    for (const auto& m : modules_) {
        out += "    " + std::string(m.name()) + "\n";
        out += "      base     : " + hex(m.vaddr_base) + "\n";
        out += "      entry    : " +
               hex(m.vaddr_base + m.nso.segments[kNsoText].vaddr) + "\n";
        out += "      text     : " + std::to_string(m.nso.text_size()) + " bytes\n";
        out += "      imagem   : " + std::to_string(m.nso.image.size()) + " bytes\n";
        out += "      64-bit   : " + std::string(m.nso.has_64_bit ? "sim" : "não") + "\n";
        out += "      addr spc : " + std::to_string(m.nso.address_space_type) +
               std::string(" (39-bit required)") + "\n";

        // **Os primeiros bytes do modulo em memoria** — e' o que o
        // rtld do jogo inspeciona (`ldr w8, [base, #4]` e depois a
        // magic "MOD0").
        //
        // O formato e' o do **MOD0** (o rtld, em `0x84461504`):
        // no inicio do `.text` vem `{u32, u32 offset_para_o_MOD0}`
        // e no offset a **magic "MOD0"** (0x30444F4D).
        // Se a magic nao bater, o rtld fica em `b,` — a espera
        // eterna — e o jogo nunca mais sai de la.
        {
            std::uint8_t cab[16]{};
            std::string erro;
            if (mem_.read(m.vaddr_base, cab, sizeof cab, erro)) {
                char linha[128];
                std::snprintf(linha, sizeof linha,
                              "      cab      : %02x %02x %02x %02x | %02x %02x %02x %02x"
                              " | '%c%c%c%c'\n",
                              cab[0], cab[1], cab[2], cab[3], cab[4], cab[5], cab[6],
                              cab[7], cab[8], cab[9], cab[10], cab[11]);
                out += linha;
            }
        }
    }
    out += "  stack     : [" + hex(stack_base_) + ", " + hex(stack_top_) + ")\n";
    out += "  heap      : [" + hex(heap_base_) + ", " +
           hex(heap_base_ + heap_size_) + ")\n";
    out += "  espaco    : [" + hex(mem_.base()) + std::string(", ") +
           hex(mem_.base() + mem_.size()) + ")\n";
    out += "  entry     : " + hex(entry_point()) + "\n";
    return out;
}

} // namespace g610
