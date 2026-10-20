// EdenMaliG610 — implementação do carregador de NSO.
#include "g610/nso_loader.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace g610 {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kNsoMagic = 0x304F534Eu; // "NSO0"

bool read_whole_file(const std::string& path, std::vector<std::uint8_t>& out,
                     std::string& error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        error = "não foi possível abrir " + path;
        return false;
    }
    const auto sz = f.tellg();
    if (sz < 0) {
        error = "tamanho inválido em " + path;
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    if (sz > 0 && !f.read(reinterpret_cast<char*>(out.data()), sz)) {
        error = "leitura incompleta de " + path;
        return false;
    }
    return true;
}

bool flag_compressed(std::uint32_t flags, int seg) {
    switch (seg) {
    case kNsoText:   return (flags & kNsoFlagTextCompressed) != 0;
    case kNsoRodata: return (flags & kNsoFlagRodataCompressed) != 0;
    case kNsoData:   return (flags & kNsoFlagDataCompressed) != 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// LZ4-HC (bloco cru, sem cabeçalho de ficheiro)
//
// O Switch usa o formato de ficheiro LZ4 padrão, que é um cabeçalho de 15 bytes
// seguido de blocos independentes. Cada bloco tem um inteiro de 32 bits com o
// tamanho comprimido, e opcionalmente o tamanho descomprimido.
//
// Não usamos a biblioteca LZ4 do sistema: o formato do Switch tem um cabeçalho
// de ficheiro próprio ("LZ4N") e a validação tem de ser explícita. Um .text
// mal descomprimido a correr no A76 é um salto para lixo — o erro tem de ser um
// erro, não um crash silencioso.
// ---------------------------------------------------------------------------

std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Um bloco LZ4: sequência de (literal_length, literais, offset, match_length).
bool lz4_decompress_block(const std::uint8_t* src, std::size_t src_size,
                          std::uint8_t* dst, std::size_t dst_size,
                          std::string& error) {
    std::size_t ip = 0, op = 0;

    while (ip < src_size) {
        const std::uint8_t token = src[ip++];

        // Literais
        std::size_t lit_len = token >> 4;
        if (lit_len == 15) {
            for (;;) {
                if (ip >= src_size) { error = "LZ4: literais truncados"; return false; }
                const std::uint8_t b = src[ip++];
                lit_len += b;
                if (b != 255) break;
            }
        }
        if (lit_len > src_size - ip) { error = "LZ4: literais além do fim"; return false; }
        if (lit_len > dst_size - op) { error = "LZ4: literais além do destino"; return false; }
        std::memcpy(dst + op, src + ip, lit_len);
        ip += lit_len;
        op += lit_len;

        // O último bloco termina só com literais.
        if (ip == src_size) break;

        // Offset (2 bytes, little-endian) e comprimento da correspondência
        if (ip + 2 > src_size) { error = "LZ4: offset truncado"; return false; }
        const std::uint16_t off = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(src[ip]) |
            (static_cast<std::uint16_t>(src[ip + 1]) << 8));
        ip += 2;
        // Um offset zero é impossível (apontaria para a posição corrente) e um
        // offset maior do que o já escrito leria memória por inicializar.
        if (off == 0 || off > op) { error = "LZ4: offset inválido"; return false; }

        std::size_t match_len = token & 0x0F;
        if (match_len == 15) {
            for (;;) {
                if (ip >= src_size) { error = "LZ4: match truncado"; return false; }
                const std::uint8_t b = src[ip++];
                match_len += b;
                if (b != 255) break;
            }
        }
        match_len += 4; // o mínimo do LZ4

        // A correspondência pode sobrepor-se com a posição de escrita (o caso
        // "run"), por isso tem de ser byte a byte.
        if (match_len > dst_size - op) { error = "LZ4: match além do destino"; return false; }
        std::size_t ref = op - off;
        for (std::size_t i = 0; i < match_len; ++i)
            dst[op++] = dst[ref++];
    }
    return true;
}

} // namespace

bool NsoLoader::decompress_lz4(const std::uint8_t* src, std::size_t src_size,
                               std::uint8_t* dst, std::size_t dst_size,
                               std::string& error) {
    // O Switch guarda o .text/.rodata/.data comprimidos como BLOCO LZ4 CRU —
    // a mesma coisa que LZ4_decompress_safe consome. Não existe cabeçalho de
    // ficheiro LZ4 ("LZ4N"), nem magic, nem campo de tamanho: o primeiro byte
    // é já o token da sequência LZ4.
    //
    // Confirmado em src/common/lz4_compression.cpp do Eden, que envolve
    // LZ4_decompress_safe directamente, sem parsear cabeçalho nenhum.
    if (src_size == 0) {
        error = "LZ4: bloco comprimido vazio";
        return false;
    }
    if (dst_size == 0)
        return true; // segmento vazio, nada a descomprimir
    return lz4_decompress_block(src, src_size, dst, dst_size, error);
}

// ---------------------------------------------------------------------------

std::string NsoLoader::find_npdm(const std::string& nso_path) {
    std::error_code ec;
    const fs::path p(nso_path);

    const fs::path side = p.parent_path() / "main.npdm";
    if (fs::exists(side, ec))
        return side.string();

    const fs::path up = p.parent_path() / (p.filename().string() + ".npdm");
    if (fs::exists(up, ec))
        return up.string();

    return {};
}

std::uint64_t NsoLoader::default_base(std::uint32_t address_space_type) {
    // As bases vêm do Eden (src/core/hle/kernel/k_process.cpp), não de um
    // palpite. Cada uma é o code_address que o kernel do Switch dá ao processo
    // naquele tipo de espaço de endereçamento.
    //
    //   32-bit : 0x0020'0000
    //   36-bit : 0x0800'0000
    //   39-bit : 0x8000'0000
    //
    // NOTA sobre o 39-bit: o Eden carrega a ~2 GiB, e não no topo dos 39 bits.
    // O comentário dele explica porquê — o ASLR de 39-bit começa em
    // 0x8000'0000 com ~512 GiB, e alguns programas (Skyline é o exemplo que
    // ele dá) dependem indevidamente de existirem páginas ASLR ANTES do entry
    // point. Carregar demasiado alto deixa esse buraco para trás.
    //
    // Verificado no Orange Pi 5 Pro: 0x8000'0000 é mapeável pelo host
    // (MAP_FIXED_NOREPLACE tem sucesso), por isso a escolha é do guest e não
    // uma restrição nossa.
    switch (address_space_type) {
    case 0: return 0x0000'0000'0020'0000ull; // 32-bit
    case 1: return 0x0000'0000'0800'0000ull; // 36-bit
    case 2: return 0x0000'0000'0020'0000ull; // 32-bit sem alias
    case 3: return 0x0000'0000'8000'0000ull; // 39-bit (AddressSpace64Bit)
    default: return 0x0000'0000'8000'0000ull;
    }
}

bool NsoLoader::load_npdm(const std::string& path, LoadedModule& out,
                         std::string& error) {
    std::vector<std::uint8_t> raw;
    if (!read_whole_file(path, raw, error))
        return false;
    if (raw.size() < sizeof(NpdmMeta)) {
        error = "NPDM demasiado pequeno: " + path;
        return false;
    }

    NpdmMeta h{};
    std::memcpy(&h, raw.data(), sizeof(h));

    if (std::memcmp(h.magic, "META", 4) != 0)
        error = "assinatura NPDM inesperada (esperado META)";

    out.has_64_bit = (h.flags0 & kNpdmFlagIs64Bit) != 0;
    out.address_space_type =
        (h.flags0 & kNpdmAddressSpaceMask) >> kNpdmAddressSpaceShift;
    out.main_thread_priority = h.main_thread_priority;
    out.system_resource_size = h.system_resource_size;
    out.main_thread_core     = h.main_thread_core;
    out.stack_size           = h.main_thread_stack_size;

    // O nome no NPDM não é terminado em NUL de forma garantida. Copia-se
    // byte a byte até ao limite do campo ou até ao terminador — o campo tem
    // 0x10 bytes e o nome real é muito mais curto.
    out.module_name.clear();
    for (std::size_t i = 0; i < sizeof(h.name); ++i) {
        const char ch = h.name[i];
        if (ch == '\0')
            break;
        out.module_name.push_back(ch);
    }
    return true;
}

std::uint64_t LoadedModule::image_size() const {
    // O maior fim de segmento (incluindo bss, que segue o data), e NÃO a
    // soma dos tamanhos. A soma seria menor que o espaço virtual: os gaps
    // entre segmentos têm de existir, ou a rodata aterraria no sitio errado.
    std::uint64_t end = 0;
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t e = segments[i].vaddr + segments[i].size;
        if (e > end)
            end = e;
    }
    const std::uint64_t bss_end = segments[kNsoData].vaddr + bss_size;
    if (bss_end > end)
        end = bss_end;
    return end;
}

bool NsoLoader::load(const std::string& path, LoadedModule& out, std::string& error) {
    std::vector<std::uint8_t> raw;
    if (!read_whole_file(path, raw, error))
        return false;

    if (raw.size() < sizeof(NsoHeader)) {
        error = "ficheiro demasiado pequeno para um NSO: " + path;
        return false;
    }

    NsoHeader h{};
    std::memcpy(&h, raw.data(), sizeof(h));

    if (h.magic != kNsoMagic) {
        error = "assinatura NSO inválida (esperado NSO0) em " + path;
        return false;
    }

    out.path    = path;
    out.version = h.version;
    std::memcpy(out.build_id, h.build_id, sizeof(out.build_id));

    // Validação dos segmentos antes de tocar em memória.
    //
    // Atenção ao tamanho a comparar: num segmento comprimido, `size` é o
    // tamanho DESCOMPRIMIDO e pode ser muito maior do que o ficheiro — é
    // normal (o .text do Pokémon Shield são 25 MB descomprimidos num ficheiro
    // de 20 MB). O que tem de caber no ficheiro é o tamanho comprimido, que
    // vem em `compressed_size`. Comparar `size` contra o ficheiro rejeita
    // exactamente os NSO comprimidos, que são a maioria dos títulos.
    const std::uint64_t file_size = raw.size();
    std::uint64_t on_disk[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const auto& s = h.segments[i];
        out.segments[i].vaddr      = s.location;
        out.segments[i].size       = s.size;
        out.segments[i].compressed = flag_compressed(h.flags, i);

        if (s.offset > file_size) {
            error = "segmento " + std::to_string(i) +
                    " começa fora do ficheiro (offset " + std::to_string(s.offset) +
                    " > " + std::to_string(file_size) + ")";
            return false;
        }

        // `compressed_size` é o tamanho em DISCO TOTAL, já com o cabeçalho LZ4
        // incluído. Verificado no ficheiro real do Pokémon Shield:
        //   data_file_off = 20 351 050 + compressed_size = 97 667 = 20 448 717
        // que é exactamente o tamanho do ficheiro. Somar o cabeçalho outra vez
        // rejeita o segmento por 20 bytes.
        const std::uint64_t need =
            out.segments[i].compressed
                ? static_cast<std::uint64_t>(h.compressed_size[i])
                : s.size;
        on_disk[i] = need;

        if (need > file_size - s.offset) {
            error = "segmento " + std::to_string(i) +
                    " truncado: precisa de " + std::to_string(need) +
                    " bytes a partir de " + std::to_string(s.offset) +
                    ", ficheiro tem " + std::to_string(file_size - s.offset);
            return false;
        }
    }
    out.bss_size = h.segments[kNsoData].bss_size;
    out.entry_point = 0; // definido abaixo, após a base

    // Descompressão: cada segmento comprimido é um bloco LZ4 cru de
    // `compressed_size` bytes, que se expande para `size` bytes.
    std::vector<std::uint8_t> decomp[3];
    for (int i = 0; i < 3; ++i) {
        const auto& s = h.segments[i];
        if (s.size == 0)
            continue;

        if (out.segments[i].compressed) {
            if (on_disk[i] == 0) {
                error = "segmento comprimido " + std::to_string(i) + " tem tamanho zero";
                return false;
            }
            decomp[i].resize(s.size);
            if (!decompress_lz4(raw.data() + s.offset,
                               static_cast<std::size_t>(on_disk[i]), decomp[i].data(),
                               s.size, error)) {
                error = "segmento " + std::to_string(i) + ": " + error;
                return false;
            }

        }
    }

    // Monta a imagem no ESPAÇO VIRTUAL do módulo.
    //
    // Cada segmento vai para o seu vaddr, e os gaps ficam a zero. Concatenar
    // seria mais simples e estaria errado: os vaddr NÃO são contíguos (3328
    // bytes de gap entre text e rodata no `main` medido), e a rodata é
    // precisamente o que o rtld percorre para resolver imports.
    const std::uint64_t total = out.image_size();
    if (total == 0 || total > (1ull << 32)) {
        error = "imagem do módulo com tamanho absurdo: " + std::to_string(total);
        return false;
    }
    out.image.assign(static_cast<std::size_t>(total), 0);

    for (int i = 0; i < 3; ++i) {
        const auto& seg = h.segments[i];
        if (seg.size == 0) {
            out.seg_offset[i] = seg.location;
            continue;
        }
        const std::uint64_t off = seg.location;
        if (off + seg.size > total) {
            error = "segmento " + std::to_string(i) + " fora da imagem: vaddr=0x"
                    + std::to_string(off) + " size=" + std::to_string(seg.size);
            return false;
        }
        const std::uint8_t* src = out.segments[i].compressed
                                      ? decomp[i].data()
                                      : raw.data() + seg.offset;
        std::memcpy(out.image.data() + off, src, static_cast<std::size_t>(seg.size));
        out.seg_offset[i] = off;
    }
    // O bss já está a zero do assign().

    // Base de endereçamento e entry point. O entry point do Switch é o início
    // do segmento de texto (não há outro símbolo de entrada).
    // Guardar o cabeçalho: é a fonte da metadata que o rtld usa para
    // localizar as tabelas de símbolos.
    out.nso_header_bytes.assign(reinterpret_cast<const std::uint8_t*>(&h),
                                reinterpret_cast<const std::uint8_t*>(&h) +
                                    sizeof(h));

    out.base = default_base(out.address_space_type);
    out.entry_point = out.base + out.segments[kNsoText].vaddr;

    error.clear();
    return true;
}

} // namespace g610
