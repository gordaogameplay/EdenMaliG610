#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extrai as listas de fontes do `core/` e do `video_core` do upstream.

Os `CMakeLists.txt` do upstream são a **fonte da verdade** das fontes
(mesmo como o README da casa exige para o seu `vendor/`). Em vez de as
reescrever à mão — e as errar — extrai-se delas.

Escreve `cmake/ia_fontes.cmake`, com duas listas:

    IA_FONTES_CORE        — os .cpp do `core/` do upstream
    IA_FONTES_VIDEO_CORE  — os .cpp do `video_core/` do upstream

Uso:
    python3 tools/extrair_fontes_upstream.py
"""

from __future__ import annotations

import os
import re

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPSTREAM = os.path.join(RAIZ, "vendor", "eden", "src")

ALVO = os.path.join(RAIZ, "cmake", "ia_fontes.cmake")


def indice_dos_ficheiros(raiz_src: str) -> set[str]:
    """Todos os ficheiros de `src/`, como caminhos relativos a `src/`."""
    indice = set()
    for pasta, _, ficheiros in os.walk(raiz_src):
        rel = os.path.relpath(pasta, raiz_src)
        for nome in ficheiros:
            indice.add(nome if rel == "." else os.path.join(rel, nome))
    return indice


def extrair(caminho: str, prefixo: str, indice: set[str], faltantes: list[str]) -> list[str]:
    """As fontes de uma biblioteca, tal como o upstream as lista.

    O `CMakeLists.txt` do upstream lista os ficheiros do `video_core/` e
    tambem os das pastas IRMAAS (o `buffer_cache`, o `macro`, ...). O
    `video_core` esta em `src/video_core` e os irmaos em `src/` — por isso
    o caminho tem de ser resolvido contra os dois.
    """
    if not os.path.exists(caminho):
        return []
    fontes: list[str] = []
    dentro = False
    for linha in open(caminho, encoding="utf-8").read().split("\n"):
        t = linha.strip()
        if not dentro:
            if t.startswith(f"add_library({prefixo} "):
                dentro = True
            continue
        if t == ")":
            break
        if not t or t.startswith("#") or t.startswith("$"):
            continue
        if not t.endswith((".cpp", ".c", ".cc")):
            continue
        if f"{prefixo}/{t}" in indice:
            fontes.append(f"{prefixo}/{t}")
        elif t in indice:
            fontes.append(t)
        else:
            faltantes.append(t)
    return fontes


def main() -> int:
    os.makedirs(os.path.dirname(ALVO), exist_ok=True)
    faltantes: list[str] = []
    indice = indice_dos_ficheiros(UPSTREAM)
    blocos = [
        ("IA_FONTES_CORE",
         extrair(os.path.join(UPSTREAM, "core", "CMakeLists.txt"), "core", indice, faltantes)),
        ("IA_FONTES_VIDEO_CORE",
         extrair(os.path.join(UPSTREAM, "video_core", "CMakeLists.txt"), "video_core",
                 indice, faltantes)),
    ]
    if faltantes:
        print("faltam (trazê-los do upstream):")
        for f in sorted(set(faltantes)):
            print("  ", f)
        with open(os.path.join(RAIZ, "cmake", "ia_fontes_em_falta.txt"), "w",
                  encoding="utf-8") as f:
            f.write("\n".join(sorted(set(faltantes))) + "\n")
    with open(ALVO, "w", encoding="utf-8") as f:
        f.write("# Gerado por tools/extrair_fontes_upstream.py — não editar à mão.\n")
        f.write("# A fonte da verdade é o CMakeLists.txt do upstream.\n\n")
        for nome, fontes in blocos:
            f.write(f"set({nome}\n")
            for fonte in fontes:
                f.write(f"  vendor/eden/src/{fonte}\n")
            f.write(")\n\n")
            print(f"{nome}: {len(fontes)} fontes")
    print(f"escrito em {ALVO}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
