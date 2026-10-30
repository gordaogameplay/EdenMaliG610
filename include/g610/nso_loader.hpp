// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — carregador de NSO.
//
// Um NSO é um ELF64 AArch64 com cabeçalhos extra da Nintendo no fim do ficheiro
// (.text, .rodata, .data, .bss). Não há ELF dinâmico: o guest é linkado
// estaticamente, e o SDK/subsdk são carregados pelo próprio código do jogo
// através de serviços de memória.
//
// Este loader só faz o essencial para execução nativa:
//   - valida o ELF64 e os cabeçalhos META/NSO,
//   - reserva o espaço de endereçamento de 39 bits,
//   - copia os segmentos para a memória do guest.
//
// Não há tradução de código: as instruções são as do binário original.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace g610 {

// Limites do espaço de endereçamento do Switch.
inline constexpr std::uint64_t kGuestAddressMask = 0x0000'7FFF'FFFF'FFFFull; // 39 bits
inline constexpr std::uint64_t kGuestPageSize    = 0x1000;

// Cabeçalho do NSO.
//
// Um NSO NÃO é um ELF. É um formato da Nintendo: um cabeçalho fixo de 0x100
// bytes com três segmentos (text, rodata, data) e, opcionalmente, esses
// segmentos comprimidos com LZ4-HC (formato proprietário do Switch). Quando um
// segmento está comprimido, os primeiros bytes do segmento são um cabeçalho
// LZ4 próprio, não código.
//
// A referência de layout é src/core/loader/nso.h do Eden; este motor
// reimplementa-o de forma independente, com validação explícita dos offsets.
//
//   offset  size  campo
//   0x000   0x04  magic "NSO0"
//   0x004   0x04  version
//   0x008   0x04  reservado
//   0x00C   0x04  flags
//   0x010   0x30  segmentos[3] {offset, location, size, alignment|bss_size}
//   0x040   0x20  build_id
//   0x060   0x0C  segmentos_compressed_size[3]
//   ...
#pragma pack(push, 1)
struct NsoSegment {
    std::uint32_t offset;      // offset no ficheiro
    std::uint32_t location;    // offset na memória do guest
    std::uint32_t size;        // tamanho descomprimido
    std::uint32_t bss_size;    // .bss tamanho (data segment) ou alinhamento
};

struct NsoHeader {
    std::uint32_t magic;       // 0x304F534E "NSO0"
    std::uint32_t version;
    std::uint32_t reserved;
    std::uint32_t flags;
    NsoSegment    segments[3]; // 0 = text, 1 = rodata, 2 = data
    std::uint8_t  build_id[0x20];
    std::uint32_t compressed_size[3];
    std::uint8_t  padding[0x1C];
    // api_info / dynstr / dynsym são relativos à rodata; não são usados na
    // execução nativa, mas existem para validar que a rodata está inteira.
    std::uint32_t api_info_offset;
    std::uint32_t api_info_size;
    std::uint32_t dynstr_offset;
    std::uint32_t dynstr_size;
    std::uint32_t dynsym_offset;
    std::uint32_t dynsym_size;
    std::uint8_t  segment_hashes[3][0x20];
};
static_assert(sizeof(NsoHeader) == 0x100, "NsoHeader tem de ter 0x100 bytes");
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Secção dinâmica do NSO (DT_* entries).
//
// Os NSOs da Switch armazenam uma secção dinâmica semelhante à de um ELF64,
// com entradas de 16 bytes (tag + valor). Não há secção .rela.dyn no formato
// ELF tradicional: as relocations vivem numa tabela apontada por DT_RELA,
// e os símbolos por DT_SYMTAB/DT_STRTAB/DT_HASH.
//
// A localização da secção dinâmica é encontrada através da estrutura MOD0:
//   1. image[4]        = offset para a estrutura MOD0 (tipicamente 8)
//   2. image[mod0+0]   = magic "MOD0" (0x30444F4D)
//   3. image[mod0+8]   = MOD0[1], offset da secção dinâmica (relativo ao base do módulo, menos 8)
//   4. dyn_offset      = 8 + MOD0[1]
//
// Todas as offsets na secção dinâmica e nas tabelas de símbolos são
// relativas ao base do módulo (vaddr 0 da imagem).
// ---------------------------------------------------------------------------

enum DynTag : std::uint64_t {
    DT_NULL          = 0,
    DT_RELA          = 7,
    DT_RELASZ        = 8,
    DT_RELAENT       = 9,
    DT_STRTAB        = 5,
    DT_SYMTAB        = 6,
    DT_HASH          = 4,
    DT_STRSZ         = 10,
    DT_SYMENT        = 11,
    DT_PLTGOT        = 3,
    DT_JMPREL        = 23,
    DT_PLTRELSZ      = 2,
    DT_PLTREL        = 20,
    DT_FLAGS         = 0x6FFFFFF9ull,
    DT_AARCH64_FLAGS = 0x6FFFFEF5ull,
};

// Tipos de relocação (formato NSO, observados no rtld real).
enum RelType : std::uint32_t {
    R_RELATIVE   = 0x403, // *target = module_base + addend
    R_ABS64      = 0x101, // *target = symbol_addr + addend
    R_ABS32      = 0x102, // *(uint32_t*)target = (uint32_t)(symbol_addr + addend)
    R_JUMP_SLOT  = 0x402, // *GOT = symbol_addr - got_page_base (formato NSO)
};

struct DynEntry {
    std::uint64_t tag;
    std::uint64_t val;
};

#pragma pack(push, 1)
struct Elf64Sym {
    std::uint32_t st_name;   // índice na DT_STRTAB
    std::uint8_t  st_info;   // tipo + bind
    std::uint8_t  st_other;  // visibilidade
    std::uint16_t st_shndx;  // secção (0 = indefinido)
    std::uint64_t st_value;  // endereço relativo ao base do módulo
    std::uint64_t st_size;   // tamanho
};
static_assert(sizeof(Elf64Sym) == 0x18, "Elf64Sym tem de ter 24 bytes");

struct Elf64Rela {
    std::uint64_t r_offset;  // offset relativo ao base do módulo
    std::uint64_t r_info;    // símbolo (bits 63:32) | tipo (bits 31:0)
    std::int64_t  r_addend;  // addendo
};
static_assert(sizeof(Elf64Rela) == 0x18, "Elf64Rela tem de ter 24 bytes");
#pragma pack(pop)

// Localiza o offset da secção dinâmica através da estrutura MOD0.
std::uint64_t find_dynamic_offset(const std::vector<std::uint8_t>& image);

// Faz parse das entradas DT_* a partir do offset da secção dinâmica.
std::vector<DynEntry> parse_dynamic(const std::vector<std::uint8_t>& image,
                                    std::uint64_t dyn_offset);

// Constrói um mapa nome->endereço (relativo ao base do módulo) a partir da
// secção dinâmica de um único módulo.
std::unordered_map<std::string, std::uint64_t> build_symbol_map(
    const std::vector<std::uint8_t>& image,
    const std::vector<DynEntry>& dyn);

// Processa todas as relocações de um módulo sobre o seu buffer `image`.
// `module_base` é o endereço onde o módulo será carregado no guest.
// `global_symbols` contém nomes já resolvidos de outros módulos.
// Retorna false e preenche `error` apenas em caso de erro estrutural;
// símbolos não resolvidos são reportados mas não falham a carga.
bool relocate_module(
    std::vector<std::uint8_t>& image,
    std::uint64_t module_base,
    const std::vector<DynEntry>& dyn,
    const std::unordered_map<std::string, std::uint64_t>& global_symbols,
    std::string& error);

// Processa apenas as relocações JUMP_SLOT (tipo 0x402) da tabela DT_JMPREL.
// Escreve em cada entrada da GOT o valor symbol_addr - got_page_base, de modo
// que o stub PLT (ADRP+LDR+ADD+BR) possa reconstituir o endereço absoluto.
// got_page_base = (module_base + DT_PLTGOT_val) & ~0xFFF, isto é, a página
// que o ADRP de cada stub PLT carrega. Símbolos não resolvidos são reportados
// mas não falham a carga.
bool process_jump_slot(
    std::vector<std::uint8_t>& image,
    std::uint64_t module_base,
    const std::vector<DynEntry>& dyn,
    const std::unordered_map<std::string, std::uint64_t>& global_symbols,
    std::string& error);

// Flags do NSO. Bit N = segmento N comprimido. Confirmado em
// src/core/loader/nso.cpp do Eden: `return ((flags >> segment_num) & 1) != 0;`
inline constexpr std::uint32_t kNsoFlagTextCompressed   = 1u << 0;
inline constexpr std::uint32_t kNsoFlagRodataCompressed = 1u << 1;
inline constexpr std::uint32_t kNsoFlagDataCompressed   = 1u << 2;

// Os segmentos comprimidos são blocos LZ4 CRU (sem cabeçalho de ficheiro), do
// mesmo tipo que LZ4_decompress_safe consome. Não existe magic "LZ4N".

// --------------------------------------------------------------- NPDM -------
//
// O cabeçalho Meta do NPDM. O ponto que engana: os flags NÃO são um u32, são
// um único byte (Flags0) em 0x0C, seguido de Flags1 em 0x0D. Assumir um u32
// lê lixo das flags adjacentes e faz um jogo 39-bit parecer 32-bit.
//
//   0x0C  Flags0   bit0 = Is64BitInstruction
//                   bits 1-3 = ProcessAddressSpace
//                              0 = 32-bit
//                              1 = 64-bit/36  (AddressSpace64BitOld)
//                              2 = 32-bit sem reservado
//                              3 = 64-bit/39  (AddressSpace64Bit)  <-- requisito NCE
//                              4 = 64-bit/42
//   0x0D  Flags1   bit0 = shadow stack (23.0.0+)
#pragma pack(push, 1)
struct NpdmMeta {
    char          magic[4];               // 0x00 "META"
    std::uint32_t signature_key_generation;// 0x04
    std::uint32_t reserved0;              // 0x08
    std::uint8_t  flags0;                 // 0x0C
    std::uint8_t  flags1;                 // 0x0D
    std::uint8_t  main_thread_priority;   // 0x0E
    std::uint8_t  main_thread_core;       // 0x0F
    std::uint32_t reserved1;              // 0x10
    std::uint32_t system_resource_size;   // 0x14
    std::uint32_t version;                // 0x18
    std::uint32_t main_thread_stack_size; // 0x1C
    char          name[0x10];             // 0x20
    char          product_code[0x10];     // 0x30
    std::uint8_t  reserved2[0x30];        // 0x40
    std::uint32_t aci_offset;             // 0x70
    std::uint32_t aci_size;               // 0x74
    std::uint32_t acid_offset;            // 0x78
    std::uint32_t acid_size;              // 0x7C
};
static_assert(sizeof(NpdmMeta) == 0x80, "NpdmMeta tem de ter 0x80 bytes");
#pragma pack(pop)

// Bits de Flags0.
inline constexpr std::uint8_t  kNpdmFlagIs64Bit          = 1u << 0;
inline constexpr std::uint8_t  kNpdmAddressSpaceMask     = 0x0E;
inline constexpr std::uint8_t  kNpdmAddressSpaceShift    = 1;
inline constexpr std::uint32_t kNpdmAddressSpace64Bit39  = 3;

// Índices dos segmentos do NSO.
enum NsoSegmentIndex { kNsoText = 0, kNsoRodata = 1, kNsoData = 2 };

// ---------------------------------------------------------------------------
// Imagem carregada, pronta a correr.
// ---------------------------------------------------------------------------
struct LoadedModule {
    std::string       path;
    std::uint64_t     base        = 0; // endereço base no guest (39-bit)

    // Um segmento carregado, em memória do guest.
    struct Segment {
        std::uint64_t vaddr = 0;
        std::uint64_t size  = 0; // descomprimido
        bool          compressed = false;
    };
    Segment segments[3];
    std::uint64_t bss_size  = 0;
    std::uint64_t entry_point = 0; // == base + segments[text].vaddr

    std::uint8_t build_id[0x20] = {};
    std::uint32_t version = 0;
    std::string   module_name; // vem do NPDM

    bool has_64_bit = false;
    std::uint32_t address_space_type = 0;
    std::uint8_t  main_thread_priority = 0;
    std::uint8_t  main_thread_core = 0;
    std::uint32_t stack_size = 0;

    // `SystemResourceSize` do NPDM meta (offset 0x14). O kernel reserva
    // esta memória para as suas estruturas internas conforme o processo
    // precisa dela. É o que `GetInfo(SystemResourceSizeTotal)` devolve.
    std::uint32_t system_resource_size = 0;

    // Imagem do módulo lays out no ESPAÇO VIRTUAL, não concatenada.
    //
    // Os vaddr dos segmentos são absolutos dentro do módulo e NÃO são
    // contíguos: no `main` do Pokémon Shield medido há 3328 bytes de gap
    // entre text e rodata, e 784 entre rodata e data. Uma concatenação
    // text|rodata|data colocaria a rodata 3328 bytes antes do sítio certo —
    // e é lá que vivem as tabelas de símbolos e de relocações, que o rtld
    // percorre para resolver imports. O jogo apareceria como "arrancou mas
    // rebenta a seguir", sem nenhum erro visível.
    //
    // Portanto: `image` espelha o espaço virtual do módulo desde vaddr 0, com
    // os gaps a zero. Copiar esta imagem para `base` coloca cada segmento
    // exactamente onde o guest o espera.
    std::vector<std::uint8_t> image;

    // Offset de cada segmento dentro de `image`. É o mesmo que o vaddr, e
    // existe para o código não fazer a conta de sintaxe repetidamente.
    std::uint64_t seg_offset[3] = {0, 0, 0};

    // O cabeçalho NSO tal como estava no ficheiro.
    //
    // Guardado — e não relido — porque é daqui que sai a metadata que o rtld
    // precisa para encontrar as tabelas de símbolos (`api_info`, `dynstr`,
    // `dynsym`). Sem estasInfo o rtld não sabe o que percorrer, e o sintoma
    // é "Unresolved symbol" para todos os símbolos do módulo.
    std::vector<std::uint8_t> nso_header_bytes;

    // Acesso ao cabeçalho, ou nullptr se o módulo foi carregado sem ele.
    const NsoHeader* nso_header() const {
        if (nso_header_bytes.size() < sizeof(NsoHeader))
            return nullptr;
        return reinterpret_cast<const NsoHeader*>(nso_header_bytes.data());
    }

    std::uint64_t text_size()   const { return segments[kNsoText].size; }
    std::uint64_t rodata_size() const { return segments[kNsoRodata].size; }
    std::uint64_t data_size()   const { return segments[kNsoData].size; }
    std::uint64_t text_vaddr()  const { return segments[kNsoText].vaddr; }

    // Tamanho da imagem = maior fim de segmento, NÃO a soma dos tamanhos.
    // A diferença é exactamente o espaço occupied pelos gaps, que têm de
    // existir em `image` para os segmentos caírem nos vaddr certos.
    std::uint64_t image_size() const;

    // Um segmento (ou o bss) está contido na imagem?
    bool segment_inside(int index) const {
        if (index < 0 || index > 2)
            return false;
        const std::uint64_t off = seg_offset[index];
        const std::uint64_t sz = segments[index].size;
        return off + sz <= image.size();
    }
};

// ---------------------------------------------------------------------------
// Carregador.
// ---------------------------------------------------------------------------
class NsoLoader {
public:
    // Carrega um NSO (ExeFS `main`). Descomprime os segmentos LZ4 se preciso.
    // Não executa nada — só prepara a imagem para o espaço do guest.
    bool load(const std::string& path, LoadedModule& out, std::string& error);

    // Lê e valida o NPDM associado. O address_space_type é o campo que decide
    // entre execução nativa e JIT: 3 = Is39Bit, requisito do NCE.
    bool load_npdm(const std::string& path, LoadedModule& out, std::string& error);

    // Localiza um NPDM ao lado do NSO ou na pasta pai.
    static std::string find_npdm(const std::string& nso_path);

    // Base de endereçamento por omissão para o espaço de 39 bits.
    static std::uint64_t default_base(std::uint32_t address_space_type);

    // Descomprime um segmento LZ4-HC do Switch. `compressed_size` é o tamanho
    // do bloco comprimido (sem o cabeçalho), `decompressed_size` o esperado.
    // A descompressão LZ4 está em src/nso_loader.cpp; se falhar, devolve false
    // em vez de devolver lixo — um .text corrompido executaria lixo no A76.
    static bool decompress_lz4(const std::uint8_t* src, std::size_t src_size,
                               std::uint8_t* dst, std::size_t dst_size,
                               std::string& error);
};

} // namespace g610
