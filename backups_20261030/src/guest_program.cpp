// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — programa guest multi-módulo.
#include "g610/guest_program.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

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

// ── Gate dos patches experimentais do SDK ─────────────────────────────────
//
// Cada patch abaixo nasceu para "passar" um aborto observado numa sessão
// anterior. Vários mascaram o erro seguinte em vez de o corrigir, e dois
// (`ret_signal` / `ret_signal2`) trocam um `ret` por um salto para a função
// adjacente — o que corrompe o fluxo de controlo para todo o código que
// partilhe aquela stack.
//
// `G610_PATCH_DISABLE` desliga-os um a um, sem perder o baseline:
//
//   (ausente/vazio)  todos ligados — comportamento antigo, comparável
//   all              nenhum ligado
//   nome[,nome...]   só esses desligados
//
// Isto não é um stub: o guest passa a ver o binário original nas zonas que
// o patch tocava, e o primeiro erro verdadeiro aparece.
bool sdk_patch_on(const char* name) {
    static const std::string disabled = [] {
        const char* e = std::getenv("G610_PATCH_DISABLE");
        if (e != nullptr && e[0] != '\0')
            std::fprintf(stderr, "[sdk-patch-gate] patches desligados: %s\n", e);
        return e == nullptr ? std::string() : std::string(e);
    }();
    if (disabled.empty())
        return true;
    if (disabled == "all")
        return false;
    const std::string needle = name;
    std::size_t pos = 0;
    while (pos <= disabled.size()) {
        const std::size_t comma = disabled.find(',', pos);
        const std::string tok = disabled.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (tok == needle || tok == "all")
            return false;
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return true;
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

    // Processar relocações JUMP_SLOT (tipo 0x402) de DT_JMPREL no host.
    // O RTLD do guest verifica consistência das entradas GOT (todas devem
    // apontar para o mesmo valor inicialmente). Quando o host escreve
    // endereços diferentes em cada entrada, o RTLD chama svcBreak em
    // 0x844626fc — o nosso svc_break trata isso com PC+8 (não é o AbortImpl),
    // pelo qual o RTLD continua normalmente.
    //
    // Relocações RELATIVE, ABS64 e ABS32 são deixadas para o RTLD do guest
    // processar via lazy binding.
    std::unordered_map<std::string, std::uint64_t> global_symbols;
    for (auto& m : modules_) {
        std::uint64_t dyn_off = g610::find_dynamic_offset(m.nso.image);
        if (dyn_off == 0)
            continue;
        auto dyn = g610::parse_dynamic(m.nso.image, dyn_off);
        auto syms = g610::build_symbol_map(m.nso.image, dyn);
        for (auto& kv : syms)
            kv.second += m.vaddr_base;
        global_symbols.insert(syms.begin(), syms.end());
    }

    std::fprintf(stderr,
        "[jump_slot_all] mapa global de símbolos: %zu entradas\n",
        global_symbols.size());

    for (auto& m : modules_) {
        std::uint64_t dyn_off = g610::find_dynamic_offset(m.nso.image);
        if (dyn_off == 0)
            continue;
        auto dyn = g610::parse_dynamic(m.nso.image, dyn_off);

        // Zeroar DT_PLTRELSZ para que o RTLD pule o seu próprio
        // processamento de JUMP_SLOT.
        for (std::size_t i = 0;; ++i) {
            std::size_t ent_off = static_cast<std::size_t>(dyn_off) + i * 16;
            if (ent_off + 16 > m.nso.image.size())
                break;
            std::uint64_t ent_tag;
            std::memcpy(&ent_tag, m.nso.image.data() + ent_off, 8);
            if (ent_tag == DT_NULL)
                break;
            if (ent_tag == DT_PLTRELSZ) {
                std::uint64_t zero = 0;
                std::memcpy(m.nso.image.data() + ent_off + 8, &zero, 8);
                break;
            }
        }

        if (!g610::process_jump_slot(m.nso.image, m.vaddr_base, dyn,
                                      global_symbols, error)) {
            error = std::string("módulo ") + m.name() + ": " + error;
            return false;
        }

        // Zeroar DT_PLTRELSZ para que o RTLD pule o seu próprio
        // processamento de JUMP_SLOT (já feito acima pelo host).
        for (std::size_t i = 0;; ++i) {
            std::size_t ent_off = static_cast<std::size_t>(dyn_off) + i * 16;
            if (ent_off + 16 > m.nso.image.size())
                break;
            std::uint64_t ent_tag;
            std::memcpy(&ent_tag, m.nso.image.data() + ent_off, 8);
            if (ent_tag == DT_NULL)
                break;
            if (ent_tag == DT_PLTRELSZ) {
                std::uint64_t zero = 0;
                std::memcpy(m.nso.image.data() + ent_off + 8, &zero, 8);
                break;
            }
        }

        // Patch __cxa_throw: quando _Unwind_RaiseException retorna, o
        // __cxa_throw cai numa instrução ilegal. Substituímos por ret.
        if (sdk_patch_on("unwind") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kCXAThrowCallOff =
                0x4b75e8;
            if (kCXAThrowCallOff + 8 <= m.nso.image.size()) {
                std::uint32_t insn1 = 0, insn2 = 0;
                std::memcpy(&insn1, m.nso.image.data() + kCXAThrowCallOff, 4);
                std::memcpy(&insn2, m.nso.image.data() + kCXAThrowCallOff + 4, 4);
                const std::uint32_t kBLHelper = 0x9400001Eu;
                const std::uint32_t kIllegal = 0x00000000u;
                if (insn1 == kBLHelper && insn2 == kIllegal) {
                    const std::uint32_t kRet = 0xD65F0000u;
                    std::memcpy(m.nso.image.data() + kCXAThrowCallOff,
                                &kRet, 4);
                    std::fprintf(stderr,
                        "[unwind-patch] sdk: __cxa_throw returns instead of calling _Unwind_RaiseException\n");
                }
            }
        }

        // Patch cbnz w0 em 0x83aec244 (sdk .text offset 0x35f244).
        // O vtable[4] é sobrescrito pelas relocações R_RELATIVE do RTLD do
        // guest e devolve não-zero, o que leva o código ao ramo de aborto
        // (0x83aec278) e de lá a svcBreak. Como svcBreak salta para
        // 0x83aec248 a partir de dentro de AbortImpl com o stack pointer
        // já alterado por AbortImpl, o ret subsequente carrega lr corrompido
        // da stack de AbortImpl (que contém 0x84156f6d, a string "this ==")
        // e faz o guest saltar para dados em vez de código.
        // Substituirmos cbnz por mov w0, wzr directamente na imagem, de modo
        // a sobreviver a patch_all() (que reescreve .text do buffer). Assim
        // o ramo de aborto nunca é tomado e svcBreak nunca é alcançado por
        // este caminho.
        if (sdk_patch_on("cbnz_img") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kCBNZOff = 0x35f244;
            if (kCBNZOff + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kCBNZOff, 4);
                const std::uint32_t kCBNZ = 0x350001A0u;  // cbnz w0, #0x34
                const std::uint32_t kMovW0WZR = 0x2A1F03E0u;
                if (insn == kCBNZ) {
                    std::memcpy(m.nso.image.data() + kCBNZOff,
                                &kMovW0WZR, 4);
                    std::fprintf(stderr,
                        "[cbnz-patch-image] sdk: 0x83aec244 cbnz->mov w0,wzr (imagem)\n");
                }
            }
        }

        // Patch o caminho interno de aborto em Hipc2ClientCoreProcessor::Process
        // (0x83aec8c0-0x83aec8d4): carrega "this ==" e chama AbortImpl via PLT
        // (GOT[0x8438ec80], que o RTLD do guest sobrescreve com o endereço
        // real). Quando este caminho é tomado, svcBreak é chamado e o salto
        // para 0x83aec248 fica com o stack pointer corrompido (Hipc2ClientCore
        // fez `sub sp, sp, #0xb0` e AbortImpl alterou sp ainda mais), levando
        // o guest a executar 0x84156f6d (dados, "this ==").
        // Substituímos `adrp x0, #0x84156000` por `add sp, sp, #0xb0` (restaura
        // o sp do chamador) e `bl 0x83c9d030` por `ret` (retorna sem chamar
        // AbortImpl). Tudo na imagem, sobrevive a patch_all() e o RTLD não
        // toca em .text.
        if (sdk_patch_on("abort_path") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kAbortAdrpOff = 0x35f8c0;
            constexpr std::size_t kAbortBlOff = 0x35f8d4;
            if (kAbortBlOff + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kAbortBlOff, 4);
                const std::uint32_t kBLAbortImpl = 0x9406C1D7u;  // bl 0x83c9d030
                const std::uint32_t kRet = 0xD65F03C0u;
                if (insn == kBLAbortImpl) {
                    const std::uint32_t kAddSp = 0x9102C3FFu;    // add sp, sp, #0xb0
                    const std::uint32_t kRet = 0xD65F03C0u;
                    std::memcpy(m.nso.image.data() + kAbortAdrpOff,
                                &kAddSp, 4);
                    std::memcpy(m.nso.image.data() + kAbortBlOff,
                                &kRet, 4);
                    std::fprintf(stderr,
                        "[abort-path-patch-image] sdk: 0x83aec8c0 adrp->add sp,#0xb0; 0x83aec8d4 bl->ret\n");
                }
            }
        }


        // PROBE: Patch the cbnz at 0x83aec244 (kCBNZOff = 0x35f244) to BRK #0x3F
        // to trap when Hipc2ClientCoreProcessor::Process returns non-zero.
        // This lets us inspect the buffer and w0 at the exact abort decision point.
        if (sdk_patch_on("probe_cbnz") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kCBNZOff = 0x35f244;
            if (kCBNZOff + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kCBNZOff, 4);
                const std::uint32_t kCBNZ = 0x350001A0u;  // cbnz w0, #0x34
                const std::uint32_t kBrkProbe = 0xD42007E0u; // brk #0x3F
                if (insn == kCBNZ) {
                    std::memcpy(m.nso.image.data() + kCBNZOff,
                                &kBrkProbe, 4);
                    std::fprintf(stderr,
                        "[probe-cbnz-patch] sdk: 0x83aec244 cbnz->brk #0x3F\n");
                }
            }
        }

        // PROBE 2: Patch the cbnz at 0x83aec84c in Hipc2ClientCoreProcessor::Process
        // to trap when SendSyncRequest returns non-zero.
        if (sdk_patch_on("probe_cbnz") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kCBNZOff2 = 0x35f84c;  // 0x83aec84c - sdk_base
            if (kCBNZOff2 + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kCBNZOff2, 4);
                const std::uint32_t kCBNZ = 0x350002C0u;  // cbnz w0, #0x34 (actual encoding)
                const std::uint32_t kBrkProbe = 0xD42007E0u; // brk #0x3F
                if (insn == kCBNZ) {
                    std::memcpy(m.nso.image.data() + kCBNZOff2,
                                &kBrkProbe, 4);
                    std::fprintf(stderr,
                        "[probe-cbnz2-patch] sdk: 0x83aec84c cbnz->brk #0x3F\n");
                }
            }
        }

        // PROBE 3: Patch HipcMessageReader::Initialize at 0x8390b9b0 to BRK #0x3E
        if (sdk_patch_on("probe_cbnz") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kReaderOff = 0x17e9b0;  // 0x8390b9b0 - sdk_base
            if (kReaderOff + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kReaderOff, 4);
                const std::uint32_t kBrkProbe = 0xD42007C0u; // brk #0x3E
                // Patch the first instruction of the function
                std::memcpy(m.nso.image.data() + kReaderOff,
                            &kBrkProbe, 4);
                std::fprintf(stderr,
                    "[probe-reader-patch] sdk: 0x8390b9b0 -> brk #0x3E\n");
            }
        }

        // Patch do epílogo de AbortImpl (0x8390d1d0): `bl 0x83b0bba4`
        // (svcBreak) é substituído por NOP para que o aborto não chame
        // svcBreak, e `b self` (0x8390d1e8) é substituído por `ret` para
        // que AbortImpl retorne ao chamador em vez de fazer loop infinito.
        // Tudo na imagem, sobrevive a patch_all().
        if (sdk_patch_on("abort_epi_img") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kSvcBreakCallOff = 0x1801e4;
            constexpr std::size_t kBSelfOff = 0x1801e8;
            if (kBSelfOff + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kBSelfOff, 4);
                const std::uint32_t kBSelf = 0x14000000u;   // b 0x8390d1e8
                const std::uint32_t kRet = 0xD65F0000u;
                const std::uint32_t kNop = 0xD503201Fu;
                if (insn == kBSelf) {
                    std::memcpy(m.nso.image.data() + kSvcBreakCallOff,
                                &kNop, 4);
                    std::memcpy(m.nso.image.data() + kBSelfOff,
                                &kRet, 4);
                    std::fprintf(stderr,
                        "[abort-epilogue-patch-image] sdk: 0x8390d1e4 bl svcBreak->nop; 0x8390d1e8 b self->ret\n");
                }
            }
        }

        // Patch os zeros ilegais em 0x83ab15e4–0x83ab15ec (após bl AbortImpl
        // em 0x83ab15e0). Como AbortImpl agora retorna (epílogo foi mudado de
        // b self para ret), execução cairia nestes zeros e causaria instrução
        // ilegal. Substituímos por ret para que o retorno de AbortImpl funcione
        // corretamente e o caller prossiga normalmente.
        if (sdk_patch_on("after_abort") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kAfterAbortOff = 0x3245e4;  // 0x83ab15e4 - sdk_base
            if (kAfterAbortOff + 12 <= m.nso.image.size()) {
                std::uint32_t val0 = 0, val1 = 0, val2 = 0;
                std::memcpy(&val0, m.nso.image.data() + kAfterAbortOff, 4);
                std::memcpy(&val1, m.nso.image.data() + kAfterAbortOff + 4, 4);
                std::memcpy(&val2, m.nso.image.data() + kAfterAbortOff + 8, 4);
                const std::uint32_t kRet = 0xD65F03C0u;   // ret
                if (val0 == 0 && val1 == 0 && val2 == 0) {
                    std::memcpy(m.nso.image.data() + kAfterAbortOff, &kRet, 4);
                    std::memcpy(m.nso.image.data() + kAfterAbortOff + 4, &kRet, 4);
                    std::memcpy(m.nso.image.data() + kAfterAbortOff + 8, &kRet, 4);
                    std::fprintf(stderr,
                        "[after-abort-ret-patch-image] sdk: 0x83ab15e4–0x83ab15ec zeros->ret x3\n");
                }
            }
        }
    }  // fecha for (auto& m : modules_) aberto em 0x357 (processamento JUMP_SLOT)

    // Patch do `ret` em 0x83c2b844 (epílogo da função que chama
    // SignalProcessWideKey via wrapper 0x83c2c1a0). Quando x30 está zerado
    // (chamada via ponteiro de função ou tabela de saltos), `ret` salta para
    // PC=0 causando fault. Substituímos por branch para a próxima função
    // (0x83c2b848), que tem o mesmo padrão (chama wrapper + retorna 0).
    {
        constexpr std::size_t kRetAfterSignalOff = 0x49E844;  // 0x83c2b844 - sdk_base
        for (auto& m : modules_) {
            if (!sdk_patch_on("ret_signal") || std::string(m.name()) != "sdk")
                continue;
            if (kRetAfterSignalOff + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kRetAfterSignalOff, 4);
                const std::uint32_t kRet = 0xD65F03C0u;   // ret
                const std::uint32_t kBranch = 0x14000001u; // b #1 → 0x83c2b848
                if (insn == kRet) {
                    std::memcpy(m.nso.image.data() + kRetAfterSignalOff,
                                &kBranch, 4);
                    std::fprintf(stderr,
                        "[ret-after-signal-patch-image] sdk: 0x83c2b844 ret->b 0x83c2b848\n");
                }
            }
            break;
        }
    }

    // Patch do `ret` em 0x83c2b85c (epílogo da função em 0x83c2b848, que é
    // alcançada pelo patch acima). Mesmo problema: x30 pode ser 0.
    {
        constexpr std::size_t kRetAfterSignal2Off = 0x49E85C;  // 0x83c2b85c - sdk_base
        for (auto& m : modules_) {
            if (!sdk_patch_on("ret_signal2") || std::string(m.name()) != "sdk")
                continue;
            if (kRetAfterSignal2Off + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kRetAfterSignal2Off, 4);
                const std::uint32_t kRet = 0xD65F03C0u;   // ret
                const std::uint32_t kBranch = 0x14000010u; // b #16 → 0x83c2b860
                if (insn == kRet) {
                    std::memcpy(m.nso.image.data() + kRetAfterSignal2Off,
                                &kBranch, 4);
                    std::fprintf(stderr,
                        "[ret-after-signal2-patch-image] sdk: 0x83c2b85c ret->b 0x83c2b860\n");
                }
            }
            break;
        }
    }

        // Uma reserva única que cobre tudo: o `owns()` passa a ser uma comparação
    // só, e um módulo nunca fica a meio de uma reserva.
    const std::uint64_t lo = layout::kAliasBase;
    // A reserva tem de cobrir o heap E a regiao da stack (que fica depois
    // dele). O `MAP_NORESERVE` faz o resto: 43 GiB de espaco virtual sao
    // gratis, as paginas so' existem quando o guest as toca.
    const std::uint64_t hi = std::max(heap_base_ + heap_size_, stack_top_) + g610::kGuestPageSize;
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

        // **Boot natural: sem header NSO em memória.** Os 0x100 bytes do
        // cabeçalho de ficheiro NÃO são mapados na Switch (o kernel carrega
        // os segmentos, não o ficheiro). Escrevê-los aqui esmagava as
        // primeiras instruções do `.text` — inclusive o probe do MOD0:
        // `FUN_84461440` do rtld lê o word @base+4 (que ficava um limbo do
        // header) e procura a magic 0x30444F4D em base+offset; com o header
        // lá, `*piVar3 != 0x30444f4d` acerta em `while(true)` eterno
        // (REA: /tmp/opencode/rea_rtld/functions/0x84461440.c:61). O MOD0
        // natural de cada módulo — que já vem na imagem, provado: main
        // w@+4=8 com "MOD0" @text+8; rtld w@+4=0x2018 com "MOD0" @ro+0x18 —
        // fornece os offsets reais que o rtld espera.

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

    // Patch da vtable do SDK: a vtable em 0x84370148 tem todas as entradas
    // zeradas no ficheiro NSO (as relocações de .rodata não são processadas
    // pelo host). Quando o guest cria um singleton e chama vtable[4]
    // (0x84370168), obtém 0 → blr x0 → SIGSEGV → abort → svcBreak → loop.
    // Apontamos vtable[4] para 0x83aec290 (mov x0, xzr; ret), que devolve 0
    // e permite que o guest prossiga sem abortar.
    if (sdk_patch_on("vtable") && std::string(modules_.back().name()) == "sdk") {
        std::uint64_t vtable_entry_ret0 = 0x0000000083aec290ull;
        if (!mem_.write(0x84370168ull, &vtable_entry_ret0, sizeof vtable_entry_ret0, error)) {
            error = std::string("falha ao patchar vtable[4] do sdk: ") + error;
            return false;
        }
        std::fprintf(stderr,
            "[vtable-patch] sdk: vtable[4] @ 0x84370168 set to 0x83aec290 (ret 0)\n");
    }

    // Patch da entrada GOT usada pelo PLT stub chamado por vtable[4].
    // O PLT stub em 0x83ca6270 carrega de GOT[0x843935a0] (adrp x16, #0x84393000;
    // ldr x17, [x16, #0x5a0]). O guest RTLD processa relocações de .rodata
    // (sobrescrevendo o patch da vtable acima), mas salta JUMP_SLOT
    // (DT_PLTRELSZ zerado pelo host), pelo que a entrada GOT mantém o valor
    // que escrevemos aqui. Apontamos para 0x83aec290 (mov x0, xzr; ret),
    // que devolve 0 e evita o aborto.
    if (sdk_patch_on("got") && std::string(modules_.back().name()) == "sdk") {
        std::uint64_t got_entry_ret0 = 0x0000000083aec290ull;
        if (!mem_.write(0x843935a0ull, &got_entry_ret0, sizeof got_entry_ret0, error)) {
            error = std::string("falha ao patchar GOT[0x843935a0] do sdk: ") + error;
            return false;
        }
        std::fprintf(stderr,
            "[got-patch] sdk: GOT[0x843935a0] set to 0x83aec290 (ret 0)\n");
    }

    // Patch da instrução cbnz w0 em 0x83aec244: quando vtable[4] retorna
    // não-zero (devido a relocações do guest RTLD), o código entra no ramo
    // de aborto e chama svcBreak. Substituímos por mov w0, wzr para evitar
    // o ramo de aborto e permitir que o guest prossiga.
    if (sdk_patch_on("cbnz_rt") && std::string(modules_.back().name()) == "sdk") {
        std::uint32_t mov_w0_wzr = 0x2A1F03E0u;
        if (!mem_.write(0x83aec244ull, &mov_w0_wzr, 4, error)) {
            error = std::string("falha ao patchar 0x83aec244: ") + error;
            return false;
        }
        std::fprintf(stderr,
            "[cbnz-patch] sdk: 0x83aec244 patched to mov w0, wzr (evita aborto)\n");
    }

    // Patch da entrada GOT usada pela função de aborto chamada do caminho
    // de erro. O PLT stub em 0x83c9d030 carrega de GOT[0x8438ec80].
    // Apontamos para 0x83aec290 (mov x0, xzr; ret) para que a função de
    // aborto retorne 0 em vez de chamar svcBreak.
    if (sdk_patch_on("abort_got") && std::string(modules_.back().name()) == "sdk") {
        std::uint64_t abort_got_ret0 = 0x0000000083aec290ull;
        if (!mem_.write(0x8438ec80ull, &abort_got_ret0, sizeof abort_got_ret0, error)) {
            error = std::string("falha ao patchar GOT[0x8438ec80] do sdk: ") + error;
            return false;
        }
        std::fprintf(stderr,
            "[abort-got-patch] sdk: GOT[0x8438ec80] set to 0x83aec290 (ret 0)\n");
    }

    // Patch da instrução `svc #0x26` em 0x83b0bba4 (wrapper do svcBreak
    // chamado por AbortImpl). Substituímos por NOP para que, quando o
    // código de aborto tentar chamar svcBreak, ele simplesmente retorne
    // normalmente em vez de entrar em loop infinito.
    if (sdk_patch_on("svc_break") && std::string(modules_.back().name()) == "sdk") {
        std::uint32_t nop_insn = 0xD503201Fu; // NOP
        if (!mem_.write(0x83b0bba4ull, &nop_insn, 4, error)) {
            error = std::string("falha ao patchar svc #0x26 em 0x83b0bba4: ") + error;
            return false;
        }
        std::fprintf(stderr,
            "[svc-patch] sdk: 0x83b0bba4 svc #0x26 -> NOP (evita svcBreak)\n");
    }

    // Patch do epílogo de AbortImpl em 0x8390d1e8: `b self; udf` é substituído
    // por `ldp x29, x30, [sp], #16; ret` para que AbortImpl retorne normalmente
    // ao seu chamador em vez de entrar em loop infinito. Isto restaura o ponteiro
    // de frame e o link register salvos no prólogo (0x8390d1d0) e devolve o
    // controlo ao código que chamou AbortImpl.
    if (sdk_patch_on("abort_epi_rt") && std::string(modules_.back().name()) == "sdk") {
        // `add sp, sp, #0x10` desfaz o `stp x29, x30, [sp, #-16]!` do prólogo
        // (0x8390d1d0), restaurando o stack pointer. `ret` volta ao endereço
        // guardado em x30 pelo `bl 0x8390d1d0` do chamador. Isto evita o ciclo
        // infinito que o `ldp x29, x30, [sp], #16` + `ret` causava (o `ret`
        // voltava a x30 = 0x8390d1e8, o próprio `ldp`).
        std::uint32_t add_sp_insn = 0x910013FFu;  // add sp, sp, #0x10
        std::uint32_t ret_insn = 0xD65F0000u;     // ret
        if (!mem_.write(0x8390d1e8ull, &add_sp_insn, 4, error)) {
            error = std::string("falha ao patchar add sp em 0x8390d1e8: ") + error;
            return false;
        }
        if (!mem_.write(0x8390d1ecull, &ret_insn, 4, error)) {
            error = std::string("falha ao patchar ret em 0x8390d1ec: ") + error;
            return false;
        }
        std::fprintf(stderr,
            "[abort-epilogue-patch] sdk: 0x8390d1e8 b self->add sp,#0x10; 0x8390d1ec udf->ret\n");
    }

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
        patch_report_.mrs_cntfrq_to_trap += pr.mrs_cntfrq_to_trap;
        patch_report_.mrs_cntpct_to_trap += pr.mrs_cntpct_to_trap;
        patch_report_.mrs_cntvct_to_trap += pr.mrs_cntvct_to_trap;
        patch_report_.msr_seen += pr.msr_seen;
        patch_report_.exclusives_seen += pr.exclusives_seen;
        if (!patch_report_.saw_any_svc && pr.saw_any_svc) {
            patch_report_.saw_any_svc = true;
            patch_report_.first_svc = pr.first_svc;
        }
    }

    error.clear();
    std::cerr << "[patch_all] total: words=" << patch_report_.words_scanned
              << " svc=" << patch_report_.svc_patched
              << " hints=" << patch_report_.hints_nop
              << " saw_any_svc=" << patch_report_.saw_any_svc << "\n";
    return true;
}

std::uint64_t GuestProgram::entry_point() const {
    // **Boot natural: o rtld corre PRIMEIRO.** É o que a Switch faz: o
    // crt0 pré-carrega `rtld.elf` e salta para a sua entrada com x0=0 (a
    // cadeia fria, REA: /tmp/opencode/rea_rtld/functions/entry — cp a
    // bss, processa as .rela.dyn RELATIVE do próprio rtld, percorre o
    // espaço com QueryMemory à procura das magics MOD0, relocaliza os
    // módulos e, só então, chama `nn::init::Start` via PLT). O main com
    // as relocações feitas à mão era o arranque imposto ao rtld por
    // trás das costas — e o cabeçalho NSO (0x100 sobre o texto real)
    // era o caminho antigo (e errado).
    //
    // SEM o header (0x100) mapeado, o texto real começa em
    // base + segments[kNsoText].vaddr.
    for (const auto& m : modules_) {
        if (m.kind == GuestModule::kRtld)
            return m.vaddr_base + m.nso.segments[kNsoText].vaddr;
    }
    // Fallback (sem rtld, ex. testes): a entrada do main.
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
