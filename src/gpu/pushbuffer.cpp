// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o leitor do push buffer (o fluxo de métodos do Maxwell).
//
// Porta directa do upstream (`src/video_core/dma_pusher.cpp`,
// `DmaPusher::ProcessCommands`), com a mesma máquina de estados e as
// mesmas regras de consumo de argumentos.
#include "g610/gpu/pushbuffer.hpp"

#include <cstring>

namespace g610::gpu {

namespace {

// Lê a palavra i do buffer. O push buffer pode não estar alinhado (vem
// de um vector do host), por isso a leitura é por `memcpy`.
bool palavra(const std::vector<std::uint8_t>& buffer, std::size_t i,
             std::uint32_t& out) {
    const std::size_t off = i * 4;
    if (off + 4 > buffer.size())
        return false;
    std::memcpy(&out, buffer.data() + off, 4);
    return true;
}

}  // namespace

std::size_t PushBufferReader::for_each(const std::vector<std::uint8_t>& buffer,
                                        const Callback& callback) {
    descartados_ = 0;

    const std::size_t words = buffer.size() / 4;
    if (words == 0)
        return 0;

    // O estado da máquina — as mesmas variáveis do upstream.
    std::uint32_t method = 0;
    std::uint32_t subchannel = 0;
    std::uint32_t method_count = 0;
    bool non_incrementing = false;
    bool dma_increment_once = false;

    std::size_t lidos = 0;
    std::size_t index = 0;

    while (index < words) {
        std::uint32_t bruto = 0;
        if (!palavra(buffer, index, bruto)) {
            ++descartados_;
            break;
        }

        const CommandHeader header{bruto};

        if (method_count != 0) {
            // Uma palavra de argumento de um método em curso.
            if (non_incrementing) {
                // `NonIncreasing`: os argumentos são um bloco para o
                // mesmo método (o `CallMultiMethod` do upstream).
                std::uint32_t max_write = method_count;
                if (index + max_write > words)
                    max_write = static_cast<std::uint32_t>(words - index);
                MethodCall call;
                call.method = method;
                call.subchannel = subchannel;
                call.method_count = method_count;
                call.argument = bruto;
                call.is_last_call = true;
                callback(call);
                ++lidos;
                method_count -= max_write;
                index += max_write;
                if (method_count == 0)
                    dma_increment_once = false;
                continue;
            }

            // `Increasing`: cada argumento é uma chamada, e o método
            // incrementa a cada uma.
            MethodCall call;
            call.method = method;
            call.argument = bruto;
            call.subchannel = subchannel;
            call.method_count = method_count;
            call.is_last_call = method_count <= 1;
            callback(call);
            ++lidos;

            method_count--;
            method++;
            if (dma_increment_once)
                non_incrementing = true;
            index++;
            continue;
        }

        // A primeira palavra de um método novo.
        switch (header.mode()) {
        case modo::Increasing:
            method = header.method();
            subchannel = header.subchannel();
            method_count = header.arg_count();
            non_incrementing = false;
            dma_increment_once = false;
            break;
        case modo::NonIncreasing:
            method = header.method();
            subchannel = header.subchannel();
            method_count = header.arg_count();
            non_incrementing = true;
            dma_increment_once = false;
            break;
        case modo::Inline:
            // O próprio cabeçalho é a chamada, e o argumento é a
            // contagem (o upstream: `CallMethod(command_header.arg_count)`).
            method = header.method();
            subchannel = header.subchannel();
            non_incrementing = true;
            dma_increment_once = false;
            {
                MethodCall call;
                call.method = method;
                call.argument = header.arg_count();
                call.subchannel = subchannel;
                call.method_count = 1;
                call.is_last_call = true;
                callback(call);
                ++lidos;
            }
            break;
        case modo::IncreaseOnce:
            method = header.method();
            subchannel = header.subchannel();
            method_count = header.arg_count();
            non_incrementing = false;
            dma_increment_once = true;
            break;
        default:
            // Modo desconhecido: a palavra não é um cabeçalho válido.
            // Conta-se e avança-se uma palavra — o mesmo que o upstream
            // faz com um buffer corrompido.
            ++descartados_;
            break;
        }
        index++;
    }

    // As palavras que sobraram (o método em curso ficou incompleto).
    if (method_count != 0)
        descartados_ += method_count;

    return lidos;
}

std::size_t PushBufferReader::for_each_canal(const std::vector<std::uint8_t>& buffer,
                                              const Callback& callback) {
    std::size_t n = 0;
    for_each(buffer, [&](const MethodCall& call) {
        // Os métodos do canal são os do subcanal 0 (o *puller*); os
        // métodos de um motorlinked subchannel são de outro motor.
        if (call.subchannel != 0)
            return;
        callback(call);
        ++n;
    });
    return n;
}

}  // namespace g610::gpu
