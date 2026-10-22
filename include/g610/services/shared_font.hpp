// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A **fonte do sistema** — o que o `pl:u` serve ao SDK.
//
// ## O que e'
//
// A fonte que a Switch usa para o texto do sistema e' um ficheiro
// `.bfttf` (uma fonte BFTTF **cifrada**) que vive nos **arquivos do
// sistema** da consola (o `NAND`).
//
// **O Eden nao tem NAND** ✗ — e por isso **sintetiza** a fonte ✗: os
// dados estao **embutidos em codigo** ✗ (`core/file_sys/system_archive/data/
// font_standard.h` ✗) ✗ e sao **cifrados** com a mesma transformacao do
// hardware ✗ ✗.
//
// **A casa faz exactamente o mesmo** ✗ ✗ — e com os **mesmos dados** ✗
// (os do upstream ✗ ✗, que sao um recurso **do Eden** ✗ ✗ e nao do
// firmware da consola ✗ ✗) ✗ ✗.
//
// ## A cifragem
//
// O Eden (`core/hle/service/ns/platform_service_manager.cpp:77-91` ✗):
//
//     const auto key = Common::swap32(EXPECTED_RESULT ^ EXPECTED_MAGIC);
//     std::vector<u32> transformed_font(input.size() + 2);
//     transformed_font[0] = Common::swap32(EXPECTED_MAGIC);
//     transformed_font[1] = Common::swap32(input.size() * 4) ^ key;
//     std::transform(input.begin(), input.end(), transformed_font.begin() + 2,
//                    [key](u32 in) { return in ^ key; });
//
// E o `DecryptSharedFont` (a linha 43 ✗) e' o inverso ✗:
//
//     const u32 KEY = input[0] ^ EXPECTED_RESULT;
//     std::transform(input.begin(), input.end(), transformed_font.begin(),
//                    [&KEY](u32 f) { return Common::swap32(f ^ KEY); });
//     transformed_font[1] = Common::swap32(transformed_font[1]) ^ KEY;
//
// **A regiao da fonte** (`FontRegion` ✗) e' o par (offset, tamanho) ✗
// **dentro da memoria partilhada** ✗ — e o offset comeca em 8 ✗ (o
// cabecalho) ✗ ✗.
//
// O `SHARED_FONT_MEM_SIZE` do upstream e' **17 MB** ✗ (`0x1100000` ✗) ✗.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace g610::services {

// A regiao da fonte na memoria partilhada: o par (offset, tamanho), como
// o `FontRegion` do upstream.
struct RegiaoDaFonte {
    std::uint32_t offset = 0;
    std::uint32_t tamanho = 0;
};

// O tamanho da memoria partilhada da fonte (o `SHARED_FONT_MEM_SIZE` do
// upstream, `platform_service_manager.cpp:40`).
inline constexpr std::size_t kTamanhoDaMemoriaDaFonte = 0x1100000;

// Os dados da fonte **cifrados** (a forma em que o SDK os recebe) e as
// regoes que ocupam.
//
// **Um vector vazio** ✗ significa "sem fonte" ✗ — e e' o que a casa
// devolvia ✗ ✗ (e o que o Eden devolve sem NAND ✗ ✗).
const std::vector<std::uint8_t>& DadosDaFonteCifrados();

// As regoes das fontes, por ordem de prioridade. Vazio = sem fonte.
const std::vector<RegiaoDaFonte>& RegioesDaFonte();

} // namespace g610::services
