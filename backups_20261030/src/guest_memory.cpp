// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — espaço de endereçamento do guest.
#include "g610/guest_memory.hpp"

#include "g610/nso_loader.hpp" // kGuestPageSize

#include <execinfo.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace g610 {

int to_posix(Prot p) noexcept {
    int flags = 0;
    if (static_cast<std::uint8_t>(p) & static_cast<std::uint8_t>(Prot::Read))
        flags |= PROT_READ;
    if (static_cast<std::uint8_t>(p) & static_cast<std::uint8_t>(Prot::Write))
        flags |= PROT_WRITE;
    if (static_cast<std::uint8_t>(p) & static_cast<std::uint8_t>(Prot::Exec))
        flags |= PROT_EXEC;
    return flags;
}

GuestMemory::~GuestMemory() { unmap(); }

GuestMemory::GuestMemory(GuestMemory&& o) noexcept
    : base_(std::exchange(o.base_, 0)),
      size_(std::exchange(o.size_, 0)),
      exec_reads_(std::exchange(o.exec_reads_, 0)),
      exec_writes_(std::exchange(o.exec_writes_, 0)) {}

GuestMemory& GuestMemory::operator=(GuestMemory&& o) noexcept {
    if (this == &o)
        return *this;
    unmap();
    base_       = std::exchange(o.base_, 0);
    size_       = std::exchange(o.size_, 0);
    exec_reads_ = std::exchange(o.exec_reads_, 0);
    exec_writes_= std::exchange(o.exec_writes_, 0);
    return *this;
}

void GuestMemory::unmap() noexcept {
    if (base_ == 0)
        return;
    ::munmap(reinterpret_cast<void*>(base_), static_cast<std::size_t>(size_));
    base_ = 0;
    size_ = 0;
}

bool GuestMemory::map(std::uint64_t base, std::uint64_t size, Prot prot,
                      std::string& error) {
    if (base == 0 || (base % kGuestPageSize) != 0) {
        error = "base do guest não alinhada a 4096: 0x" + std::to_string(base);
        return false;
    }
    if (size == 0 || (size % kGuestPageSize) != 0) {
        error = "tamanho do guest não múltiplo de 4096: " + std::to_string(size);
        return false;
    }

    // MAP_FIXED_NOREPLACE falha se o endereço já estiver ocupado. MAP_FIXED
    // sobrepunha em silêncio — e um mapeamento cego a este nível pode
    // destruir a memória de outra biblioteca sem nenhum sintoma até muito
    // depois.
    // **MAP_NORESERVE**: o guest tem um espaco de enderecos de 39 bits, e
    // a reserva e' de varios gigabytes. O overcommit do Linux (heuristico)
    // pode recusar uma reserva tao grande — e o guest so' toca nas paginas
    // que usa. `MAP_NORESERVE` diz ao kernel para nao contar a reserva
    // para o overcommit: as paginas so' sao contabilizadas quando o guest
    // as toca, e nesse momento ha RAM ou nao ha.
    //
    // Medido: sem isto, o `mmap` de 0x60000000 + 8,75 GiB falha com
    // ENOMEM num Pi de 7,7 GiB — e o guest nem chega a arrancar.
    void* p = ::mmap(reinterpret_cast<void*>(base),
                     static_cast<std::size_t>(size), to_posix(prot),
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
                     -1, 0);

    if (p == MAP_FAILED) {
        // MAP_FIXED_NOREPLACE devolve o endereço pedido em caso de falha, o
        // que não serve para diagnóstico. Reproduzir a falha com MAP_FIXED
        // numa janela descartável à parte não ajuda; o que ajuda é dizer qual
        // é o endereço e o tamanho, para o erro ser accionável.
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "não consegui mapear 0x%llx + %llu bytes (%s)",
                      static_cast<unsigned long long>(base),
                      static_cast<unsigned long long>(size), std::strerror(errno));
        error = buf;
        return false;
    }

    base_ = base;
    size_ = size;
    return true;
}

bool GuestMemory::protect(std::uint64_t base, std::uint64_t size, Prot prot,
                          std::string& error) {
    if (!mapped()) {
        error = "espaço do guest não mapeado";
        return false;
    }
    if (!owns(base) || !owns(base + size)) {
        error = "mprotect fora do espaço do guest";
        return false;
    }
    if (::mprotect(reinterpret_cast<void*>(base), static_cast<std::size_t>(size),
                   to_posix(prot)) != 0) {
        error = std::string("mprotect falhou: ") + std::strerror(errno);
        return false;
    }
    return true;
}

// A pagina do watchpoint — o mesmo global do despachante.
extern volatile std::uint64_t g_watch_pagina;

bool GuestMemory::write(std::uint64_t dst, const void* src, std::uint64_t size,
                        std::string& error) {
    // **O canario** — a escrita do HOST na memoria do guest e' um
    // `memcpy`: nao levanta fault (a pagina pode estar em so'
    // leitura) e por isso o watchpoint nao a apanha.
    //
    // **Esta e' a unica forma de ver QUEM escreve** — e a resposta
    // importa: o global do jogo vale `0xcb9000000000808e` (um
    // valor que nao e' do guest).
    if (std::getenv("IA_WATCH") != nullptr && g_watch_pagina != 0 && size != 0) {
        const std::uint64_t lo = g_watch_pagina;
        const std::uint64_t hi = lo + 0x1000;
        if (dst < hi && dst + size > lo) {
            std::fprintf(stderr,
                         "[watch] O HOST escreve em 0x%llx (+%llu bytes) — "
                         "a pagina e' do jogo \n",
                         (unsigned long long)dst, (unsigned long long)size);
            void* quadros[16]{};
            const int n = ::backtrace(quadros, 16);
            ::backtrace_symbols_fd(quadros, n, 2);
        }
    }
    if (!mapped()) {
        error = "espaço do guest não mapeado";
        return false;
    }
    if (size == 0)
        return true;
    if (!owns(dst) || !owns(dst + size)) {
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "escrita fora do espaço do guest: 0x%llx + %llu",
                      static_cast<unsigned long long>(dst),
                      static_cast<unsigned long long>(size));
        error = buf;
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(dst), src, static_cast<std::size_t>(size));
    return true;
}

bool GuestMemory::read(std::uint64_t src, void* dst, std::uint64_t size,
                       std::string& error) const {
    if (!mapped()) {
        error = "espaço do guest não mapeado";
        return false;
    }
    if (size == 0)
        return true;
    if (!owns(src) || !owns(src + size)) {
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "leitura fora do espaço do guest: 0x%llx + %llu",
                      static_cast<unsigned long long>(src),
                      static_cast<unsigned long long>(size));
        error = buf;
        return false;
    }
    std::memcpy(dst, reinterpret_cast<const void*>(src),
                static_cast<std::size_t>(size));
    return true;
}

bool GuestMemory::owns(std::uint64_t addr) const noexcept {
    if (base_ == 0)
        return false;
    // `addr < base + size` tem de ser escrito sem overflow: um addr muito
    // grande (o guest a calcular fora das paredes) somaria e daria dentro.
    return addr >= base_ && addr - base_ < size_;
}

std::uint8_t* GuestMemory::host_pointer(std::uint64_t addr, std::uint64_t size) const noexcept {
    if (!owns(addr))
        return nullptr;
    // `addr - base_` cabe em `size_` porque `owns` já o verificou; falta
    // garantir que o bloco pedido **termina** dentro da reserva.
    //
    // A verificação é escrita sem overflow pelo mesmo motivo que a de `owns`:
    // `size` vem do guest e pode ser qualquer coisa, e `addr + size` a somar
    // daria dentro quando quer dizer fora.
    if (size > size_ || addr - base_ > size_ - size)
        return nullptr;
    // E o mapeamento é de gama fixa, feito pelo motor: o endereço do guest é
    // o endereço do host. Ver a nota no cabeçalho.
    return reinterpret_cast<std::uint8_t*>(addr);
}

} // namespace g610
