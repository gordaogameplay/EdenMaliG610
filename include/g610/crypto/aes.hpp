// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — a cripto que o motor precisa para ler o RomFS do jogo.
//
// ## O que é
//
// O jogo vive num NCA (um content archive) whose the header and the data
// are encrypted. To read them, three primitives are needed:
//
//   * **AES-ECB** — to decrypt the *key area* of the NCA header;
//   * **AES-XTS** — to decrypt the NCA header itself (the "standard
//     crypto" of the Switch);
//   * **AES-CTR** — to decrypt the section that holds the RomFS.
//
// ## De onde vem a forma
//
// The semantics are the Switch's, and the reference is the Eden
// (`core/crypto/aes_util.cpp`, `core/file_sys/fssystem/fssystem_aes_xts_storage.cpp`,
// `core/file_sys/fssystem/fssystem_aes_ctr_storage.cpp`). The implementation
// is ours: OpenSSL's block cipher underneath, and the two counter schemes
// written out explicitly — because they are **not** the standard ones and
// getting them wrong produces plausible-looking garbage.
//
// ## Os dois esquemas, e porque nao sao os de serie
//
// **XTS.** O Eden's XTS does not use a single data unit: it re-derives the
// *tweak* of each 0x200-byte sector from the sector number
// (`AesXtsStorage::Read` calls `SetIV` for each sector, with the IV being
// the sector number in big-endian at bytes 8..15). OpenSSL's XTS, on the
// other hand, keeps one tweak for the whole buffer and multiplies it by
// alpha as it goes. **They are not the same thing**, so the tweak is
// computed here, over the block cipher, exactly as the Eden does.
//
// **CTR.** The counter is 128 bits, big-endian, and the first 8 bytes are
// the "upper" IV that comes from the section header
// (`AesCtrStorage::MakeIv`). mbedtls and OpenSSL both increment the whole
// 16-byte counter as a big-endian number, so the two agree — but writing it
// out keeps the "why" visible.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace g610::crypto {

using Chave128 = std::array<std::uint8_t, 16>;

// O tamanho do sector do XTS do Switch — o Eden: `NcaHeader::XtsBlockSize`.
inline constexpr std::size_t kSectorXts = 0x200;

// AES-128 em modo ECB: o bloco de 16 bytes, e nada mais.
//
// E' o que o XTS e' feito de: `T = E_k2(tweak)` e `P = D_k1(C ^ T)`.
class Bloco {
public:
    explicit Bloco(const Chave128& chave);
    ~Bloco();
    Bloco(const Bloco&) = delete;
    Bloco& operator=(const Bloco&) = delete;

    // Cifra (ou decifra) um multiplo de 16 bytes. O ECB e' independente
    // por bloco, por isso uma so chamada para o buffer inteiro.
    void Cifrar(std::span<const std::uint8_t> entrada, std::span<std::uint8_t> saida);
    void Decifrar(std::span<const std::uint8_t> entrada, std::span<std::uint8_t> saida);

private:
    void* ctx_cifra_ = nullptr;   // EVP_CIPHER_CTX*
    void* ctx_decifra_ = nullptr; // EVP_CIPHER_CTX*
};

// AES-CTR: o contador e' de 128 bits, big-endian.
//
// `contador[0..8]` = o "upper" (vem do cabecalho da seccao), e
// `contador[8..16]` = o numero do bloco. O Eden: `AesCtrStorage::MakeIv`.
void CtrDecifrar(Bloco& bloco, std::span<const std::uint8_t> entrada,
                 std::span<std::uint8_t> saida, std::array<std::uint8_t, 16> contador);

// AES-XTS, **a variante do Switch**.
//
// O *tweak* do sector `s` e' o numero do sector em big-endian nas
// posicoes 8..15 (o resto zero), e dentro do sector o tweak e'
// multiplicado por alpha a cada bloco de 16 bytes.
void XtsDecifrar(Bloco& k1, Bloco& k2, std::span<const std::uint8_t> entrada,
                 std::span<std::uint8_t> saida, std::uint64_t sector_inicial);

} // namespace g610::crypto
