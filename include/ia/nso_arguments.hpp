// EdenMaliIA — bloco de argumentos do processo (argc/argv/environ).
//
// ---------------------------------------------------------------------------
// O QUE É ISTO
// ---------------------------------------------------------------------------
// O rtld do Switch não começa a correr em C puro. Ele começa a correr a
// partir de um estado que o kernel do Switch preparou, e esse estado inclui
// um bloco de dados no fim do segmento de dados do rtld, com:
//
//     struct NSOArgumentHeader {
//         u32 allocated_size;   // 0x9000 — o tamanho reservado
//         u32 actual_size;      // o tamanho realmente usado
//         ... padding até 0x20 ...
//     };
//
// seguido dos próprios argumentos.
//
// FONTE: src/core/loader/nso.h:65 e nso.cpp:119-127 do Eden:
//
//     constexpr u32 NSO_ARGUMENT_DATA_ALLOCATION_SIZE = 0x9000;
//     NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, size, {}};
//     memcpy(..., end_offset, &args_header, sizeof(args_header));
//     memcpy(..., end_offset + sizeof(args_header), arg_data.data(), size);
//
// ---------------------------------------------------------------------------
// PORQUÊ SEM ISTO O rtld NÃO ARRANCA
// ---------------------------------------------------------------------------
// Medido. Sem o bloco, o rtld corre, levanta `svc #ReturnFromException` (0x28)
// logo no início, e os registos x18/x19 contêm TEXTO ASCII lido da stack
// ("80 rec=a", "aaae1899").
//
// O rtld lê essas palavras como se fossem a stack canary, a verificação falha, e
// o código de recuperação chama `svc #0x28`. O sintoma — um SVC cujo x0 não é
// um endereço de 39 bits — não aponta de forma nenhuma para "falta o bloco de
// argumentos".
//
// Este ficheiro existe porque esse bug custou mais tempo a encontrar do que
// qualquer outro: foi o único em que o gdb foi indispensável.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ia {

// O cabeçalho tem 0x20 bytes: dois u32 e 0x18 de padding. Confirmado por
// static_assert no Eden (nso.h:72).
#pragma pack(push, 1)
struct NsoArgumentHeader {
    std::uint32_t allocated_size;  // 0x9000
    std::uint32_t actual_size;     // bytes usados pelos argumentos
    std::uint8_t  padding[0x18];
};
#pragma pack(pop)
static_assert(sizeof(NsoArgumentHeader) == 0x20,
              "NsoArgumentHeader tem de ter 0x20 bytes");

// O Eden reserva sempre 0x9000, mesmo quando não há argumentos. É o tamanho
// que o rtld espera encontrar, e por isso não é negociável: um bloco menor
// seria lido para lá da memória escrita.
inline constexpr std::uint32_t kNsoArgAllocatedSize = 0x9000;

// O bloco de argumentos, pronto a copiar para o fim da `.data` do rtld.
class NsoArgumentBlock {
public:
    // Constrói o bloco a partir de uma lista de argumentos (o primeiro é
    // normalmente o caminho do executável).
    //
    // O formato dos argumentos é o do Switch: cada um é uma string terminada em
    // NUL, e o bloco termina com um NUL extra. Não é argc/argv no formato
    // ELF — é uma lista plana de strings, e o rtld é que a interpreta.
    static NsoArgumentBlock build(const std::vector<std::string>& args);

    // Bloco vazio: só o cabeçalho, zero argumentos. É o que um jogo sem
    // opções de linha de comando recebe.
    static NsoArgumentBlock empty();

    const std::uint8_t* data() const noexcept { return bytes_.data(); }
    std::size_t size() const noexcept { return bytes_.size(); }

    std::uint32_t actual_size() const noexcept {
        return static_cast<std::uint32_t>(bytes_.size() -
                                          sizeof(NsoArgumentHeader));
    }

    // Para diagnóstico: as strings que vão ser entregues ao rtld.
    std::string summary() const;

private:
    std::vector<std::uint8_t> bytes_;
};

} // namespace ia