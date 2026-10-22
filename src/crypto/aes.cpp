// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — a cripto que o motor precisa para ler o RomFS do jogo.
//
// A implementacao usa o OpenSSL (`libcrypto`) **so' para o bloco**: o ECB
// e' o unico modo em que o OpenSSL e' exactamente o que o Switch faz. Os
// dois esquemas com contador (o CTR e o XTS do Switch) sao escritos aqui
// por causa, e por causa do `XTS` ser uma variante.
#include "g610/crypto/aes.hpp"

#include <openssl/evp.h>

#include <cstring>
#include <vector>

namespace g610::crypto {

namespace {

// O *tweak* do XTS: o numero do sector em big-endian nas posicoes 8..15.
//
// O Eden: `MakeAesXtsIv` escreve um `s64_be` no offset 8, e `AddCounter`
// soma o numero do sector como um inteiro de 128 bits big-endian.
std::array<std::uint8_t, 16> TweakDoSector(std::uint64_t sector) {
    std::array<std::uint8_t, 16> tweak{};
    for (int i = 0; i < 8; ++i) {
        tweak[8 + i] = static_cast<std::uint8_t>((sector >> (8 * (7 - i))) & 0xFF);
    }
    return tweak;
}

// Multiplica o tweak por alpha no corpo de GF(2^128) — o XTS.
//
// O byte 15 e' o mais significativo (como no OpenSSL e' no mbedtls).
void MultiplicarPorAlpha(std::array<std::uint8_t, 16>& t) {
    std::uint8_t ultimo = t[15];
    for (int i = 15; i > 0; --i) {
        t[i] = static_cast<std::uint8_t>((t[i] << 1) | (t[i - 1] >> 7));
    }
    t[0] = static_cast<std::uint8_t>(t[0] << 1);
    if (ultimo & 0x80) {
        t[0] ^= 0x87;
    }
}

} // namespace
Bloco::Bloco(const Chave128& chave) {
    // **Dois contextos.** O `EVP_CIPHER_CTX` guarda a chave da direccao com
    // que foi inicializado: um contexto inicializado para cifrar e'
    // inicializado de novo (sem chave) para decifrar fica **sem chave de
    // decifracao** — e devolve lixo que parece texto.
    //
    // O Eden tambem tem os dois (`crypto/aes_util.cpp`: `encryption_context`
    // e `decryption_context`).
    EVP_CIPHER_CTX* cifra = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX* decifra = EVP_CIPHER_CTX_new();
    if (cifra == nullptr || decifra == nullptr) {
        if (cifra != nullptr) EVP_CIPHER_CTX_free(cifra);
        if (decifra != nullptr) EVP_CIPHER_CTX_free(decifra);
        return;
    }
    if (EVP_EncryptInit_ex(cifra, EVP_aes_128_ecb(), nullptr, chave.data(), nullptr) != 1 ||
        EVP_CIPHER_CTX_set_padding(cifra, 0) != 1 ||
        EVP_DecryptInit_ex(decifra, EVP_aes_128_ecb(), nullptr, chave.data(), nullptr) != 1 ||
        EVP_CIPHER_CTX_set_padding(decifra, 0) != 1) {
        EVP_CIPHER_CTX_free(cifra);
        EVP_CIPHER_CTX_free(decifra);
        return;
    }
    ctx_cifra_ = cifra;
    ctx_decifra_ = decifra;
}

Bloco::~Bloco() {
    if (ctx_cifra_ != nullptr) {
        EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX*>(ctx_cifra_));
    }
    if (ctx_decifra_ != nullptr) {
        EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX*>(ctx_decifra_));
    }
}

void Bloco::Cifrar(std::span<const std::uint8_t> entrada, std::span<std::uint8_t> saida) {
    if (ctx_cifra_ == nullptr || entrada.size() % 16 != 0 || saida.size() < entrada.size()) {
        return;
    }
    EVP_CIPHER_CTX* ctx = static_cast<EVP_CIPHER_CTX*>(ctx_cifra_);
    int out = 0;
    EVP_EncryptUpdate(ctx, saida.data(), &out, entrada.data(), static_cast<int>(entrada.size()));
}

void Bloco::Decifrar(std::span<const std::uint8_t> entrada, std::span<std::uint8_t> saida) {
    if (ctx_decifra_ == nullptr || entrada.size() % 16 != 0 || saida.size() < entrada.size()) {
        return;
    }
    EVP_CIPHER_CTX* ctx = static_cast<EVP_CIPHER_CTX*>(ctx_decifra_);
    int out = 0;
    EVP_DecryptUpdate(ctx, saida.data(), &out, entrada.data(), static_cast<int>(entrada.size()));
}

Ctr::Ctr(const Chave128& chave) : bloco_{chave} {}

void Ctr::Iv(std::span<const std::uint8_t> iv) {
    if (iv.size() != 16) {
        return;
    }
    std::memcpy(contador_.data(), iv.data(), 16);
}

void Ctr::Decifrar(std::span<const std::uint8_t> entrada, std::span<std::uint8_t> saida) {
    const std::size_t n = entrada.size();
    if (n == 0) {
        return;
    }
    // O fluxo de chaves: um contador por bloco, em big-endian de 128 bits.
    // O bloco e' cifrado **todo de uma vez** com o ECB — que e'
    // independente por bloco.
    //
    // **O ultimo bloco pode ser parcial** — e tem de ser cifrado na mesma:
    // o keystream de um bloco comeca' no principio do bloco, e o que
    // interessa sao os primeiros bytes. Sem isto, os ultimos bytes de uma
    // leitura de tamanho nao multiplo de 16 ficavam *cifrados* — e o guest
    // lia-os como dados, sem eu dar por isso.
    const std::size_t total = ((n + 15) / 16) * 16;
    std::vector<std::uint8_t> fluxo(total);
    auto contador = contador_;
    for (std::size_t i = 0; i < total; i += 16) {
        std::memcpy(fluxo.data() + i, contador.data(), 16);
        for (int k = 15; k >= 0; --k) {
            if (++contador[k] != 0) {
                break;
            }
        }
    }
    bloco_.Cifrar(std::span<const std::uint8_t>(fluxo.data(), total),
                  std::span<std::uint8_t>(fluxo.data(), total));
    for (std::size_t i = 0; i < n; ++i) {
        saida[i] = static_cast<std::uint8_t>(entrada[i] ^ fluxo[i]);
    }
}

void XtsDecifrar(Bloco& k1, Bloco& k2, std::span<const std::uint8_t> entrada,
                 std::span<std::uint8_t> saida, std::uint64_t sector_inicial) {
    std::size_t feito = 0;
    std::uint64_t sector = sector_inicial;
    while (feito < entrada.size()) {
        const std::size_t n = std::min(kSectorXts, entrada.size() - feito);
        // O tweak do sector: `E_k2(tweak_do_sector)`.
        std::array<std::uint8_t, 16> tweak{};
        const auto entrada_tweak = TweakDoSector(sector);
        k2.Cifrar(std::span<const std::uint8_t>(entrada_tweak), std::span<std::uint8_t>(tweak));

        for (std::size_t i = 0; i + 16 <= n; i += 16) {
            std::array<std::uint8_t, 16> pp{};
            for (std::size_t k = 0; k < 16; ++k) {
                pp[k] = static_cast<std::uint8_t>(entrada[feito + i + k] ^ tweak[k]);
            }
            std::array<std::uint8_t, 16> claro{};
            k1.Decifrar(std::span<const std::uint8_t>(pp), std::span<std::uint8_t>(claro));
            for (std::size_t k = 0; k < 16; ++k) {
                saida[feito + i + k] = static_cast<std::uint8_t>(claro[k] ^ tweak[k]);
            }
            MultiplicarPorAlpha(tweak);
        }
        // A cauda (bytes que nao fecham o bloco) fica como esta.
        const std::size_t cheios = (n / 16) * 16;
        for (std::size_t i = cheios; i < n; ++i) {
            saida[feito + i] = entrada[feito + i];
        }
        feito += n;
        ++sector;
    }
}

} // namespace g610::crypto
