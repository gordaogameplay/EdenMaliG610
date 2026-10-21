// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o leitor do push buffer (o fluxo de métodos do Maxwell).
//
// ## De onde vem isto
//
// O upstream do Eden (`src/video_core/dma_pusher.cpp`, `ProcessCommands`)
// percorre o push buffer como um **fluxo de métodos**: cada método
// começa por um `CommandHeader` (uma palavra de 32 bits) e é seguido
// pelos seus argumentos.
//
//     CommandHeader (u32)
//       bits  0..12  o método (o índice do registo no motor)
//       bits 13..15  o subcanal
//       bits 16..28  o número de argumentos
//       bits 29..31  o modo
//
// O modo decide como os argumentos são consumidos:
//
//   Increasing     — cada argumento é uma chamada, e o **método
//                    incrementa** a cada uma (o registo auto-incrementa);
//   NonIncreasing  — os argumentos são um bloco para o mesmo método;
//   Inline         — o próprio cabeçalho é a chamada;
//   IncreaseOnce   — como Increasing, mas o incremento só acontece uma vez.
//
// ## Os métodos do CANAL
//
// Os métodos que não pertencem a um motor (os do *puller* do upstream)
// são os que o próprio canal executa: o bind dos subcanais, os
// semáforos e a fence. As posições são as do `Puller::Regs` do upstream
// (`src/video_core/engines/puller.h`, pelas `ASSERT_REG_POSITION`).
//
// O que importa aqui: a **fence** (0x1D) e os **semáforos** (0x04..0x07,
// 0x1A, 0x1B). É por aí que o driver diz ao jogo "o trabalho acabou" —
// e é o que faltava para o jogo sair do impasse.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace g610::gpu {

// Uma chamada de método — o `MethodCall` do upstream
// (`src/video_core/engines/puller.h`).
struct MethodCall {
    std::uint32_t method = 0;
    std::uint32_t argument = 0;
    std::uint32_t subchannel = 0;
    std::uint32_t method_count = 0;  // quantos argumentos ainda faltam
    bool          is_last_call = false;
};

// Os métodos do canal (o *puller*). As posições vêm do upstream
// (`puller.h`, `ASSERT_REG_POSITION`).
namespace canal {
inline constexpr std::uint32_t SemaphoreAddressHigh = 0x04;
inline constexpr std::uint32_t SemaphoreAddressLow = 0x05;
inline constexpr std::uint32_t SemaphoreSequence = 0x06;
inline constexpr std::uint32_t SemaphoreTrigger = 0x07;
inline constexpr std::uint32_t ReferenceCount = 0x14;
inline constexpr std::uint32_t SemaphoreAcquire = 0x1A;
inline constexpr std::uint32_t SemaphoreRelease = 0x1B;
inline constexpr std::uint32_t FenceValue = 0x1C;
inline constexpr std::uint32_t FenceAction = 0x1D;
}  // namespace canal

// As operações da fence (`Puller::FenceOperation`): o bit 0 da palavra.
namespace fence_op {
inline constexpr std::uint32_t Acquire = 0;
inline constexpr std::uint32_t Increment = 1;
}  // namespace fence_op

// As operações do semáforo (`Puller::GpuSemaphoreOperation`): os 4 bits
// baixos da palavra do trigger.
namespace sem_op {
inline constexpr std::uint32_t AcquireEqual = 0x1;
inline constexpr std::uint32_t WriteLong = 0x2;
inline constexpr std::uint32_t AcquireGequal = 0x4;
inline constexpr std::uint32_t AcquireMask = 0x8;
}  // namespace sem_op

// O cabeçalho de um método (`Tegra::CommandHeader` do upstream,
// `src/video_core/dma_pusher.h`).
struct CommandHeader {
    std::uint32_t raw = 0;

    std::uint32_t method() const { return raw & 0x1FFFu; }
    std::uint32_t subchannel() const { return (raw >> 13) & 0x7u; }
    std::uint32_t arg_count() const { return (raw >> 16) & 0x1FFFu; }
    std::uint32_t mode() const { return (raw >> 29) & 0x7u; }
};

// Os modos (`Tegra::SubmissionMode` do upstream, `video_core/dma_pusher.h`).
//
// **Copia exacta do upstream** ✓ — e a tabela errada custou caro ✗.
//
// O upstream (`dma_pusher.h:35-42` ✓):
//
//     enum class SubmissionMode : u32 {
//         IncreasingOld = 0,
//         Increasing = 1,
//         NonIncreasingOld = 2,
//         NonIncreasing = 3,
//         Inline = 4,
//         IncreaseOnce = 5
//     };
//
// A casa tinha `Increasing = 0` ✗, `NonIncreasing = 1` ✗, `Inline = 2` ✗ e
// `IncreaseOnce = 3` ✗ — **todos deslocados** ✗.
//
// **Consequencia medida** ✗: o dump do push buffer real (`/tmp/boot_v4.log`
// ✗) mostra `modo=1` ✗ — que no upstream e' `Increasing` ✓ (o modo normal
// ✗) ✗, mas a casa tratava-o como `NonIncreasing` ✗. O leitor consumia os
// argumentos pelo caminho errado ✗ ✗ — o fluxo dessincronizava ✗ — e
// nunca chegava ao `draw_end` ✗ nem aos metodos do *puller* ✗ (o
// semaforo ✗ e a fence ✗) ✗.
//
// **E o mais grave** ✗: os modos `0` ✗ e `2` ✗ (`IncreasingOld` ✗ e
// `NonIncreasingOld` ✗) **nao existem** no upstream ✗ — o `ProcessCommands`
// (`dma_pusher.cpp:164-191` ✗) cai no `default` ✗ e **ignora** a palavra
// ✗. A casa, com a tabela errada ✗, tratava-os como modos validos ✗ e
// comia o fluxo ✗ ✗.
namespace modo {
inline constexpr std::uint32_t IncreasingOld = 0;
inline constexpr std::uint32_t Increasing = 1;
inline constexpr std::uint32_t NonIncreasingOld = 2;
inline constexpr std::uint32_t NonIncreasing = 3;
inline constexpr std::uint32_t Inline = 4;
inline constexpr std::uint32_t IncreaseOnce = 5;
}  // namespace modo

// O leitor do push buffer.
class PushBufferReader {
public:
    using Callback = std::function<void(const MethodCall&)>;

    // Percorre o push buffer e chama `callback` para cada chamada de
    // método, pela ordem em que a GPU as executaria.
    //
    // Devolve o número de métodos lidos. Um buffer malformado não
    // propaga: o leitor pára no que dá para ler e conta o resto como
    // descartado — é o sintoma que interessa (e é o que a telemetria
    // mostra).
    std::size_t for_each(const std::vector<std::uint8_t>& buffer, const Callback& callback);

    // O mesmo, mas só com os métodos do CANAL (os que o *puller*
    // executa). O resto é ignorado — quem os executa é o motor.
    std::size_t for_each_canal(const std::vector<std::uint8_t>& buffer,
                               const Callback& callback);

    // Quantos métodos não foram lidos por o buffer ter ficado malformado.
    std::size_t descartados() const { return descartados_; }

private:
    std::size_t descartados_ = 0;
};

}  // namespace g610::gpu
