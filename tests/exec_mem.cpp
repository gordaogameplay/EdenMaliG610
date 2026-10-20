// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — memória executável para os testes de execução.
#include "exec_mem.hpp"

#include <sys/mman.h>
#include <cstring>

namespace g610::test {

void* mmap_exec(void* host_image, std::size_t size) {
    if (size == 0)
        return nullptr;

    // Página: é o que o mmap exige e o que evita straddling numa fronteira.
    const std::size_t page = 4096;
    const std::size_t rounded = (size + page - 1) & ~(page - 1);

    void* p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;

    if (host_image)
        std::memcpy(p, host_image, size);

    // Tornar executável. Numa implementação real, W^X seria o ideal
    // (escrever, depois selar em exec); aqui o guest precisa de ser escrito
    // pelo patcher e lido pelo CPU, e o objectivo do teste é funcional.
    if (::mprotect(p, rounded, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ::munmap(p, rounded);
        return nullptr;
    }

    // Limpar o cache de instruções: sem isto o CPU pode executar a versão
    // antiga da página depois de o patcher a ter escrito — e num AArch64 com
    // I-cache separada isto é um bug real, não teórico.
    __builtin___clear_cache(reinterpret_cast<char*>(p),
                            reinterpret_cast<char*>(p) + size);

    return p;
}

void munmap_exec(void* addr, std::size_t size) {
    if (!addr || size == 0)
        return;
    const std::size_t page = 4096;
    const std::size_t rounded = (size + page - 1) & ~(page - 1);
    ::munmap(addr, rounded);
}

} // namespace g610::test
