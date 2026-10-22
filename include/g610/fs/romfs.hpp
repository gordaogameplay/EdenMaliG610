// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o RomFS do jogo: ler o NCA do NSP e decifra-lo.
//
// ## O que é
//
// O jogo vive num **NCA** (um "content archive") dentro de um **NSP**. O
// NCA tem o cabecalho cifrado (AES-XTS, a "standard crypto" do Switch) e
// as seccoes cifradas (AES-CTR). O **RomFS** — os ficheiros de dados do
// jogo — e' uma dessas seccoes.
//
// O `fsp-srv` da casa precisa de servir essa imagem: e' o que o `nn::fs`
// do SDK le para encontrar os ficheiros do jogo.
//
// ## Porquê que isto lê o NSP de cada vez
//
// **Medido**: o RomFS do Pokémon Shield tem **10 181 768 972 bytes**
// (9.48 GiB). O Pi tem 11 GB livres. Extrair o RomFS para um ficheiro
// separado nao cabe — e o NSP ja' esta' la. Logo o motor **decifra no
// sitio**, a cada leitura, a partir do NSP.
//
// ## A forma
//
// A disposicao vem do Eden (`core/file_sys/fssystem/`):
//
//   * o **NSP** e' um `PFS0`: um cabecalho, uma tabela de entradas e a
//     tabela de nomes;
//   * o **NCA** tem 0x400 bytes de cabecalho e, a seguir, quatro
//     `NcaFsHeader` de 0x200 bytes (o Eden: `NcaReader::ReadHeader` —
//     `offset = sizeof(NcaHeader) + sizeof(NcaFsHeader) * index`);
//   * o `fs_info` do cabecalho diz onde esta cada seccao (em sectores);
//   * o `fs[i]` cujo `fs_type` e' 0 (RomFs) e' o RomFS.
//
// A *key area* (0x100 bytes do cabecalho) e' cifrada com AES-ECB; a chave
// e' a `key_area_key_application_<master_key_id>`, onde o
// `master_key_id` vem da geracao de chaves do NCA
// (`MasterKeyIdForKeyGeneration`: `max(geracao, 1) - 1`).
//
// **Medido** (este NCA): geracao 8 -> `master_key_id` 7 ->
// `key_area_key_application_07`; a chave AES-CTR do sector e'
// `7919170a...` com o indice 0 e `014a361b...` com o indice 7 — a do
// RomFS e' a segunda.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace g610 {

// As chaves do console.
//
// **Nao vao para o repositorio** (que e' publico): leem-se de um ficheiro
// de chaves **fora** do repositorio — o do Eden, por omissao.
class ChavesConsole {
public:
    // Le um ficheiro de chaves no formato do Eden (`nome = hex`).
    bool Carregar(const std::string& caminho);

    // A chave de 128 bits com este nome (`key_area_key_application_07`).
    std::optional<std::array<std::uint8_t, 16>> Chave(const std::string& nome) const;

    // A chave de 256 bits com este nome (`header_key`).
    std::optional<std::array<std::uint8_t, 32>> Chave256(const std::string& nome) const;

    bool Vazia() const { return chaves_.empty(); }
    std::size_t Numero() const { return chaves_.size(); }

private:
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> chaves_;
};

// O RomFS de um programa, lido do NSP.
class RomfsJogo {
public:
    ~RomfsJogo();
    RomfsJogo() = default;
    RomfsJogo(const RomfsJogo&) = delete;
    RomfsJogo& operator=(const RomfsJogo&) = delete;

    // Abre o NSP, acha o NCA do `program_id` e prepara a decifracao.
    //
    // Devolve `false` (e escreve no log) se o NSP nao estiver la, se as
    // chaves faltarem, ou se o NCA nao for encontrado.
    bool Abrir(const std::string& nsp, std::uint64_t program_id, const ChavesConsole& chaves);

    bool Aberto() const { return fd_ >= 0; }

    // O tamanho da imagem do RomFS, em bytes.
    std::uint64_t Tamanho() const { return tamanho_; }

    // Le `size` bytes a partir de `offset` da imagem do RomFS.
    bool Ler(std::uint64_t offset, std::size_t size, std::uint8_t* destino) const;

    // Onde a imagem vive no NSP (so' para o log).
    std::uint64_t DesvioNoNsp() const { return desvio_imagem_; }

private:
    bool LerNoNsp(std::uint64_t posicao, std::size_t size, std::uint8_t* destino) const;

    int fd_ = -1;
    std::uint64_t desvio_imagem_ = 0;   // no ficheiro do NSP
    std::uint64_t desvio_regiao_ = 0;   // o inicio da seccao dentro do NCA
    std::uint64_t desvio_dados_ = 0;    // o offset dos dados dentro da seccao
    std::uint64_t tamanho_ = 0;         // o tamanho da imagem do RomFS
    std::uint64_t bloco_inicial_ = 0;   // o numero do bloco CTR da imagem
    std::array<std::uint8_t, 16> chave_ctr_{};
    std::array<std::uint8_t, 16> iv_ctr_{};
};

} // namespace g610
