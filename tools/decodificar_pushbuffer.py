#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
#
# O decodificador offline do push buffer de EdenMaliG610.
#
# Reproduz **exactamente** a maquina de estados do `PushBufferReader::for_each`
# (`src/gpu/pushbuffer.cpp`) em Python, para ler os dumps do
# `G610_GPFIFO_DUMP` fora da placa e verificar o teste deterministico
# contra o log vivo do Orange Pi.
#
# Uso:
#   python3 tools/decodificar_pushbuffer.py arquivo.bin [outro.bin ...]
#   python3 tools/decodificar_pushbuffer.py --pedidos arquivos.bin
#
# O formato (o mesmo do header `g610/gpu/pushbuffer.hpp`, do upstream
# `dma_pusher.h`): cada palavra u32 little-endian:
#   bits 0..12  o metodo
#   bits 13..15 o subcanal
#   bits 16..28 o numero de argumentos
#   bits 29..31 o modo (0 e 2 = modos antigos, ignorados palavra a palavra)
import sys
from collections import Counter

MODO_INCREASING = 1
MODO_NON_INCREASING = 3
MODO_INLINE = 4
MODO_INCREASE_ONCE = 5


def palavras(caminho):
    with open(caminho, "rb") as f:
        bruto = f.read()
    n = len(bruto) // 4
    return [int.from_bytes(bruto[i * 4:(i + 1) * 4], "little") for i in range(n)]


def decodificar(words):
    """A mesma maquina de estados do `PushBufferReader::for_each`."""
    descartados = 0
    method = 0
    subchannel = 0
    method_count = 0
    non_incrementing = False
    dma_increment_once = False
    total_words = len(words)
    chamadas = []
    index = 0
    while index < total_words:
        bruto = words[index]
        if method_count != 0:
            if non_incrementing:
                # O mesmo da casa: UM callback com a primeira palavra e
                # o resto do bloco e' consumido de uma vez (`index +=
                # max_write`). O upstream faz `CallMultiMethod` com o
                # bloco todo; o da casa entrega a primeira palavra.
                max_write = min(method_count, total_words - index)
                chamadas.append(
                    dict(index=index, method=method, subchannel=subchannel,
                         argument=bruto, method_count=method_count,
                         is_last_call=True))
                method_count -= max_write
                index += max_write
                continue
            chamadas.append(
                dict(index=index, method=method, subchannel=subchannel,
                     argument=bruto, method_count=method_count,
                     is_last_call=method_count <= 1))
            method_count -= 1
            method += 1
            if dma_increment_once:
                non_incrementing = True
            index += 1
            continue
        # A primeira palavra de um metodo novo.
        modo = (bruto >> 29) & 7
        if modo == MODO_INCREASING:
            method = bruto & 0x1FFF
            subchannel = (bruto >> 13) & 7
            method_count = (bruto >> 16) & 0x1FFF
            non_incrementing = False
            dma_increment_once = False
        elif modo == MODO_NON_INCREASING:
            method = bruto & 0x1FFF
            subchannel = (bruto >> 13) & 7
            method_count = (bruto >> 16) & 0x1FFF
            non_incrementing = True
            dma_increment_once = False
        elif modo == MODO_INLINE:
            method = bruto & 0x1FFF
            subchannel = (bruto >> 13) & 7
            non_incrementing = True
            dma_increment_once = False
            chamadas.append(
                dict(index=index, method=method, subchannel=subchannel,
                     argument=(bruto >> 16) & 0x1FFF, method_count=1,
                     is_last_call=True))
        elif modo == MODO_INCREASE_ONCE:
            method = bruto & 0x1FFF
            subchannel = (bruto >> 13) & 7
            method_count = (bruto >> 16) & 0x1FFF
            non_incrementing = False
            dma_increment_once = True
        else:
            descartados += 1
        if method_count == 0 and modo in (MODO_INCREASING, MODO_NON_INCREASING,
                                          MODO_INCREASE_ONCE):
            pass
        index += 1
    # O que sobrou: o bloco ficou incompleto.
    if method_count != 0:
        descartados += method_count
    return chamadas, descartados


def main():
    args = sys.argv[1:]
    if not args or "--ajuda" in args or "-h" in args:
        print("uso: decodificar_pushbuffer.py [-v] arquivo.bin [...]")
        sys.exit(args == [])
    verboso = False
    if args and args[0] == "-v":
        verboso = True
        args = args[1:]
    for caminho in args:
        words = palavras(caminho)
        chamadas, descartados = decodificar(words)
        ids = Counter(c["method"] for c in chamadas)
        por_sub = Counter(c["subchannel"] for c in chamadas)
        print(f"== {caminho} ==")
        print(f"palavras={len(words)} bytes={len(words) * 4} "
              f"chamadas={len(chamadas)} descartados={descartados} "
              f"ids_unicos={len(ids)}")
        print("por_subcanal:", " ".join(
            f"{sc}:{n}" for sc, n in sorted(por_sub.items())))
        binds = [(c["index"], c["subchannel"], c["argument"]) for c in chamadas
                 if c["method"] == 0 and c["argument"] != 0]
        if binds:
            print("BindObject:", " ".join(
                f"sc{sc}=0x{arg:X}@{idx}" for idx, sc, arg in binds))
        macros = Counter(c["method"] for c in chamadas if c["method"] >= 0xE00)
        if macros:
            print("macros (id:vezes):", " ".join(
                f"{m}:{n}" for m, n in sorted(macros.items())))
        # Os ids que a regla do translator usa para desenho.
        desenho = {"draw_end": 1413, "draw_begin": 1414}
        for nome, metodo in desenho.items():
            print(f"{nome} ({metodo}): {'PRESENTE ' + str(ids[metodo]) if ids[metodo] else 'AUSENTE'}")
        if verboso:
            for c in chamadas:
                print("  dec[{idx}] sc={sc} m={m}={m:#x} arg={arg:#010x} "
                      "args={args}".format(idx=c["index"], sc=c["subchannel"],
                                           m=c["method"], arg=c["argument"],
                                           args=c["method_count"]))


if __name__ == "__main__":
    main()
