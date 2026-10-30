// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação do carregador de NSO.
#include "g610/nso_loader.hpp"

#include <algorithm>
#include <cstring>
#include <lz4.h>
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
    int written = LZ4_decompress_safe(
        reinterpret_cast<const char*>(src),
        reinterpret_cast<char*>(dst),
        static_cast<int>(src_size),
        static_cast<int>(dst_size));
    if (written < 0) {
        error = "LZ4: falha na descompressão (código " +
                std::to_string(written) + ")";
        return false;
    }
    if (static_cast<std::size_t>(written) != dst_size) {
        std::fprintf(stderr,
            "[DEBUG LZ4] written=%d dst_size=%zu (mismatch!)\n",
            written, dst_size);
    }
    return true;
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
        // DEBUG: verificar primeiros bytes do segmento descomprimido
        if (i == 0 && out.segments[i].compressed) {
            std::fprintf(stderr,
                "[DEBUG NSO] text seg: compressed=1 src_size=%u dst_size=%u "
                "first64=",
                (unsigned int)on_disk[i], (unsigned int)seg.size);
            for (int j = 0; j < 64 && j < (int)seg.size; ++j)
                std::fprintf(stderr, "%02x", (unsigned int)src[j]);
            std::fprintf(stderr, "\n");
        }
        std::memcpy(out.image.data() + off, src, static_cast<std::size_t>(seg.size));
        out.seg_offset[i] = off;
        // DEBUG: contar instruções SVC (opcode 0xD4) no texto descomprimido
        if (i == 0 && out.segments[i].compressed) {
            std::size_t svc_count = 0;
            std::size_t d4_count = 0;
            for (std::size_t j = 0; j < seg.size; ++j) {
                if (src[j] == 0xD4) ++d4_count;
                if (j + 4 <= seg.size) {
                    std::uint32_t insn;
                    std::memcpy(&insn, src + j, 4);
                    if ((insn & 0xFFE00000u) == 0xD4200000u)
                        ++svc_count;
                }
            }
            std::fprintf(stderr,
                "[DEBUG NSO] text seg %d: src_size=%u dst_size=%zu d4_bytes=%zu svc_count=%zu\n",
                i, (unsigned int)on_disk[i], seg.size, d4_count, svc_count);
        }
    }
    // O bss já está a zero do assign().

    // Base de endereçamento e entry point. O entry point do Switch é o início
    // do segmento de texto (não há outro símbolo de entrada).
    // Guardar o cabeçalho: é a fonte da metadata que o rtld usa para
    // localizar as tabelas de símbolos.
    out.nso_header_bytes.assign(reinterpret_cast<const std::uint8_t*>(&h),
                                reinterpret_cast<const std::uint8_t*>(&h) +
                                    sizeof(h));

    // **Ajustar offsets de dynstr/dynsym no cabeçalho NSO.**
    // O cabeçalho NSO armazena dynstr_offset/dynsym_offset relativos ao
    // segmento de rodata, não ao base do módulo. Quando o rtld lê o cabeçalho
    // e soma ao base do módulo, obtém um endereço dentro do segmento de texto
    // (código), em vez de dentro do rodata (dados). Corrigir os offsets para
    // serem relativos ao base do módulo, para que o rtld consiga encontrar as
    // tabelas de símbolos e resolver imports.
    if (out.nso_header_bytes.size() >= sizeof(NsoHeader)) {
        auto* hdr = reinterpret_cast<NsoHeader*>(out.nso_header_bytes.data());
        const std::uint64_t rodata_vaddr = out.segments[kNsoRodata].vaddr;
        const std::uint64_t text_vaddr = out.segments[kNsoText].vaddr;

        // dynstr_offset está relativo ao início do segmento rodata.
        // Converter para offset relativo ao base do módulo:
        //   rodata_vaddr + dynstr_offset - text_vaddr
        std::uint32_t dynstr_offset = hdr->dynstr_offset;
        std::uint32_t dynsym_offset = hdr->dynsym_offset;
        std::uint64_t dynstr_va = rodata_vaddr + dynstr_offset - text_vaddr;
        std::uint64_t dynsym_va = rodata_vaddr + dynsym_offset - text_vaddr;
        hdr->dynstr_offset = static_cast<std::uint32_t>(dynstr_va);
        hdr->dynsym_offset = static_cast<std::uint32_t>(dynsym_va);
    }

    out.base = default_base(out.address_space_type);
    out.entry_point = out.base + out.segments[kNsoText].vaddr;

    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Secção dinâmica e relocações (processamento ELF64-style sobre NSO).
// ---------------------------------------------------------------------------

namespace {

// ELF64 hash function (DT_HASH).
constexpr std::uint32_t elf_hash(const char* name) {
    std::uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + static_cast<std::uint8_t>(*name++);
        g = h & 0xF0000000u;
        if (g != 0)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// Lê um u32 little-endian de `image` com bounds-check.
std::uint32_t read_u32(const std::vector<std::uint8_t>& image, std::size_t off,
                       std::string& error) {
    if (off + 4 > image.size()) {
        error = "leitura fora da imagem (u32 @ 0x" +
                std::to_string(off) + ")";
        return 0;
    }
    std::uint32_t v;
    std::memcpy(&v, image.data() + off, 4);
    return v;
}

// Lê um u64 little-endian de `image` com bounds-check.
std::uint64_t read_u64(const std::vector<std::uint8_t>& image, std::size_t off,
                       std::string& error) {
    if (off + 8 > image.size()) {
        error = "leitura fora da imagem (u64 @ 0x" +
                std::to_string(off) + ")";
        return 0;
    }
    std::uint64_t v;
    std::memcpy(&v, image.data() + off, 8);
    return v;
}

} // namespace

std::uint64_t find_dynamic_offset(const std::vector<std::uint8_t>& image) {
    if (image.size() < 16)
        return 0;
    // image[4] = offset para a estrutura MOD0
    std::string err;
    std::uint32_t mod0_off = read_u32(image, 4, err);
    if (mod0_off + 12 > image.size())
        return 0;
    // Verificar magic "MOD0"
    std::uint32_t magic = read_u32(image, mod0_off, err);
    if (magic != 0x30444F4Du) // "MOD0"
        return 0;
    // image[mod0 + 4] = MOD0[1] = offset da secção dinâmica (relativo à base do módulo, menos 8)
    std::uint32_t mod0_1 = read_u32(image, mod0_off + 4, err);
    return 8ull + mod0_1;
}

std::vector<DynEntry> parse_dynamic(const std::vector<std::uint8_t>& image,
                                    std::uint64_t dyn_offset) {
    std::vector<DynEntry> entries;
    std::string err;
    for (std::size_t i = 0;; ++i) {
        std::size_t off = static_cast<std::size_t>(dyn_offset) + i * 16;
        if (off + 16 > image.size())
            break;
        DynEntry e;
        std::memcpy(&e.tag, image.data() + off, 8);
        std::memcpy(&e.val, image.data() + off + 8, 8);
        entries.push_back(e);
        if (e.tag == DT_NULL)
            break;
    }
    return entries;
}

std::unordered_map<std::string, std::uint64_t> build_symbol_map(
    const std::vector<std::uint8_t>& image,
    const std::vector<DynEntry>& dyn) {

    std::unordered_map<std::string, std::uint64_t> syms;
    std::string err;

    // Encontrar DT_HASH, DT_STRTAB, DT_SYMTAB, DT_SYMENT
    std::uint64_t hash_va = 0, strtab_va = 0, symtab_va = 0;
    std::uint32_t syment = 0x18;
    for (const auto& e : dyn) {
        switch (e.tag) {
        case DT_HASH:   hash_va = e.val; break;
        case DT_STRTAB: strtab_va = e.val; break;
        case DT_SYMTAB: symtab_va = e.val; break;
        case DT_SYMENT: syment = static_cast<std::uint32_t>(e.val); break;
        default: break;
        }
    }
    if (hash_va == 0 || strtab_va == 0 || symtab_va == 0)
        return syms; // sem dados dinâmicos suficientes

    // Ler nbucket e nchain de DT_HASH
    if (hash_va + 8 > image.size())
        return syms;
    std::uint32_t nbucket = read_u32(image, static_cast<std::size_t>(hash_va), err);
    std::uint32_t nchain  = read_u32(image, static_cast<std::size_t>(hash_va) + 4, err);

    // Ler buckets e chains
    std::size_t buckets_off = static_cast<std::size_t>(hash_va) + 8;
    std::size_t chains_off  = buckets_off + nbucket * 4;

    // Percorrer a tabela de símbolos
    for (std::uint32_t i = 0; i < nchain; ++i) {
        std::size_t sym_off = static_cast<std::size_t>(symtab_va) + i * syment;
        if (sym_off + 0x18 > image.size())
            break;
        Elf64Sym s;
        std::memcpy(&s, image.data() + sym_off, sizeof(s));

        // Símbolo definido (st_shndx != 0)
        if (s.st_shndx == 0 || s.st_name == 0)
            continue;

        // Ler nome da string table
        std::size_t name_off = static_cast<std::size_t>(strtab_va) + s.st_name;
        std::string name;
        for (std::size_t j = name_off; j < image.size(); ++j) {
            char c = static_cast<char>(image[j]);
            if (c == '\0')
                break;
            name.push_back(c);
        }
        if (!name.empty()) {
            // Endereço absoluto relativo ao base do módulo (caller adiciona module_base)
            syms[name] = s.st_value;
        }
    }
    return syms;
}

bool relocate_module(
    std::vector<std::uint8_t>& image,
    std::uint64_t module_base,
    const std::vector<DynEntry>& dyn,
    const std::unordered_map<std::string, std::uint64_t>& global_symbols,
    std::string& error) {

    // Encontrar DT_RELA, DT_RELASZ, DT_RELAENT, DT_JMPREL, DT_PLTRELSZ
    std::uint64_t rela_va = 0, rela_sz = 0, rela_ent = 0;
    std::uint64_t jmprel_va = 0, pltrel_sz = 0;
    std::uint64_t strtab_va = 0, symtab_va = 0;
    std::uint32_t syment = 0x18;
    std::uint64_t got_page_base = 0;

    for (const auto& e : dyn) {
        switch (e.tag) {
        case DT_RELA:     rela_va = e.val; break;
        case DT_RELASZ:   rela_sz = e.val; break;
        case DT_RELAENT:  rela_ent = e.val; break;
        case DT_JMPREL:   jmprel_va = e.val; break;
        case DT_PLTRELSZ: pltrel_sz = e.val; break;
        case DT_STRTAB:   strtab_va = e.val; break;
        case DT_SYMTAB:   symtab_va = e.val; break;
        case DT_SYMENT:   syment = static_cast<std::uint32_t>(e.val); break;
        case DT_PLTGOT:   got_page_base = e.val & ~0xFFFull; break;
        default: break;
        }
    }
    if (rela_ent == 0) rela_ent = 0x18;

    // Helper para resolver um símbolo pelo índice na tabela dinâmica.
    // Devolve o endereço ABSOLUTO (já inclui module_base para símbolos locais).
    auto resolve_symbol = [&](std::uint32_t sym_idx) -> std::uint64_t {
        if (symtab_va == 0 || syment == 0)
            return 0;
        std::size_t sym_off = static_cast<std::size_t>(symtab_va) + sym_idx * syment;
        if (sym_off + 0x18 > image.size())
            return 0;
        Elf64Sym s;
        std::memcpy(&s, image.data() + sym_off, sizeof(s));
        if (s.st_shndx != 0) {
            // Símbolo definido neste módulo: endereço absoluto
            return module_base + s.st_value;
        }
        // Símbolo indefinido: procurar pelo nome nas outras módulos
        if (s.st_name == 0 || strtab_va == 0)
            return 0;
        std::size_t name_off = static_cast<std::size_t>(strtab_va) + s.st_name;
        std::string name;
        for (std::size_t j = name_off; j < image.size(); ++j) {
            char c = static_cast<char>(image[j]);
            if (c == '\0')
                break;
            name.push_back(c);
        }
        if (name.empty())
            return 0;
        auto it = global_symbols.find(name);
        if (it != global_symbols.end())
            return it->second; // Já é endereço absoluto
        return 0; // não resolvido
    };

    // Processa DT_RELA. As entradas DT_JMPREL (PLT/lazy binding) usam um
    // formato de relocação próprio do NSO que requer conhecimento do page-base
    // de cada stub PLT; são deixadas para o resolvedor lazy do RTLD.
    if (rela_va != 0 && rela_sz != 0) {
        std::uint64_t num_relas = rela_sz / rela_ent;
        for (std::uint64_t i = 0; i < num_relas; ++i) {
            std::size_t rel_off = static_cast<std::size_t>(rela_va) + i * rela_ent;
            if (rel_off + 0x18 > image.size())
                break;
            Elf64Rela r;
            std::memcpy(&r, image.data() + rel_off, sizeof(r));

            std::uint32_t r_type = static_cast<std::uint32_t>(r.r_info & 0xFFFFFFFFull);
            std::uint32_t r_sym  = static_cast<std::uint32_t>(r.r_info >> 32);

            if (r.r_offset + 8 > image.size())
                continue;

            switch (r_type) {
            case R_RELATIVE: {
                std::uint64_t val = module_base + r.r_addend;
                std::memcpy(image.data() + r.r_offset, &val, 8);
                break;
            }
            case R_ABS64: {
                std::uint64_t val = resolve_symbol(r_sym);
                if (val == 0)
                    continue;
                val += r.r_addend;
                std::memcpy(image.data() + r.r_offset, &val, 8);
                break;
            }
            case R_ABS32: {
                std::uint64_t val = resolve_symbol(r_sym);
                if (val == 0)
                    continue;
                std::uint32_t v32 = static_cast<std::uint32_t>(val + r.r_addend);
                std::memcpy(image.data() + r.r_offset, &v32, 4);
                break;
            }
            default:
                break;
            }
        }
        std::fprintf(stderr,
            "[reloc] módulo base=0x%llx: %llu relocações DT_RELA processadas\n",
            (unsigned long long)module_base,
            (unsigned long long)num_relas);
    }

    return true;
}

bool relocate_module(LoadedModule& mod,
                     std::uint64_t module_base,
                     const std::unordered_map<std::string, std::uint64_t>& global_symbols,
                     std::string& error) {
    std::uint64_t dyn_off = find_dynamic_offset(mod.image);
    if (dyn_off == 0) {
        error = "não foi possível localizar a secção dinâmica via MOD0";
        return false;
    }
    auto dyn = parse_dynamic(mod.image, dyn_off);
    return relocate_module(mod.image, module_base, dyn, global_symbols, error);
}

bool process_jump_slot(
    std::vector<std::uint8_t>& image,
    std::uint64_t module_base,
    const std::vector<DynEntry>& dyn,
    const std::unordered_map<std::string, std::uint64_t>& global_symbols,
    std::string& error) {

    // Encontrar DT_JMPREL, DT_PLTRELSZ, DT_SYMTAB, DT_STRTAB, DT_SYMENT
    std::uint64_t jmprel_va = 0, pltrel_sz = 0;
    std::uint64_t strtab_va = 0, symtab_va = 0;
    std::uint32_t syment = 0x18;

    for (const auto& e : dyn) {
        switch (e.tag) {
        case DT_JMPREL:   jmprel_va = e.val; break;
        case DT_PLTRELSZ: pltrel_sz = e.val; break;
        case DT_STRTAB:   strtab_va = e.val; break;
        case DT_SYMTAB:   symtab_va = e.val; break;
        case DT_SYMENT:   syment = static_cast<std::uint32_t>(e.val); break;
        default: break;
        }
    }

    if (jmprel_va == 0 || pltrel_sz == 0)
        return true; // sem entradas de PLT

    // Helper para resolver um símbolo pelo índice na tabela dinâmica.
    // Devolve o endereço ABSOLUTO.
    auto resolve_symbol = [&](std::uint32_t sym_idx) -> std::uint64_t {
        if (symtab_va == 0 || syment == 0)
            return 0;
        std::size_t sym_off = static_cast<std::size_t>(symtab_va) + sym_idx * syment;
        if (sym_off + 0x18 > image.size())
            return 0;
        Elf64Sym s;
        std::memcpy(&s, image.data() + sym_off, sizeof(s));
        if (s.st_shndx != 0) {
            return module_base + s.st_value;
        }
        if (s.st_name == 0 || strtab_va == 0)
            return 0;
        std::size_t name_off = static_cast<std::size_t>(strtab_va) + s.st_name;
        std::string name;
        for (std::size_t j = name_off; j < image.size(); ++j) {
            char c = static_cast<char>(image[j]);
            if (c == '\0')
                break;
            name.push_back(c);
        }
        if (name.empty())
            return 0;
        auto it = global_symbols.find(name);
        if (it != global_symbols.end())
            return it->second;
        return 0;
    };

    std::uint64_t num_entries = pltrel_sz / 0x18;
    std::uint64_t resolved = 0, unresolved = 0;

    for (std::uint64_t i = 0; i < num_entries; ++i) {
        std::size_t rel_off = static_cast<std::size_t>(jmprel_va) + i * 0x18;
        if (rel_off + 0x18 > image.size())
            break;
        Elf64Rela r;
        std::memcpy(&r, image.data() + rel_off, sizeof(r));

        std::uint32_t r_type = static_cast<std::uint32_t>(r.r_info & 0xFFFFFFFFull);
        std::uint32_t r_sym  = static_cast<std::uint32_t>(r.r_info >> 32);

        if (r_type != R_JUMP_SLOT)
            continue;

        if (r.r_offset + 8 > image.size())
            continue;

        std::uint64_t sym_addr = resolve_symbol(r_sym);
        if (sym_addr == 0) {
            std::fprintf(stderr,
                "[jump_slot] símbolo não resolvido (índice=%u) @ módulo+0x%llx\n",
                r_sym, (unsigned long long)r.r_offset);
            ++unresolved;
            continue;
        }

        // GOT[entry] = endereço absoluto do símbolo.
        // O stub PLT carrega *GOT_entry diretamente e faz BR x17 para esse
        // valor (confirmado via objdump: ldr x17, [x16, #offset]; br x17).
        // NÃO subtrair got_page_base — o valor deve ser o endereço absoluto.
        std::memcpy(image.data() + r.r_offset, &sym_addr, 8);
        ++resolved;
    }

    std::fprintf(stderr,
        "[jump_slot] módulo base=0x%llx: %llu resolvidas, %llu não resolvidas\n",
        (unsigned long long)module_base,
        (unsigned long long)resolved,
        (unsigned long long)unresolved);

    error.clear();
    return true;
}

} // namespace g610
