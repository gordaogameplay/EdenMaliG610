// Ferramenta de diagnóstico: descomprime os segmentos de um NSO para .bin,
// para desmontar (objdump) sem tocar no motor. Uso:
//   unpack_nso <nso> <dir_saida>   (escreve <dir_saida>/text.bin, ro.bin, data.bin)
//
// Não entra no build do jogo nem nos testes: o gatilho é o depurador humano.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "g610/nso_loader.hpp"

using g610::NsoHeader;
using g610::NsoLoader;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "uso: unpack_nso <nso> <dir_saida>\n");
        return 2;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "sem ficheiro: %s\n", argv[1]);
        return 1;
    }
    NsoHeader h;
    if (std::fread(&h, sizeof h, 1, f) != 1) {
        std::fprintf(stderr, "cabeçalho curto\n");
        std::fclose(f);
        return 1;
    }
    if (h.magic != 0x304F534Eu) {
        std::fprintf(stderr, "não é NSO0: %08x\n", h.magic);
        std::fclose(f);
        return 1;
    }
    std::printf("flags=%08x text compensado=%u/%u ro=%u/%u data=%u/%u\n",
                h.flags,
                h.segments[0].offset, h.segments[0].size,
                h.segments[1].offset, h.segments[1].size,
                h.segments[2].offset, h.segments[2].size);

    static const char* names[3] = {"text", "ro", "data"};
    for (int i = 0; i < 3; ++i) {
        const std::size_t loaded = h.segments[i].size;
        if (loaded == 0)
            continue;
        std::string buf(loaded, '\0');
        if (std::fseek(f, h.segments[i].offset, SEEK_SET) != 0) {
            std::fprintf(stderr, "seek %s falhou\n", names[i]);
            continue;
        }
        // flags bit i = comprimido (confirmado em nso_loader.hpp)
        if (h.flags & (1u << i)) {
            const std::size_t comp = h.compressed_size[i];
            std::string cbuf(comp, '\0');
            if (std::fread(cbuf.data(), 1, comp, f) != comp) {
                std::fprintf(stderr, "%s: bloco comprimido curto\n", names[i]);
                continue;
            }
            std::string err;
            if (!NsoLoader::decompress_lz4(
                    reinterpret_cast<const std::uint8_t*>(cbuf.data()), comp,
                    reinterpret_cast<std::uint8_t*>(buf.data()), loaded, err)) {
                std::fprintf(stderr, "%s: descompressão falhou: %s\n",
                             names[i], err.c_str());
                continue;
            }
        } else {
            if (std::fread(buf.data(), 1, loaded, f) != loaded) {
                std::fprintf(stderr, "%s: bloco curto\n", names[i]);
                continue;
            }
        }
        std::string out = std::string(argv[2]) + "/" + names[i] + ".bin";
        std::FILE* o = std::fopen(out.c_str(), "wb");
        if (o == nullptr) {
            std::fprintf(stderr, "sem escrita: %s\n", out.c_str());
            continue;
        }
        std::fwrite(buf.data(), 1, loaded, o);
        std::fclose(o);
        std::printf("%s: %zu bytes -> %s\n", names[i], loaded, out.c_str());
    }
    std::fclose(f);
    return 0;
}
