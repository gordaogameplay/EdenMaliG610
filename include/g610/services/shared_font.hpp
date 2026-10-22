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
// **O Eden nao tem NAND** — e por isso **sintetiza** a fonte: os dados
// estao **embutidos em codigo** (`core/file_sys/system_archive/data/
// font_*.h`) e sao cifrados com a mesma transformacao do hardware.
//
// **A casa faz exactamente o mesmo** — e com os **mesmos dados** (os do
// upstream, que sao um recurso **do Eden** e nao do firmware da consola).
//
// ## Porque sao **sete** regioes e nao uma
//
// **Medido** (`/tmp/boot_watch.log`): o jogo pede `GetSize`/`GetSharedMemory-
// AddressOffset` para os tipos 4, 2, 1 e 3, e a casa so' tinha a regiao 0.
// Todos os pedidos devolviam **0**, e o jogo morria a seguir no indice 2 do
// ciclo das 5 fontes (`0x8177d290`, o glifo `?` de uma fonte vazia).
//
// O Eden tem **7** entradas em `SHARED_FONTS`
// (`platform_service_manager.h:42-50`): a standard, duas chinesas
// simplificadas, a chinesa tradicional, a coreana, e a `ext` **duas vezes**
// (`nintendo_ext_003.bfttf` e `nintendo_ext2_003.bfttf`, com os mesmos
// dados). As regioes seguem essa ordem, e o tipo do `SharedFontType` e' o
// indice directo — como no upstream.
//
// ## A cifragem e a memoria partilhada
//
// O Eden (`core/hle/service/ns/platform_service_manager.cpp:77-91`)
// cifra a fonte e o `DecryptSharedFont` (a linha 43) escreve-a **ja'
// decifrada** na memoria partilhada. A regiao comeca **8 bytes depois**
// do inicio de cada fonte: os primeiros 8 bytes sao o cabecalho
// (`swap32(EXPECTED_RESULT)` e o tamanho com a chave), e o TTF comeca no
// offset da regiao.
//
// **A casa escreve o mesmo layout directamente** — nao ha' round-trip
// cifrar/decifrar. O que o guest le em `regiao.offset` e' o TTF.
//
// O `SHARED_FONT_MEM_SIZE` do upstream e' **17 MB** (`0x1100000`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Kernel {
class KEvent;
} // namespace Kernel

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

// O que vai para a memoria partilhada: para cada fonte, o cabecalho de 8
// bytes e o TTF. E' o layout **ja' decifrado** — o mesmo que o
// `DecryptSharedFont` do Eden escreve.
const std::vector<std::uint8_t>& DadosDaFontePartilhados();

// As regioes das fontes, por ordem de prioridade (a ordem do `SHARED_FONTS`
// do Eden). Vazio = sem fonte.
const std::vector<RegiaoDaFonte>& RegioesDaFonte();

// O evento do `pl:u` que representa a memoria partilhada. O kernel
// vendorizado nao tem `KSharedMemory`; o guest recebe um handle para este
// evento, e o `svcMapSharedMemory` reconhece-o para copiar o blob para o
// endereco que o guest escolheu.
//
// **Medido**: sem esta copia, o `svcMapSharedMemory` da casa era um no-op e
// o guest lia zeros onde devia estar o TTF.
void RegistarEventoDaFonte(Kernel::KEvent* evento);
bool EventoDaFonte(const Kernel::KEvent* evento);

} // namespace g610::services
