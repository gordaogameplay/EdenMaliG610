#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extrai a tabela completa dos **comandos** do Maxwell 3D do upstream.

O que é
-------
No motor Maxwell do Eden, o ID de um método **é o índice de um
registo** (`MAXWELL3D_REG_INDEX` = `offsetof(Regs, campo) / 4` ✗). A
fonte da verdade é, portanto, a estrutura `Regs` do
`src/video_core/engines/maxwell_3d.h` do upstream ✓.

Este script:
  1. lê a estrutura `Regs` e tira os **caminhos dos campos** (as
     folhas: `vertex_buffer.count` ✓, `const_buffer.buffer` ✓, ...);
  2. gera um programa que imprime o índice de cada um ✓ — compilado
     **contra o cabeçalho do upstream** ✓, que é a única forma de o
     saber sem adivinhar ✓;
  3. escreve `include/g610/gpu/maxwell_comandos.hpp` ✓ — a tabela que a
     casa usa ✓.

Os campos que não compilam (os bit-fields ✗ — o `offsetof` não é
aplicável a eles ✗) são descartados ✓ e listados no relatório ✓.

Uso:
    python3 tools/extrair_comandos_maxwell.py
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CABECALHO = os.path.join(RAIZ, "vendor", "eden", "src", "video_core", "engines",
                          "maxwell_3d.h")
SAIDA = os.path.join(RAIZ, "include", "g610", "gpu", "maxwell_comandos.hpp")

# Um campo: `u32 nome;`, `u32 nome[4];`, `BitField<..> nome;`,
# `u32 nome : 3;`, `GPUVAddr nome;`...
RE_CAMPO = re.compile(
    r"^\s*(?:const\s+)?(?:u8|u16|u32|u64|s8|s16|s32|s64|bool|float|double"
    r"|GPUVAddr|DAddr|PAddr|VAddr|GPUVAddrRange|BitField<[^>]+>|Tegra::[A-Za-z_:<>]+)"
    r"\s+([A-Za-z_][A-Za-z_0-9]*)\s*(?:\[[^\]]*\])?\s*(?::\s*\d+\s*)?;")
RE_STRUCT = re.compile(r"^\s*struct\s+([A-Za-z_][A-Za-z_0-9]*)\s*\{")


def campos_da_estrutura(texto: str, nome: str) -> list[str]:
    """Os caminhos das folhas de `struct <nome> { ... }`."""
    m = re.search(r"struct\s+" + re.escape(nome) + r"\s*\{", texto)
    if not m:
        return []
    i = m.end()
    nivel = 1
    corpo: list[str] = []
    while i < len(texto) and nivel > 0:
        c = texto[i]
        if c == "{":
            nivel += 1
        elif c == "}":
            nivel -= 1
            if nivel == 0:
                break
        corpo.append(c)
        i += 1
    return _folhas("".join(corpo))


def _folhas(texto: str, prefixo: str = "") -> list[str]:
    saida: list[str] = []
    pilha: list[str] = []
    for linha in texto.split("\n"):
        t = linha.strip()
        if not t or t.startswith("//") or t.startswith("/*") or t.startswith("*"):
            continue
        ms = RE_STRUCT.match(t)
        if ms:
            pilha.append(ms.group(1))
            continue
        if t.startswith("}") and pilha:
            pilha.pop()
            continue
        mc = RE_CAMPO.match(t)
        if mc:
            caminho = ".".join(pilha + [mc.group(1)])
            saida.append(caminho)
    return saida


def main() -> int:
    if not os.path.exists(CABECALHO):
        print(f"falta o cabeçalho do upstream: {CABECALHO}", file=sys.stderr)
        return 1
    texto = open(CABECALHO, encoding="utf-8").read()
    nomes = campos_da_estrutura(texto, "Regs")
    # Os que o upstream usa nos .cpp são a certeza; os outros sao
    # candidatos (podem ser bit-fields, que nao compilam).
    usados = set()
    pasta = os.path.dirname(CABECALHO)
    for nome in os.listdir(pasta):
        if nome.endswith(".cpp"):
            for m in re.finditer(r"MAXWELL3D_REG_INDEX\(([a-z_0-9.]+)\)",
                                 open(os.path.join(pasta, nome), encoding="utf-8").read()):
                usados.add(m.group(1))
    candidatos = sorted(set(nomes) | usados)
    print(f"{len(candidatos)} candidatos")

    # Compilar em tentativas: cada um que falhe e' descartado.
    bons: list[tuple[str, int]] = []
    maus: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        prog = os.path.join(tmp, "idx.cpp")
        binario = os.path.join(tmp, "idx")
        for nome in candidatos:
            with open(prog, "w", encoding="utf-8") as f:
                f.write('#include <cstddef>\n#include <cstdio>\n'
                        '#include "video_core/engines/maxwell_3d.h"\n'
                        '#define IDX(x) (offsetof(Tegra::Engines::Maxwell3D::Regs, x) / sizeof(u32))\n'
                        'int main() { printf("%zu\\n", IDX(' + nome + ')); return 0; }\n')
            cmd = ["g++", "-std=c++20", "-O0", "-I",
                   os.path.join(RAIZ, "vendor", "eden", "src"),
                   "-I", os.path.join(RAIZ, "vendor", "third_party", "Vulkan-Headers", "include"),
                   "-o", binario, prog]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                maus.append(nome)
                continue
            saida = subprocess.run([binario], capture_output=True, text=True)
            try:
                bons.append((nome, int(saida.stdout.strip())))
            except ValueError:
                maus.append(nome)

    print(f"compilaram: {len(bons)}  descartados: {len(maus)}")

    with open(SAIDA, "w", encoding="utf-8") as f:
        f.write("// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay\n")
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
        f.write("// EdenMaliG610 — os comandos do motor Maxwell 3D.\n//\n")
        f.write("// Gerado por `tools/extrair_comandos_maxwell.py` — não editar à mão.\n//\n")
        f.write("// A fonte da verdade é o upstream (`src/video_core/engines/maxwell_3d.h`),\n")
        f.write("// e os índices saem de `offsetof(Regs, campo) / 4` — a mesma macro que o\n")
        f.write("// Eden usa (`MAXWELL3D_REG_INDEX`). Compilados contra o cabeçalho do\n")
        f.write("// upstream, não adivinhados.\n//\n")
        f.write("// O ID de um método **é** o índice do registo: no push buffer, o\n")
        f.write("// cabeçalho do método traz esse ID nos bits 0..12 (ver\n")
        f.write("// `g610/gpu/pushbuffer.hpp`).\n#pragma once\n\n")
        f.write("#include <cstdint>\n\nnamespace g610::gpu {\n\n")
        f.write("// Os comandos do Maxwell 3D (o índice do registo no upstream).\n")
        f.write("namespace maxwell3d {\n")
        for nome, indice in sorted(bons, key=lambda x: x[1]):
            f.write(f"inline constexpr std::uint32_t {nome.replace('.', '_')} = {indice};\n")
        f.write("}  // namespace maxwell3d\n\n}  // namespace g610::gpu\n")

    print(f"escrito {SAIDA} ({len(bons)} comandos)")
    if maus:
        print(f"descartados: {len(maus)} (bit-fields e campos não plano)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
