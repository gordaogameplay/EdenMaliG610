#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Vendoriza o `video_core` do Eden upstream — o motor Maxwell 3D.

Porquê
------
O Eden **não** traduz os comandos Maxwell para Vulkan com uma tabela: ele
**emula a GPU Maxwell** (`src/video_core/engines/maxwell_3d.*`) e executa
os comandos do push buffer, e a tradução para Vulkan está em
`src/video_core/renderer_vulkan/maxwell_to_vk.cpp`.

Reimplementar isso seria reescrever dezenas de milhares de linhas. A
alternativa — e a que este projecto escolhe — é **vendorizar o
`video_core` do upstream** e usá-lo tal como é.

O que é preciso vendorizar
---------------------------
    src/video_core/**          — o motor Maxwell, o puller, o dma_pusher,
                                 o renderer_vulkan, a memória da GPU
    src/shader_recompiler/**   — a conversão NVIDIA GLSL → SPIR-V (o
                                 `ShaderCache::compile` da casa é hoje um
                                 placeholder; o upstream faz a conversão
                                 a sério)

O que a casa já tem
-------------------
    vendor/eden/common/**  e  vendor/eden/core/**  — já vendorizados ✓

Uso
---
    python3 tools/vendorizar_video_core.py            # só a árvore
    python3 tools/vendorizar_video_core.py --fetch    # descarrega os ficheiros

O `--fetch` é o que escreve os ficheiros; a árvore imprime o que
seria trazido (e o tamanho total, que é a informação que interessa
antes de decidir).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

UPSTREAM = "emuall/eden"
BRANCH = "master"
RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESTINO = os.path.join(RAIZ, "vendor", "eden")

# As árvores que se trazem do upstream.
ARVORES = ("src/video_core/", "src/shader_recompiler/")

# Ficheiros que não são código (licenças, imagens, dados) — não são
# necessários para compilar e pesam.
EXTENSOES_IGNORADAS = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".webp",
    ".mp4", ".webm", ".ogg", ".wav",
    ".zip", ".gz", ".xz", ".7z", ".tar",
}


def api(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=60) as resposta:
        return json.load(resposta)


def listar() -> list[tuple[str, int]]:
    """Devolve [(caminho, tamanho)] de tudo o que interessa."""
    url = f"https://api.github.com/repos/{UPSTREAM}/git/trees/{BRANCH}?recursive=1"
    arvore = api(url)
    ficheiros = []
    for entrada in arvore.get("tree", []):
        if entrada.get("type") != "blob":
            continue
        caminho = entrada["path"]
        if not caminho.startswith(ARVORES):
            continue
        if os.path.splitext(caminho)[1].lower() in EXTENSOES_IGNORADAS:
            continue
        ficheiros.append((caminho, entrada.get("size", 0)))
    return ficheiros


def descarregar(caminho: str, destino: str) -> bool:
    url = f"https://raw.githubusercontent.com/{UPSTREAM}/{BRANCH}/{caminho}"
    destino_abs = os.path.join(DESTINO, caminho)
    os.makedirs(os.path.dirname(destino_abs), exist_ok=True)
    for tentativa in range(3):
        try:
            with urllib.request.urlopen(url, timeout=120) as resposta:
                dados = resposta.read()
            break
        except (urllib.error.URLError, TimeoutError) as erro:
            if tentativa == 2:
                print(f"  FALHOU {caminho}: {erro}", file=sys.stderr)
                return False
    with open(destino_abs, "wb") as f:
        f.write(dados)
    return True


def main() -> int:
    analise = argparse.ArgumentParser(description=__doc__)
    analise.add_argument("--fetch", action="store_true",
                          help="descarrega mesmo os ficheiros")
    args = analise.parse_args()

    print(f"árvore do upstream: {UPSTREAM}@{BRANCH}")
    ficheiros = listar()
    if not ficheiros:
        print("nada encontrado — a API mudou?", file=sys.stderr)
        return 1

    total = sum(tamanho for _, tamanho in ficheiros)
    print(f"{len(ficheiros)} ficheiros, {total / (1024 * 1024):.1f} MiB")

    if not args.fetch:
        for caminho, tamanho in sorted(ficheiros)[:40]:
            print(f"  {tamanho:>9}  {caminho}")
        if len(ficheiros) > 40:
            print(f"  ... e mais {len(ficheiros) - 40}")
        print("\n(repetir com --fetch para descarregar)")
        return 0

    ok = 0
    falhados = []
    for i, (caminho, _) in enumerate(ficheiros, 1):
        if descarregar(caminho, destino := caminho):
            ok += 1
        else:
            falhados.append(caminho)
        if i % 25 == 0:
            print(f"  {i}/{len(ficheiros)}")

    print(f"descarregados: {ok}/{len(ficheiros)}")
    if falhados:
        print("falharam:", *falhados, sep="\n  ", file=sys.stderr)
        return 1
    print(f"em {DESTINO}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
