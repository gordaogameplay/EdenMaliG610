// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A **fonte do sistema** — a implementacao.
//
// ## De onde vem a fonte
//
// Os dados vem do **upstream** (`vendor/eden/core/file_sys/system_archive/
// data/font_*.cpp`) — que os tem **embutidos em codigo**. Sao um recurso do
// **Eden**, nao do firmware da consola — e por isso a casa pode usa-los (o
// mesmo que o Eden).
//
// ## O layout na memoria partilhada
//
// **Medido** (a aritmetica foi reproduzida com `FONT_STANDARD`): o
// `DecryptSharedFont` do Eden escreve, por fonte,
//
//     [ swap32(EXPECTED_RESULT) ] [ tamanho ^ KEY ] [ TTF ]
//
// com `KEY = EXPECTED_RESULT ^ EXPECTED_MAGIC` e `tamanho` em bytes. A
// regiao devolvida ao guest (`FontRegion`) comeca no **offset + 8** e tem o
// tamanho do TTF — e' exactamente o TTF. O cabecalho fica para tras' e nao
// e' lido pelo SDK.
//
// Escrever isto directamente evita o round-trip cifrar/decifrar do Eden e
// garante que o que o guest le em `regiao.offset` e' o TTF.
#include "g610/services/shared_font.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#include "vendor/eden/core/file_sys/system_archive/data/font_chinese_simplified.h"
#include "vendor/eden/core/file_sys/system_archive/data/font_chinese_traditional.h"
#include "vendor/eden/core/file_sys/system_archive/data/font_extended_chinese_simplified.h"
#include "vendor/eden/core/file_sys/system_archive/data/font_korean.h"
#include "vendor/eden/core/file_sys/system_archive/data/font_nintendo_extended.h"
#include "vendor/eden/core/file_sys/system_archive/data/font_standard.h"
#include "vendor/eden/core/hle/kernel/k_event.h"

namespace g610::services {

namespace {

namespace SharedFontData = FileSys::SystemArchive::SharedFontData;

// Os dois valores do upstream (`platform_service_manager.cpp:38-39`).
constexpr std::uint32_t kResultadoEsperado = 0x7f9a0218;
constexpr std::uint32_t kMagicoEsperado = 0x36f81a1e;

// A chave do cabecalho: `EXPECTED_RESULT ^ EXPECTED_MAGIC`.
constexpr std::uint32_t kChave = kResultadoEsperado ^ kMagicoEsperado;

// O `Common::swap32` do upstream — o endianness invertido.
constexpr std::uint32_t Trocar(std::uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

struct FonteEmbutida {
    const unsigned char* dados;
    std::size_t tamanho;
};

// A ordem e' a do `SHARED_FONTS` do Eden (`platform_service_manager.h:42-50`):
// `Standard`, `ChineseSimple` (org + ext), `ChineseTraditional`, `Korean`,
// `Extension` (ext + ext2, os mesmos dados). O `SharedFontType` e' o indice
// directo nesta lista, como no upstream.
const std::array<FonteEmbutida, 7>& Fontes() {
    static const std::array<FonteEmbutida, 7> fontes{{
        {SharedFontData::FONT_STANDARD.data(), SharedFontData::FONT_STANDARD.size()},
        {SharedFontData::FONT_CHINESE_SIMPLIFIED.data(),
         SharedFontData::FONT_CHINESE_SIMPLIFIED.size()},
        {SharedFontData::FONT_EXTENDED_CHINESE_SIMPLIFIED.data(),
         SharedFontData::FONT_EXTENDED_CHINESE_SIMPLIFIED.size()},
        {SharedFontData::FONT_CHINESE_TRADITIONAL.data(),
         SharedFontData::FONT_CHINESE_TRADITIONAL.size()},
        {SharedFontData::FONT_KOREAN.data(), SharedFontData::FONT_KOREAN.size()},
        {SharedFontData::FONT_NINTENDO_EXTENDED.data(),
         SharedFontData::FONT_NINTENDO_EXTENDED.size()},
        {SharedFontData::FONT_NINTENDO_EXTENDED.data(),
         SharedFontData::FONT_NINTENDO_EXTENDED.size()},
    }};
    return fontes;
}

struct FontePartilhada {
    std::vector<std::uint8_t> dados;
    std::vector<RegiaoDaFonte> regioes;
};

// Constroi o blob e as regioes numa so' passagem: os offsets das regioes
// tem de ser os offsets reais dentro do blob.
FontePartilhada Construir() {
    FontePartilhada saida;

    std::size_t total = 0;
    for (const auto& fonte : Fontes()) {
        total += 2 * sizeof(std::uint32_t) + fonte.tamanho;
    }
    saida.dados.reserve(total);
    saida.regioes.reserve(Fontes().size());

    for (const auto& fonte : Fontes()) {
        const std::size_t inicio = saida.dados.size();

        // O cabecalho de 8 bytes, exactamente como o `DecryptSharedFont`
        // do Eden o deixa na memoria partilhada.
        const std::uint32_t cabecalho[2] = {
            Trocar(kResultadoEsperado),
            static_cast<std::uint32_t>(fonte.tamanho) ^ kChave,
        };
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(cabecalho);
        saida.dados.insert(saida.dados.end(), bytes, bytes + sizeof(cabecalho));

        saida.dados.insert(saida.dados.end(), fonte.dados,
                           fonte.dados + fonte.tamanho);

        // O TTF comeca depois do cabecalho — e' o que o SDK le.
        saida.regioes.push_back(RegiaoDaFonte{
            static_cast<std::uint32_t>(inicio + sizeof(cabecalho)),
            static_cast<std::uint32_t>(fonte.tamanho),
        });
    }

    return saida;
}

const FontePartilhada& Fonte() {
    static const FontePartilhada fonte = Construir();
    return fonte;
}

// O evento do `pl:u` (um so' na casa). O `svcMapSharedMemory` compara o
// `KEvent*` do handle com este.
std::mutex g_mutex;
const Kernel::KEvent* g_evento = nullptr;

} // namespace

const std::vector<std::uint8_t>& DadosDaFontePartilhados() {
    return Fonte().dados;
}

const std::vector<RegiaoDaFonte>& RegioesDaFonte() {
    return Fonte().regioes;
}

void RegistarEventoDaFonte(Kernel::KEvent* evento) {
    std::scoped_lock lock{g_mutex};
    g_evento = evento;
}

bool EventoDaFonte(const Kernel::KEvent* evento) {
    if (evento == nullptr) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    return g_evento != nullptr && evento == g_evento;
}

} // namespace g610::services
