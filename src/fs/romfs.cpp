// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o RomFS do jogo: ler o NCA do NSP e decifra-lo.
//
// A forma vem do Eden (`core/file_sys/fssystem/`); a implementacao e' nossa.
// Ver `include/g610/fs/romfs.hpp` para o mapa.
#include "g610/fs/romfs.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "g610/crypto/aes.hpp"
#include "vendor/eden/common/logging.h"

namespace g610 {

namespace {

// O tamanho do cabecalho do NCA (o Eden: `NcaHeader::Size`).
constexpr std::size_t kCabecalhoNca = 0x400;
// O tamanho de um `NcaFsHeader` (o Eden: `NcaFsHeader::Size`).
constexpr std::size_t kCabecalhoFs = 0x200;
// O cabecalho + os 4 `NcaFsHeader`.
constexpr std::size_t kCabecalhoNcaMaisFs = kCabecalhoNca + 4 * kCabecalhoFs;

// Os campos do `NcaFsHeader` (o Eden: `fssystem_nca_header.h`).
constexpr std::size_t kFsType = 0x02;
constexpr std::size_t kHashType = 0x03;
constexpr std::size_t kEncryptionType = 0x04;
constexpr std::size_t kAesCtrUpperIv = 0x140;

// `HashType` (o Eden: `NcaFsHeader::HashType`).
constexpr std::uint8_t kHashHierarchicalSha256 = 2;
constexpr std::uint8_t kHashHierarchicalIntegrity = 3;

// `EncryptionType` (o Eden: `NcaFsHeader::EncryptionType`).
constexpr std::uint8_t kCifraAesCtr = 3;

// O offset dos dados dentro da *key area* (o Eden:
// `NcaHeader::DecryptionKey_AesCtr` = 2).
constexpr std::size_t kChaveCtrNaKeyArea = 2 * 16;

std::uint16_t Ler16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t Ler32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t Ler64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

// Hex -> bytes. Devolve `false` se o texto nao for hex valido.
bool DeHex(const std::string& texto, std::vector<std::uint8_t>& saida) {
    if (texto.size() % 2 != 0) {
        return false;
    }
    saida.clear();
    saida.reserve(texto.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < texto.size(); i += 2) {
        const int a = nibble(texto[i]);
        const int b = nibble(texto[i + 1]);
        if (a < 0 || b < 0) {
            return false;
        }
        saida.push_back(static_cast<std::uint8_t>((a << 4) | b));
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// As chaves
// ─────────────────────────────────────────────────────────────────────────────

bool ChavesConsole::Carregar(const std::string& caminho) {
    chaves_.clear();
    std::ifstream ficheiro(caminho);
    if (!ficheiro.is_open()) {
        LOG_ERROR(Service_FS, "ChavesConsole: nao abri o ficheiro de chaves '{}'", caminho);
        return false;
    }
    std::string linha;
    while (std::getline(ficheiro, linha)) {
        const auto igual = linha.find('=');
        if (igual == std::string::npos) {
            continue;
        }
        std::string nome = linha.substr(0, igual);
        std::string valor = linha.substr(igual + 1);
        // O Eden remove os espacos dos dois lados (`ipc_helpers`/`key_manager`).
        const auto cortar = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        };
        cortar(nome);
        cortar(valor);
        if (nome.empty() || nome[0] == '#') {
            continue;
        }
        std::vector<std::uint8_t> bytes;
        if (!DeHex(valor, bytes) || bytes.empty()) {
            continue;
        }
        chaves_.emplace_back(nome, std::move(bytes));
    }
    LOG_DEBUG(Service_FS, "ChavesConsole: {} chaves de '{}'", chaves_.size(), caminho);
    return !chaves_.empty();
}

std::optional<std::array<std::uint8_t, 16>> ChavesConsole::Chave(const std::string& nome) const {
    for (const auto& [n, v] : chaves_) {
        if (n == nome && v.size() == 16) {
            std::array<std::uint8_t, 16> out{};
            std::memcpy(out.data(), v.data(), 16);
            return out;
        }
    }
    return std::nullopt;
}

std::optional<std::array<std::uint8_t, 32>> ChavesConsole::Chave256(const std::string& nome) const {
    for (const auto& [n, v] : chaves_) {
        if (n == nome && v.size() == 32) {
            std::array<std::uint8_t, 32> out{};
            std::memcpy(out.data(), v.data(), 32);
            return out;
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// O RomFS
// ─────────────────────────────────────────────────────────────────────────────

RomfsJogo::~RomfsJogo() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool RomfsJogo::LerNoNsp(std::uint64_t posicao, std::size_t size, std::uint8_t* destino) const {
    std::size_t feito = 0;
    while (feito < size) {
        const ssize_t n = ::pread(fd_, destino + feito, size - feito,
                                  static_cast<off_t>(posicao + feito));
        if (n <= 0) {
            return false;
        }
        feito += static_cast<std::size_t>(n);
    }
    return true;
}

bool RomfsJogo::Abrir(const std::string& nsp, std::uint64_t program_id,
                      const ChavesConsole& chaves) {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    fd_ = ::open(nsp.c_str(), O_RDONLY);
    if (fd_ < 0) {
        LOG_ERROR(Service_FS, "RomfsJogo: nao abri o NSP '{}'", nsp);
        return false;
    }

    // ── o PFS0 ─────────────────────────────────────────────────────────────
    std::uint8_t cab[0x10]{};
    if (!LerNoNsp(0, sizeof cab, cab) || std::memcmp(cab, "PFS0", 4) != 0) {
        LOG_ERROR(Service_FS, "RomfsJogo: '{}' nao e' um PFS0", nsp);
        return false;
    }
    const std::uint32_t entradas = Ler32(cab + 4);
    const std::uint32_t tam_nomes = Ler32(cab + 8);
    const std::uint64_t base_tabela = 0x10 + static_cast<std::uint64_t>(entradas) * 24;
    const std::uint64_t base_dados = base_tabela + tam_nomes;

    // ── a chave do cabecalho (AES-XTS) ─────────────────────────────────────
    const auto header_key = chaves.Chave256("header_key");
    if (!header_key) {
        LOG_ERROR(Service_FS, "RomfsJogo: falta a 'header_key'");
        return false;
    }
    crypto::Chave128 metade1{};
    crypto::Chave128 metade2{};
    for (std::size_t i = 0; i < 16; ++i) {
        metade1[i] = (*header_key)[i];
        metade2[i] = (*header_key)[16 + i];
    }
    crypto::Bloco k1{metade1};
    crypto::Bloco k2{metade2};

    // ── procura o NCA do programa ──────────────────────────────────────────
    std::vector<std::uint8_t> buf(kCabecalhoNcaMaisFs);
    std::vector<std::uint8_t> dec(buf.size());
    std::uint64_t base_nca = 0;
    std::uint64_t prog_encontrado = 0;
    bool achou = false;

    for (std::uint32_t i = 0; i < entradas && !achou; ++i) {
        std::uint8_t entrada[24]{};
        if (!LerNoNsp(0x10 + static_cast<std::uint64_t>(i) * 24, sizeof entrada, entrada)) {
            continue;
        }
        const std::uint64_t off = Ler64(entrada);
        const std::uint64_t tam = Ler64(entrada + 8);
        const std::uint64_t pos = base_dados + off;
        if (tam < kCabecalhoNcaMaisFs) {
            continue;
        }
        if (!LerNoNsp(pos, buf.size(), buf.data())) {
            continue;
        }
        crypto::XtsDecifrar(k1, k2, buf, dec, 0);
        if (std::memcmp(dec.data() + 0x200, "NCA3", 4) != 0) {
            continue;
        }
        // O `content_type`: 0 = Program (o jogo), 1 = Data, 2 = Meta,
        // 3 = Control, 4 = Manual. So' o Program tem o RomFS.
        //
        // **Medido** (este NSP): os NCAs sao Program, Meta, Control,
        // Manual e Data — **todos** com o mesmo `program_id`, por isso o
        // `program_id` nao chega para escolher: o `content_type` e' que
        // sim.
        const std::uint8_t content_type = dec[0x205];
        if (content_type != 0) {
            LOG_DEBUG(Service_FS, "RomfsJogo: NCA {:#x} ignorado (content_type={})",
                      Ler64(dec.data() + 0x210), content_type);
            continue;
        }
        const std::uint64_t prog = Ler64(dec.data() + 0x210);
        LOG_DEBUG(Service_FS, "RomfsJogo: NCA de programa (program_id={:#x})", prog);
        if (program_id != 0 && prog != program_id) {
            LOG_WARNING(Service_FS, "RomfsJogo: o NCA e' {:#x} e nao {:#x}", prog, program_id);
            continue;
        }
        base_nca = pos;
        prog_encontrado = prog;
        achou = true;
    }

    if (!achou) {
        LOG_ERROR(Service_FS, "RomfsJogo: nenhum NCA de programa no NSP");
        return false;
    }

    // ── o cabecalho do NCA ─────────────────────────────────────────────────
    if (!LerNoNsp(base_nca, buf.size(), buf.data())) {
        return false;
    }
    crypto::XtsDecifrar(k1, k2, buf, dec, 0);

    // A geracao de chaves: `max(key_generation, key_generation_2)`.
    const std::uint8_t geracao =
        std::max(dec[0x206], dec[0x220]);
    // `MasterKeyIdForKeyGeneration`: `max(geracao, 1) - 1`.
    const int master_key_id = std::max<int>(geracao, 1) - 1;
    // O indice da *key area* (0 = Application, 1 = Ocean, 2 = System).
    const std::uint8_t indice_key_area = dec[0x207];

    char nome_chave[64];
    std::snprintf(nome_chave, sizeof nome_chave, "key_area_key_application_%02x",
                  master_key_id);
    const auto chave_key_area = chaves.Chave(nome_chave);
    if (!chave_key_area) {
        LOG_ERROR(Service_FS, "RomfsJogo: falta a chave '{}'", nome_chave);
        return false;
    }
    crypto::Bloco bloco_key_area{*chave_key_area};

    // A *key area* tem 8 chaves de 16 bytes; a do AES-CTR e' a indice 2
    // (o Eden: `NcaHeader::DecryptionKey_AesCtr`).
    //
    // **O bloco e' decifrado como um bloco.** O ECB nao tem corrente: o
    // bloco de 16 bytes da *key area* sai de uma so decifracao. Decifrar
    // byte a byte (fazendo 16 decifracoes de 16 bytes e tomando o
    // primeiro byte de cada) da' outra coisa — e uma coisa que parece
    // plausivel.
    std::array<std::uint8_t, 16> chave_ctr{};
    {
        std::uint8_t bloco[16]{};
        bloco_key_area.Decifrar(
            std::span<const std::uint8_t>(dec.data() + 0x300 + kChaveCtrNaKeyArea, 16),
            std::span<std::uint8_t>(bloco, 16));
        for (std::size_t i = 0; i < 16; ++i) {
            chave_ctr[i] = bloco[i];
        }
    }

    // ── a seccao do RomFS ──────────────────────────────────────────────────
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t* fs = dec.data() + kCabecalhoNca + static_cast<std::size_t>(i) * kCabecalhoFs;
        if (fs[kFsType] != 0) { // RomFs
            continue;
        }
        const std::uint8_t cifragem = fs[kEncryptionType];
        if (cifragem != kCifraAesCtr) {
            LOG_WARNING(Service_FS, "RomfsJogo: o RomFS nao e' AES-CTR (e' {})", cifragem);
            continue;
        }
        // O `fs_info`: sectores inicial e final.
        const std::uint8_t* info = dec.data() + 0x240 + static_cast<std::size_t>(i) * 0x10;
        const std::uint64_t setor_ini = Ler32(info);
        const std::uint64_t setor_fim = Ler32(info + 4);
        if (setor_ini == 0 && setor_fim == 0) {
            continue;
        }
        const std::uint64_t off_regiao = setor_ini * 0x200;
        const std::uint64_t tam_regiao = (setor_fim - setor_ini) * 0x200;

        // Onde comeca' os dados dentro da seccao: o *hash target*.
        //
        // O Eden: `NcaFsHeader::GetHashTargetOffset` — para o hash
        // hierarquico de integridade e' `info[max_layers - 2].offset`; para
        // o SHA-256 hierarquico e' a ultima camada.
        std::uint64_t off_dados = 0;
        std::uint64_t tam_dados = tam_regiao;
        const std::uint8_t hash_type = fs[kHashType];
        if (hash_type == kHashHierarchicalIntegrity) {
            const std::uint32_t max_layers = Ler32(fs + 0x14);
            if (max_layers < 3 || max_layers > 7) {
                continue;
            }
            const std::uint8_t* nivel = fs + 0x18 + static_cast<std::size_t>(max_layers - 2) * 24;
            off_dados = Ler64(nivel);
            tam_dados = Ler64(nivel + 8);
        } else if (hash_type == kHashHierarchicalSha256) {
            const std::uint32_t camadas = Ler32(fs + 0x18 + 0x20);
            if (camadas == 0 || camadas > 5) {
                continue;
            }
            const std::uint8_t* nivel =
                fs + 0x18 + 0x28 + static_cast<std::size_t>(camadas - 1) * 16;
            off_dados = Ler64(nivel);
            tam_dados = tam_regiao - off_dados;
        } else {
            LOG_WARNING(Service_FS, "RomfsJogo: hash tipo {} — nao sei onde esta' os dados",
                        hash_type);
            continue;
        }

        // O "upper IV" da seccao (o Eden: `NcaFsHeader::aes_ctr_upper_iv`).
        const std::uint64_t upper = Ler64(fs + kAesCtrUpperIv);

        // O contador do CTR: `BE64(upper) || BE64(bloco)`, e o bloco
        // inicial e' o do **inicio da seccao** (o Eden:
        // `AesCtrStorage::MakeIv(iv, size, upper, fs_data_offset)`).
        iv_ctr_ = {};
        for (int k = 0; k < 8; ++k) {
            iv_ctr_[k] = static_cast<std::uint8_t>((upper >> (8 * (7 - k))) & 0xFF);
        }
        const std::uint64_t bloco_inicial = (off_regiao + off_dados) / 0x10;
        for (int k = 0; k < 8; ++k) {
            iv_ctr_[8 + k] = static_cast<std::uint8_t>((bloco_inicial >> (8 * (7 - k))) & 0xFF);
        }
        chave_ctr_ = chave_ctr;
        bloco_inicial_ = bloco_inicial;
        desvio_regiao_ = off_regiao;
        desvio_dados_ = off_dados;
        desvio_imagem_ = base_nca + off_regiao + off_dados;
        tamanho_ = tam_dados;

        LOG_DEBUG(Service_FS,
                  "RomfsJogo: RomFS pronto (program_id={:#x}) — {} bytes em {:#x} do NSP",
                  prog_encontrado, tamanho_, desvio_imagem_);

        // **A verificacao**: o cabecalho do RomFS comeca' com o tamanho
        // dele proprio (0x50). Se a decifracao estiver errada, o primeiro
        // valor nao e' 0x50 — e e' melhor saber-se aqui do que no guest,
        // que aborta com um codigo que nao diz nada.
        std::uint8_t cab[16]{};
        LOG_DEBUG(Service_FS, "RomfsJogo: a verificar o cabecalho...");
        if (!Ler(0, sizeof cab, cab)) {
            LOG_ERROR(Service_FS, "RomfsJogo: nao consegui ler o cabecalho do RomFS");
            return false;
        }
        std::uint64_t tamanho_cab = 0;
        std::uint64_t segundo = 0;
        for (int i = 7; i >= 0; --i) {
            tamanho_cab = (tamanho_cab << 8) | cab[i];
        }
        for (int i = 15; i >= 8; --i) {
            segundo = (segundo << 8) | cab[i];
        }
        LOG_DEBUG(Service_FS, "RomfsJogo: cabecalho lido: {:#x} {:#x}", tamanho_cab, segundo);
        if (tamanho_cab != 0x50) {
            LOG_ERROR(Service_FS,
                      "RomfsJogo: o cabecalho do RomFS nao tem o tamanho certo ({:#x}, "
                      "esperado 0x50) — a decifracao esta' errada",
                      tamanho_cab);
            return false;
        }
        LOG_DEBUG(Service_FS, "RomfsJogo: cabecalho do RomFS verificado");
        return true;
    }

    LOG_ERROR(Service_FS, "RomfsJogo: o NCA nao tem RomFS");
    return false;
}

bool RomfsJogo::Ler(std::uint64_t offset, std::size_t size, std::uint8_t* destino) const {
    if (fd_ < 0 || size == 0) {
        return size == 0;
    }
    if (offset >= tamanho_) {
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(
        std::min<std::uint64_t>(size, tamanho_ - offset));

    // **O contador do CTR e' relativo a seccao, nao ao ficheiro.**
    //
    // O Eden (`AesCtrStorage::MakeIv` + `Read`): o contador comeca' em
    // `fs_data_offset / 16` e o `Read` soma `offset / 16`, onde o
    // `offset` e' relativo ao armazenamento — que comeca' no inicio da
    // seccao. Logo o bloco de uma leitura e':
    //
    //     (off_regiao + off_dados + X) / 16
    //
    // e nao `desvio_no_ficheiro / 16`. Confundir as duas coisas da' lixo
    // que parece texto — e, se o resultado for *menor*, um salto negativo
    // que vira um contador de 2^64 voltas (e o motor fica pendurado).
    const std::uint64_t posicao_regiao = desvio_regiao_ + desvio_dados_ + offset;
    const std::uint64_t bloco = posicao_regiao / 0x10;
    const std::size_t resto = static_cast<std::size_t>(posicao_regiao % 0x10);

    // O ficheiro tambem e' lido a partir do bloco alinhado.
    const std::uint64_t posicao_ficheiro = desvio_imagem_ + offset - resto;
    std::vector<std::uint8_t> buffer(n + resto);
    if (!LerNoNsp(posicao_ficheiro, buffer.size(), buffer.data())) {
        return false;
    }

    crypto::Bloco bloco_ctr{chave_ctr_};
    // O contador: os 8 primeiros bytes sao o "upper" (fixo) e os 8
    // seguintes sao o numero do bloco.
    //
    // **Calcula-se, nao conta-se.** O Eden avanca o contador de um em um
    // (`AddCounter`), mas isso e' um avanco por bloco de 16 bytes: para
    // uma leitura no fim de uma imagem de 9.5 GiB sao ~640 milhoes de
    // voltas — e o motor fica la preso. O contador e' um numero de 128
    // bits big-endian, por isso escreve-se logo o valor certo.
    auto contador = iv_ctr_;
    for (int k = 0; k < 8; ++k) {
        contador[8 + k] = static_cast<std::uint8_t>((bloco >> (8 * (7 - k))) & 0xFF);
    }
    crypto::CtrDecifrar(bloco_ctr, std::span<const std::uint8_t>(buffer.data(), buffer.size()),
                        std::span<std::uint8_t>(buffer.data(), buffer.size()), contador);
    std::memcpy(destino, buffer.data() + resto, n);
    return true;
}

} // namespace g610
