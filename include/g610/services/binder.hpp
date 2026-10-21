// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O compositor — o lado do binder que recebe os buffers do jogo.
//
// A implementacao esta' em `src/services/binder.cpp`; aqui fica so a
// declaracao, porque quem o chama e' o `HosBinderDriver` (em
// `src/services/vi.cpp`).
#pragma once

#include <cstdint>
#include <vector>

namespace g610::services {

// Atende uma transaccao do `IGraphicBufferProducer` (o protocolo do
// binder do Android) e devolve o parcel da resposta.
//
// `codigo` e' o codigo da transaccao (9 = Query, 10 = Connect,
// 14 = SetPreallocatedBuffer, 3 = DequeueBuffer, 7 = QueueBuffer) e
// `entrada` e' o parcel que o jogo mandou.
std::vector<std::uint8_t> TransactionarParcel(std::uint32_t codigo,
                                               const std::vector<std::uint8_t>& entrada);

// Aloca o buffer da **native window** (onde o jogo desenha) e devolve o
// endereco (0 se nao houver espaco).
std::uint64_t AlocarJanelaDoCompositor(std::uint64_t tamanho);

// Despeja os primeiros bytes da native window (o diagnostico de "o jogo
// desenhou?").
void DespejarJanelaDoCompositor();

// Varre a regiao do canal da GPU a procura de conteudo escrito.
void VarrerRegiaoDoCanalDoCompositor();

} // namespace g610::services
