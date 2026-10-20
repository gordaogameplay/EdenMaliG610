// A memória do guest, com o nome e o namespace que o Eden usa.
//
// Ver `vendor/eden/core/hle/kernel/k_process_bridge.h` para o resto da ponte.
//
// ## O que substitui
//
// `core/memory.h` e `core/memory.cpp` do Eden.
//
// ## Porquê
//
// `HLERequestContext` tem um membro do tipo `Core::Memory::Memory&`
// (`hle_ipc.h:427`), e `hle_ipc.cpp` chama-lhe `ReadBlock`, `WriteBlock`,
// `ReadBlockUnsafe` e `GetSpan`. **O nome e o namespace não são negociáveis**:
// são eles que o `hle_ipc.h` do Eden escreve, e mudá-los seria reescrever o
// framework de IPC em vez de o usar.
//
// Vendorizar o `memory.h` do Eden resolveria a assinatura — e arrastaria o
// `GuestMemory` do Eden, o VMA, as traduções de página e a gestão de memória
// dinâmica. Ou seja, o emulador, exactamente como no `KProcess`.
//
// ## A forma do Eden
//
//     bool ReadBlock(ProcessAddress src, void* dst, size_t size);
//     bool ReadBlockUnsafe(ProcessAddress src, void* dst, size_t size);
//     const u8* GetSpan(const VAddr src, const size_t size) const;
//     bool WriteBlock(ProcessAddress dest, const void* src, size_t size);
//
// As quatro estão em `core/memory.h:353-396`, e são as únicas que o código
// vendorizado chama — medido, não suposto.
//
// ## O que muda de comportamento
//
// **`GetSpan` devolve sempre `nullptr`.**
//
// No Eden, `GetSpan` dá um ponteiro directo para a página mapeada, e é uma
// optimização: evita a cópia. O `guest_memory.h` do Eden está escrito para
// isso — `TrySetSpan()` (linha 160-167) devolve `false` quando o `GetSpan`
// dá `nullptr`, e o código cai para a cópia via `ReadBlock`.
//
// Portanto `nullptr` não é um caso de erro: é um caso previsto e com
// tratamento. A consequência é que os pedidos IPC passam a ser copiados em
// vez de acedidos no sítio, o que numa message buffer de alguns KiB é
// irrelevante — e que se mede na telemetria se algum dia chegar a doer.
//
// **Não devolvemos um ponteiro meio verdadeiro.** A nossa reserva é um
// `mmap` único e plano; para dar um ponteiro do host a partir de um endereço
// do guest seria preciso saber qual é a base do mapeamento, e essa base é um
// detalhe do `GuestMemory` nosso. Devolver `nullptr` é dizer a verdade sobre o
// que sabemos; devolver um ponteiro calculado à pressa seria o contrário.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "g610/guest_memory.hpp"
#include "vendor/eden/common/common_types.h"   // VAddr, u8
#include "vendor/eden/common/typed_address.h"  // Common::ProcessAddress

// `GuestMemory` e `GuestMemoryFlags` sao do Eden (`core/guest_memory.h`) e
// ficam como estao. Sao genericos: so precisam que o `Memory` que lhes passa
// tenha `ReadBlock`, `WriteBlock` e `GetSpan` -- que e' precisamente o que a
// classe abaixo implementa.
//
// Vendoriza-los nao era preciso e nao e' COMPENSAVEL: `guest_memory.h` nao
// inclui `core/memory.h`, e por isso trazi-lo nao arrasta a classe `Memory` do
// Eden. E' o caso em que o fecho transitivo ajuda em vez de atrapalhar.
#include "vendor/eden/core/guest_memory.h"

namespace Core {

class System;

namespace Memory {

class Memory {
public:
    // **Construtor nosso, e não `explicit Memory(System&)`** como no Eden
    // (`memory.h:61`). O Eden recebe o `System` e vai buscar a page table;
    // nós não temos page table — a reserva é nossa e está no `GuestMemory`.
    //
    // A assinatura muda, e é o único ponto em que a ponte não bate com o
    // Eden. Compila porque ninguém do lado vendorizado constrói esta classe:
    // quem a constrói é o `KProcess` da ponte.
    explicit Memory(::g610::GuestMemory& memoria) : memoria_(memoria) {}

    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = delete;
    Memory& operator=(Memory&&) = delete;

    ~Memory() = default;

    // ── o que o código vendorizado chama ─────────────────────────────────
    //
    // `ProcessAddress` é um `TypedAddress<bool, ProcessAddressTag>`. Tem
    // construtor implícito de `uint64_t` (`typed_address.h:22`) — por isso
    // `memory.WriteBlock(thread->GetTlsAddress(), ...)` compila como o Eden
    // quer — mas **não** tem conversão para `uint64_t`; para extrair o
    // endereço há que chamar `GetValue()` (`typed_address.h:132`), e é
    // explícito de propósito.
    //
    // Substituir `GetValue()` por um `static_cast<std::uint64_t>(src)` dá
    // "invalid static_cast from ProcessAddress to uint64_t", e o que a
    // mensagem não diz é que o accessor se chama `GetValue`.

    bool ReadBlock(Common::ProcessAddress src, void* dest, std::size_t size) {
        std::string err;
        return memoria_.read(src.GetValue(), dest, size, err);
    }

    bool WriteBlock(Common::ProcessAddress dest, const void* src, std::size_t size) {
        std::string err;
        return memoria_.write(dest.GetValue(), src, size, err);
    }

    // O "unsafe" no Eden é ler sem verificar a permissão da página. Aqui não
    // há diferença: o nosso `GuestMemory` verifica o endereço contra a
    // reserva, e nãocontra permissões — a reserva é uma só, com uma só
    // protecção, posta pelo próprio motor.
    bool ReadBlockUnsafe(Common::ProcessAddress src, void* dest, std::size_t size) {
        std::string err;
        return memoria_.read(src.GetValue(), dest, size, err);
    }

    // **DUAS sobrecargas**, como no Eden (`core/memory.h:374-375`): uma `const`
    // que devolve `const u8*` e uma que devolve `u8*`. Não é redundância — o
    // `guest_memory.h:161` escreve
    //
    //     if (u8* ptr = m_memory->GetSpan(m_addr, this->size_bytes()); ptr)
    //
    // e sem a versão não-const o compilador escolhe a `const` e falha a
    // conversão. Duas sobrecargas que só diferem no `const` do retorno é
    // incomum, e é exactamente por isso que a segunda tem de existir.
    //
    // Ambas devolvem sempre `nullptr`. Ver a nota longa no topo deste
    // ficheiro: é o valor que o Eden trata como "não há span", e o
    // `guest_memory.h` cai para a cópia.
    const u8* GetSpan(const VAddr /*src*/, const std::size_t /*size*/) const {
        return nullptr;
    }
    u8* GetSpan(const VAddr /*src*/, const std::size_t /*size*/) { return nullptr; }

    // ── o ponteiro directo ────────────────────────────────────────────────
    //
    // `k_server_session.cpp:379` faz
    //
    //     msg_ptr = GetCurrentMemory(kernel).GetPointer<u32>(thread.GetTlsAddress());
    //
    // e precisa de um `u32*` para a message buffer do TLS — não de uma
    // cópia, de um **ponteiro**, porque o código escreve por ele.
    //
    // No Eden isto é uma tradução de endereço virtual para físico, porque a
    // memória do guest vive em páginas espalhadas. Aqui **o endereço do guest
    // é o endereço do host**: a reserva é um `mmap` de gama fixa feito pelo
    // motor, não há page table, e a conversão é uma subtração que não precisa
    // de ser feita. Ver `GuestMemory::host_pointer`.
    //
    // Devolve `nullptr` fora da reserva, como o Eden devolve — e a diferença
    // entre `GetPointer` e `GetPointerSilent` no Eden é exactamente quem trata
    // desse caso.
    u8* GetPointer(Common::ProcessAddress vaddr) {
        return memoria_.host_pointer(vaddr.GetValue(), 1);
    }
    u8* GetPointerSilent(Common::ProcessAddress vaddr) { return GetPointer(vaddr); }

    // A base e o tamanho da reserva.
    //
    // Usados pela `KPageTableBase` da ponte (`k_page_table_bridge.h`) para
    // saber se um bloco pedido cabe inteiro. É a **única** verificação de
    // limites que resta, porque não há páginas nem estado de páginas — e é a
    // que substitui a verificação de permissões que o Eden faz antes de cada
    // cópia.
    std::uint64_t base() const noexcept { return memoria_.base(); }
    std::uint64_t size_bytes() const noexcept { return memoria_.size(); }

    // O ponteiro do host para um endereço do guest, se o bloco inteiro couber.
    // Devolve `nullptr` quando não couber — que é o mesmo contrato de
    // `GetPointer`, e é o que permite a `KPageTableBase` decidir entre
    // `memcpy` e `ResultInvalidAddress`.
    std::uint8_t* host_pointer(std::uint64_t endereco, std::uint64_t size) const noexcept {
        return memoria_.host_pointer(endereco, size);
    }

    template <typename T>
    T* GetPointer(Common::ProcessAddress vaddr) {
        return reinterpret_cast<T*>(GetPointer(vaddr));
    }

    // `static constexpr bool HAS_FLUSH_INVALIDATION = false;` no Eden
    // (`core/memory.h:70`). Consumido em `guest_memory.h:128` dentro de um
    // `if constexpr`, e o seu valor decide se `FlushRegion` chega a ser
    // instanciada. O Eden põe `false`; nós também.
    static constexpr bool HAS_FLUSH_INVALIDATION = false;

    // Existe só para satisfazer o nome. Com `HAS_FLUSH_INVALIDATION` a
    // `false`, o `guest_memory.h` nunca a chama — e a razão de ela não fazer
    // nada é a razão de não haver nada para fazer: nós não temos cache de
    // páginas do guest para invalidar, porque a reserva é uma só e é nossa.
    void FlushRegion(const VAddr /*start*/, const std::size_t /*size*/) {}

    // ── o que a ponte usa ────────────────────────────────────────────────

    template <typename T>
    T Read(std::uint64_t endereco) const {
        T v{};
        std::string err;
        if (!memoria_.read(endereco, &v, sizeof(T), err))
            return T{};
        return v;
    }

    template <typename T>
    void Write(std::uint64_t endereco, T valor) {
        std::string err;
        (void)memoria_.write(endereco, &valor, sizeof(T), err);
    }

    // O limite é o mesmo do Eden (`KObjectName::NameLengthMax`): um nome que
    // não acaba dentro desse limite é um nome inválido, e ler sem limite
    // memória do guest é ler fora da reserva.
    std::string ReadCString(std::uint64_t endereco) const {
        return ReadCString(endereco, 33);
    }

    std::string ReadCString(std::uint64_t endereco, std::size_t max_length) const {
        if (max_length == 0 || max_length > 256)
            return {};
        char buf[256];
        std::string err;
        if (!memoria_.read(endereco, buf, max_length, err))
            return {};
        buf[max_length - 1] = '\0';
        return std::string(buf);
    }

    bool ReadBytes(std::uint64_t endereco, void* dest, std::size_t size) const {
        std::string err;
        return memoria_.read(endereco, dest, size, err);
    }
    bool WriteBytes(std::uint64_t endereco, const void* src, std::size_t size) {
        std::string err;
        return memoria_.write(endereco, src, size, err);
    }

private:
    ::g610::GuestMemory& memoria_;
};


// ── os aliases, copiados do Eden ───────────────────────────────────────────
//
// De `core/memory.h:507-510`, e copiados literalmente. São quatro linhas que
// ligam `GuestMemory` à NOSSA classe `Memory` em vez de à do Eden — que é
// exactamente a substituição que este ficheiro existe para fazer, e sem o
// `static_assert` a ninguém denotaria que o `Memory` trocado é o nosso.
//
//     template <typename T, GuestMemoryFlags FLAGS>
//     using CpuGuestMemory = GuestMemory<Core::Memory::Memory, T, FLAGS>;
//
// O `Core::Memory::Memory` que aparece aí é o NOSSO, e o compilador não
// distingue — que é o ponto.
template <typename T, GuestMemoryFlags FLAGS>
using CpuGuestMemory = GuestMemory<Memory, T, FLAGS>;

template <typename T, GuestMemoryFlags FLAGS>
using CpuGuestMemoryScoped = GuestMemoryScoped<Memory, T, FLAGS>;

} // namespace Memory

} // namespace Core