// EdenMaliG610 — bloco de argumentos do processo.
#include "g610/nso_arguments.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace g610 {

namespace {

void push_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

} // namespace

NsoArgumentBlock NsoArgumentBlock::build(const std::vector<std::string>& args) {
    NsoArgumentBlock b;
    b.bytes_.reserve(kNsoArgAllocatedSize);

    // Phase 1: reserva o cabeçalho. `actual_size` é corrigido no fim, quando
    // o tamanho real dos argumentos for conhecido.
    push_u32(b.bytes_, kNsoArgAllocatedSize);
    push_u32(b.bytes_, 0); // actual_size, preenchido abaixo
    b.bytes_.resize(sizeof(NsoArgumentHeader), 0);

    // Phase 2: cada argumento, terminado em NUL.
    for (const auto& a : args) {
        b.bytes_.insert(b.bytes_.end(), a.begin(), a.end());
        b.bytes_.push_back(0);
    }
    // NUL final: sem ele, um rtld que leia até ao fim sairia da alocação.
    b.bytes_.push_back(0);

    // Phase 3: escreve o tamanho real. Não deve passar da alocação — o Eden
    // recusa-o implicitamente ao encolher o bloco, e um bloco maior faria o
    // rtld ler memória que não pertence ao processo.
    const std::size_t used = b.bytes_.size() - sizeof(NsoArgumentHeader);
    if (used > kNsoArgAllocatedSize) {
        // Truncar em vez de escrever um tamanho que não cabe: é o mesmo que o
        // Eden faz ao encolher para `arg_data.size()`.
        b.bytes_.resize(kNsoArgAllocatedSize + sizeof(NsoArgumentHeader));
    }
    const std::uint32_t final_size = static_cast<std::uint32_t>(
        b.bytes_.size() - sizeof(NsoArgumentHeader));
    std::memcpy(b.bytes_.data() + 4, &final_size, sizeof(final_size));

    return b;
}

NsoArgumentBlock NsoArgumentBlock::empty() { return build({}); }

std::string NsoArgumentBlock::summary() const {
    std::string out;
    // O cabeçalho tem o allocated_size em 0 e o actual_size em 4.
    std::uint32_t act = 0;
    if (bytes_.size() >= 8)
        std::memcpy(&act, bytes_.data() + 4, sizeof(act));

    out += "argument block: 0x" ;
    {
        char b[32];
        std::snprintf(b, sizeof b, "%zx", bytes_.size());
        out += b;
    }
    out += " bytes, actual_size = 0x";
    {
        char b[32];
        std::snprintf(b, sizeof b, "%x", act);
        out += b;
    }
    out += "\n  args:";
    // Percorre as strings depois do cabeçalho.
    std::size_t p = sizeof(NsoArgumentHeader);
    while (p < bytes_.size() && bytes_[p] != 0) {
        std::string s;
        while (p < bytes_.size() && bytes_[p] != 0) {
            s.push_back(static_cast<char>(bytes_[p++]));
        }
        ++p; // salta o NUL
        out += "\n    " + s;
    }
    return out;
}

} // namespace g610