#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A simulacao do MME e o calculo do hash das macros, no modelo **exacto**
# do upstream:
#
#   - o codigo em buckets por chave (`macro.cpp`, `AddCode`:
#     `uploaded_macro_code[method].push_back(data)` — o ponteiro
#     `instruction_ptr` e' um registo escrito pelo metodo 69 e *
#     nao* auto-incrementa no `AddCode` (o `ProcessMethodCall` do
#     `maxwell_3d.cpp` na' chama `AddCode(instruction_ptr, argument)`);
#
#   - o bind por slot (`ProcessMacroBind`:
#     `macro_positions[start_address_ptr++] = data`);
#
#   - o hash exato (`common/container_hash.h`,
#     `HashCombineImpl<64>::fn`) sobre o *bucket* da macro, que e' o
#     que o `MacroEngine::Execute` compara com os builders do HLE.
#
# A semantica das palavras e' a do `dma_pusher.cpp` (`ProcessCommands`),
# por palavra — nao a do leitor da casa (o `for_each` colapsa os blocos
# NonIncreasing/IncreaseOnce).
#
# Uso:
#   python3 tools/simular_mme.py arquivo.bin [...]
MODE_INCREASING = 1
MODE_NON_INCREASING = 3
MODE_INLINE = 4
MODE_INCREASE_ONCE = 5

MACRO_REGS_START = 0xE00   # `maxwell_3d.cpp` (MacroRegistersStart)
NUM_MACRO_POSITIONS = 0x80 # `macro_positions` (`maxwell_3d.h`) ✓

LOAD_MME_INSTRUCTION_PTR = 69   # `load_mme.instruction_ptr` — ClearCode(ptr)
LOAD_MME_INSTRUCTION = 70       # `load_mme.instruction` — AddCode(ptr, arg)
LOAD_MME_START_ADDRESS_PTR = 71 # `load_mme.start_address_ptr` — escrita simples
LOAD_MME_START_ADDRESS = 72     # `load_mme.start_address` — ProcessMacroBind

# Os hashes inscritos no `macro_hle.cpp` do upstream, com o nome do
# builder (o `builders.emplace(hash, …)`; as classes sao as do ficheiro
# `macro_hle.cpp` ✓).
HLE_HASHES = {
    0X0D61FC9FAAC9FCAD: "HLE_DrawArraysIndirect<false>",
    0X8A4D173EB99A8603: "HLE_DrawArraysIndirect<true>",
    0X771BB18C62444DA0: "HLE_DrawIndexedIndirect<false>",
    0X0217920100488FF7: "HLE_DrawIndexedIndirect<true>",
    0X3F5E74B9C9A50164: "HLE_MultiDrawIndexedIndirectCount",
    0XEAD26C3E2109B06B: "HLE_MultiLayerClear",
    0XC713C83D8F63CCF3: "HLE_C713C83D8F63CCF3",
    0XD7333D26E0A93EDE: "HLE_D7333D26E0A93EDE",
    0XEB29B2A09AA06D38: "HLE_BindShader",
    0XDB1341DBEB4C8AF7: "HLE_SetRasterBoundingBox",
    0X6C97861D891EDF7E: "HLE_ClearConstBuffer<0x5F00>",
    0XD246FDDF3A6173D7: "HLE_ClearConstBuffer<0x7000>",
    0XEE4D0004BEC8ECF4: "HLE_ClearMemory",
    0XFC0CF27F5FFAA661: "HLE_TransformFeedbackSetup",
    0XB5F74EDB717278EC: "HLE_DrawIndirectByteCount",
}


def palavras(caminho):
    with open(caminho, "rb") as f:
        bruto = f.read()
    n = len(bruto) // 4
    return [int.from_bytes(bruto[i * 4:(i + 1) * 4], "little") for i in range(n)]


def passos_por_palavra(words):
    """A semantica por palavra do `ProcessCommands` (o upstream): rende
    (method, subchannel, argument) por chamada, sem colapsar blocos."""
    method = 0
    subchannel = 0
    method_count = 0
    non_incrementing = False
    dma_increment_once = False
    for index, bruto in enumerate(words):
        if method_count != 0:
            yield (method, subchannel, bruto, index)
            if not non_incrementing:
                method += 1
            if dma_increment_once:
                non_incrementing = True
            method_count -= 1
            continue
        mode = (bruto >> 29) & 7
        if mode in (MODE_INCREASING, MODE_NON_INCREASING, MODE_INCREASE_ONCE):
            method = bruto & 0x1FFF
            subchannel = (bruto >> 13) & 7
            method_count = (bruto >> 16) & 0x1FFF
            non_incrementing = mode == MODE_NON_INCREASING
            dma_increment_once = mode == MODE_INCREASE_ONCE
        elif mode == MODE_INLINE:
            yield (bruto & 0x1FFF, (bruto >> 13) & 7, (bruto >> 16) & 0x1FFF, index)
        # modos 0/2: ignorados (o `default` do upstream).


MASK64 = (1 << 64) - 1
HASH_M = ((0xC6A4A793 << 32) + 0x5BD1E995) & MASK64


def hash_value_u32_vec(vecu32):
    """`Common::HashValue(span<const u32>)` — o mesmo do upstream."""
    seed = 0
    for k in vecu32:
        k = (k * HASH_M) & MASK64
        k ^= k >> 47
        k = (k * HASH_M) & MASK64
        seed ^= k
        seed = (seed * HASH_M) & MASK64
        seed = (seed + 0xE6546B64) & MASK64
    return seed


def simular(words):
    """O motor (o Canal -> 3D): o bind dos subcanais e o MME."""
    instruction_ptr = 0
    start_address_ptr = 0
    bound = {}            # subcanal -> EngineID (o `BindObject`, o metodo 0)
    buckets = {}          # chave (o `instruction_ptr`) -> palavras (o bucket)
    positions = {}        # slot -> chave (o `macro_positions`)
    executing_macro = 0
    macro_params = []
    chamadas_macros = []  # (method, [params])

    def e_do_3d(sc):
        return bound.get(sc) == 0xB197  # o `MAXWELL_B` (`puller.cpp`)

    for method, sc, arg, index in passos_por_palavra(words):
        if method == 0:
            bound[sc] = arg
            continue
        if not e_do_3d(sc):
            continue
        if method >= MACRO_REGS_START:
            # O ProcessMacro (o upstream): o primeiro 'e o metodo da macro;
            # os params vem pelo metodo seguinte.
            if executing_macro == 0:
                executing_macro = method
            macro_params.append(arg)
            # o fim do burst (a proxima palavra de outro metodo).
            if index + 1 >= len(words) or True:
                pass
            continue
        if executing_macro != 0 and method != executing_macro + 1:
            # o fim implicito do burst de params (o `is_last_call`).
            chamadas_macros.append((executing_macro, macro_params))
            executing_macro = 0
            macro_params = []
        if method == LOAD_MME_INSTRUCTION_PTR:
            instruction_ptr = arg
        elif method == LOAD_MME_INSTRUCTION:
            buckets.setdefault(instruction_ptr, []).append(arg)
        elif method == LOAD_MME_START_ADDRESS_PTR:
            start_address_ptr = arg
        elif method == LOAD_MME_START_ADDRESS:
            positions[start_address_ptr % NUM_MACRO_POSITIONS] = arg
            start_address_ptr += 1
    if executing_macro != 0:
        chamadas_macros.append((executing_macro, macro_params))
    return buckets, positions, chamadas_macros


def main():
    import sys
    for caminho in sys.argv[1:]:
        words = palavras(caminho)
        buckets, positions, chamadas = simular(words)
        print(f"== {caminho} ==")
        total = sum(len(v) for v in buckets.values())
        print(f"instrucoes em buckets: {total} ({len(buckets)} buckets)")
        print(f"posicoes de macro: {positions}")
        contagem = {}
        for m, p in chamadas:
            contagem[m] = contagem.get(m, 0) + 1
        print("macro calls (por metodo):",
              " ".join(f"{m}x{n}" for m, n in sorted(contagem.items())))
        for metodo, params in sorted(chamadas, key=lambda x: x[0])[:40]:
            entry = ((metodo - MACRO_REGS_START) >> 1) % NUM_MACRO_POSITIONS
            chave = positions.get(entry)
            print(f" metodo={metodo} entry={entry} bucket={chave} "
                  f"params={params[:8]}")
        print("hash por bucket (o `Common::HashValue(code->second)`):")
        matches = {}
        for chave, codigo in sorted(buckets.items()):
            h = hash_value_u32_vec(codigo)
            nome = HLE_HASHES.get(h)
            if nome:
                matches[chave] = nome
            tag = f"* {nome}" if nome else ""
            print(f"  bucket={chave} len={len(codigo)} hash={h:016x} {tag}")
        print("baterias HLE:", matches if matches else "— (nenhuma)")
        # O hash da 1a macro de desenho (o entry 5 no caso real).
        chave5 = positions.get(5)
        if chave5 is not None and chave5 in buckets:
            h5 = hash_value_u32_vec(buckets[chave5])
            print(f"entry=5: bucket={chave5} len={len(buckets[chave5])} "
                  f"hash={h5:016x}")


if __name__ == "__main__":
    main()
