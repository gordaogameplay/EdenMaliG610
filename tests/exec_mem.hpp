// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — memória executável para o guest nos testes.
//
// O patcher escreve no .text do guest para o transformar (SVC->BRK). O .text
// de um executável vivo é read-only, por isso os testes correm uma cópia numa
// página com PROT_EXEC.
//
// Isto é também o que o motor faz em produção, com uma diferença: num jogo
// real a imagem vem do NSO e é mapeada executável uma vez; aqui é um buffer
// alocado para o teste.
#pragma once

#include <cstddef>
#include <cstdint>

namespace g610::test {

// Reserva `size` bytes alinhados a 4 KB, com permissão de leitura, escrita e
// execução. Devolve nullptr se não for possível.
void* mmap_exec(void* host_image, std::size_t size);

// Liberta a região. `size` tem de ser o mesmo usado em mmap_exec.
void munmap_exec(void* addr, std::size_t size);

} // namespace g610::test
