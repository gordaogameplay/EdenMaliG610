// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// Sonda: o que um NSO real diz sobre o seu próprio layout?
//
// O carregador precisa de três factos que ainda não estão medidos:
//
//   1. Os segmentos têm vaddr ABSOLUTOS (relativos ao base) ou offsets que o
//      carregador tem de somar? Determina se o `image` concatenado pode ser
//      copiado de uma vez ou se cada segmento precisa do seu destino.
//   2. Qual é o maior endereço virtual que o guest chega a usar? Numa
//      execução nativa o endereço é real — se o jogo usar endereços acima de
//      qualquer região que o host possa mapear, o NCE não é viável e é
//      melhor sabê-lo agora.
//   3. A base 0x7fff00000 que o carregador escolhe bate certo com o que o
//      Eden faz (0x80000000)? São ~2 GiB de diferença, e o Eden tem um
//      comentário sobre porquê (programas que dependem de páginas ASLR antes
//      do entry point).
//
// Esta sonda responde lendo os cabeçalhos e os vaddr dos segmentos. Não
// executa nada — é leitura de estrutura, e é o mais barato de corrigir se
// estiver errado.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>

namespace fs = std::filesystem;

// Espelha o cabeçalho do NSO. Duplicado aqui de propósito: a sonda é
// independente do carregador, para que um erro no carregador não se esconda
// atrás de uma estrutura partilhada.
#pragma pack(push, 1)
struct Seg { std::uint32_t offset, location, size, bss_size; };
struct Hdr {
    std::uint32_t magic, version, reserved, flags;
    Seg segments[3];
    std::uint8_t  build_id[0x20];
    std::uint32_t compressed_size[3];
    std::uint8_t  padding[0x1C];
};
#pragma pack(pop)

static bool read_all(const fs::path& p, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<std::size_t>(n));
    std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("uso: %s <main.nso> [sdk.nso] [rtld.nso]\n", argv[0]); return 1; }

    for (int i = 1; i < argc; ++i) {
        const fs::path p = argv[i];
        std::vector<std::uint8_t> raw;
        if (!read_all(p, raw)) { std::printf("nao consegui ler %s\n", p.c_str()); continue; }
        if (raw.size() < sizeof(Hdr) || std::memcmp(raw.data(), "NSO0", 4) != 0) {
            std::printf("%s: nao e' um NSO\n", p.filename().c_str()); continue;
        }
        Hdr h{};
        std::memcpy(&h, raw.data(), sizeof(h));

        std::printf("\n══ %s ══ (%zu bytes)\n", p.filename().c_str(), raw.size());
        std::printf("  flags=0x%08x  (T=%d R=%d D=%d)\n", h.flags,
                    (h.flags >> 0) & 1, (h.flags >> 1) & 1, (h.flags >> 2) & 1);

        static const char* nomes[3] = {"text", "rodata", "data"};
        std::uint64_t max_end = 0;
        for (int s = 0; s < 3; ++s) {
            const std::uint64_t sz = h.segments[s].size;
            std::uint64_t end = static_cast<std::uint64_t>(h.segments[s].location) + sz;
            if (end > max_end) max_end = end;
            std::printf("  %-6s vaddr=0x%08x  size=%10u  end=0x%08x  comp_size=%u  align/bss=0x%x\n",
                        nomes[s], h.segments[s].location, h.segments[s].size,
                        static_cast<unsigned>(end), h.compressed_size[s],
                        h.segments[s].bss_size);
        }
        std::printf("  maior endereço de segmento: 0x%llx (%llu bytes)\n",
                    (unsigned long long)max_end, (unsigned long long)max_end);

        // Vaddr absolutos ou relativos? Se location[1] == location[0]+size[0]
        // (aproximadamente, com alinhamento), a imagem é contígua e pode ser
        // copiada de uma vez. Se houver um salto grande, são absolutos.
        const std::uint32_t l0 = h.segments[0].location;
        const std::uint32_t l1 = h.segments[1].location;
        const std::uint32_t l2 = h.segments[2].location;
        std::printf("\n  gaps entre segmentos:\n");
        std::printf("    text->rodata : %d bytes\n", (int)(l1 - (l0 + h.segments[0].size)));
        std::printf("    rodata->data : %d bytes\n", (int)(l2 - (l1 + h.segments[1].size)));
        if (l1 == 0 && l2 == 0)
            std::printf("    => vaddr RELATIVOS ao base (text implica 0)\n");
        else
            std::printf("    => vaddr ABSOLUTOS (ja incluem o base)\n");

        // Onde o entry point tem de cair: base + location[text]?
        std::printf("  entry (base + vaddr[text]) = base + 0x%08x\n", l0);
    }

    std::printf("\n=== referência do Eden ===\n");
    std::printf("  39-bit: code_address = 0x80000000 + aslr_offset\n");
    std::printf("  o nosso default_base(3) = 0x0007FFF00000 = %llu bytes\n",
                (unsigned long long)0x0007FFF00000ull);
    std::printf("  diferença = %llu bytes (~%.1f GiB)\n",
                (unsigned long long)(0x0007FFF00000ull - 0x80000000ull),
                (0x0007FFF00000ull - 0x80000000ull) / (1024.0 * 1024 * 1024));
    return 0;
}