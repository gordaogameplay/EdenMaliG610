// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A **fonte do sistema** — a implementacao.
//
// ## De onde vem a fonte
//
// Os dados vem do **upstream** (`vendor/eden/core/file_sys/system_archive/
// data/font_standard.cpp` ✗) ✗ — que os tem **embutidos em codigo**
// ✗ ✗. Sao um recurso do **Eden** ✗ ✗, nao do firmware da consola ✗ ✗
// — e por isso a casa pode usa-los ✗ ✗ (o mesmo que o Eden ✗ ✗).
//
// **A cifragem** e' a do hardware ✗ — a mesma do upstream ✗ (ver a nota
// em `include/g610/services/shared_font.hpp` ✗) ✗ ✗.
#include "g610/services/shared_font.hpp"

#include <algorithm>
#include <cstring>

#include "vendor/eden/core/file_sys/system_archive/data/font_standard.h"

namespace g610::services {

namespace {

// O que o upstream espera (`platform_service_manager.cpp:38-39` ✗).
constexpr std::uint32_t kResultadoEsperado = 0x7f9a0218;
constexpr std::uint32_t kMagicoEsperado = 0x36f81a1e;

// O `Common::swap32` do upstream — o endianness invertido.
constexpr std::uint32_t Trocar(std::uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

// **A cifragem** ✗ — a do upstream
// (`platform_service_manager.cpp:77-91` ✗) ✗.
//
// A entrada sao os bytes **big-endian** ✗ (o upstream troca-os ✗) ✗ e a
// saida e' o `.bfttf` ✗ (com o cabecalho de 8 bytes ✗) ✗ ✗.
std::vector<std::uint8_t> CifrarFonte(const std::vector<std::uint8_t>& entrada) {
    // Os bytes em palavras, com o endianness trocado ✗ (o upstream ✗).
    std::vector<std::uint32_t> palavras(entrada.size() / sizeof(std::uint32_t));
    if (!palavras.empty()) {
        std::memcpy(palavras.data(), entrada.data(), palavras.size() * sizeof(std::uint32_t));
        for (std::uint32_t& p : palavras) {
            p = Trocar(p);
        }
    }

    const auto chave = Trocar(kResultadoEsperado ^ kMagicoEsperado);

    std::vector<std::uint32_t> transformadas(palavras.size() + 2);
    transformadas[0] = Trocar(kMagicoEsperado);
    transformadas[1] = Trocar(static_cast<std::uint32_t>(palavras.size() * sizeof(std::uint32_t))) ^
                       chave;
    for (std::size_t i = 0; i < palavras.size(); ++i) {
        transformadas[i + 2] = palavras[i] ^ chave;
    }

    std::vector<std::uint8_t> saida(transformadas.size() * sizeof(std::uint32_t));
    if (!transformadas.empty()) {
        std::memcpy(saida.data(), transformadas.data(), saida.size());
    }
    return saida;
}

} // namespace

const std::vector<std::uint8_t>& DadosDaFonteCifrados() {
    static const std::vector<std::uint8_t> dados = [] {
        const auto& fonte = FileSys::SystemArchive::SharedFontData::FONT_STANDARD;
        std::vector<std::uint8_t> entrada(fonte.begin(), fonte.end());
        return CifrarFonte(entrada);
    }();
    return dados;
}

const std::vector<RegiaoDaFonte>& RegioesDaFonte() {
    static const std::vector<RegiaoDaFonte> regioes = [] {
        std::vector<RegiaoDaFonte> r;
        const auto& dados = DadosDaFonteCifrados();
        if (dados.size() <= sizeof(std::uint32_t) * 2) {
            return r;
        }
        // **A regiao** ✗ — o offset comeca em 8 ✗ (o cabecalho ✗) ✗ e o
        // tamanho e' o do ficheiro **sem** o cabecalho ✗ — o upstream
        // (`platform_service_manager.cpp:200-201` ✗) ✗.
        r.push_back(RegiaoDaFonte{8,
                                  static_cast<std::uint32_t>(dados.size() - 2 * sizeof(std::uint32_t))});
        return r;
    }();
    return regioes;
}

} // namespace g610::services
