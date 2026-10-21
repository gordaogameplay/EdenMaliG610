#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Traz o `core/` e o `common/` **completos** do Eden upstream.

Porquê
------
O `vendor/eden/core` da casa foi **podado** (`tools/podar_vendor.py`):
ficou só o que a casa usava. Isso servia enquanto a casa tinha o seu
próprio motor.

Mas o motor Maxwell do Eden (`src/video_core/**`) precisa do `core/`
completo — o escalonador, o `KProcess`, a memória, o `GPU`, o
`Host1x` ✗. Sem isso, compilar o `video_core` é impossível ✗.

Este script traz o `core/` e o `common/` completos para
`vendor/eden/`, por cima do que lá está. Os *overrides* da casa
(`tools/overrides/`, aplicados por `tools/aplicar_overrides.py`) são
aplicados **depois** ✓ — por isso o resultado continua a ter as
correções da casa ✓.

Uso
---
    python3 tools/vendorizar_core_completo.py --listar
    python3 tools/vendorizar_core_completo.py --fetch
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

ARVORES = ("src/core/", "src/common/")

IGNORAR = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".webp",
    ".mp4", ".webm", ".ogg", ".wav", ".zip", ".gz", ".xz", ".7z", ".tar",
}


def api(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=60) as resposta:
        return json.load(resposta)


def listar() -> list[tuple[str, int]]:
    url = f"https://api.github.com/repos/{UPSTREAM}/git/trees/{BRANCH}?recursive=1"
    arvore = api(url)
    saida = []
    for entrada in arvore.get("tree", []):
        if entrada.get("type") != "blob":
            continue
        caminho = entrada["path"]
        if not caminho.startswith(ARVORES):
            continue
        if os.path.splitext(caminho)[1].lower() in IGNORAR:
            continue
        saida.append((caminho, entrada.get("size", 0)))
    return saida


def descarregar(caminho: str) -> bool:
    url = f"https://raw.githubusercontent.com/{UPSTREAM}/{BRANCH}/{caminho}"
    destino = os.path.join(DESTINO, caminho)
    os.makedirs(os.path.dirname(destino), exist_ok=True)
    for tentativa in range(3):
        try:
            with urllib.request.urlopen(url, timeout=120) as resposta:
                dados = resposta.read()
            break
        except (urllib.error.URLError, TimeoutError) as erro:
            if tentativa == 2:
                print(f"  FALHOU {caminho}: {erro}", file=sys.stderr)
                return False
    with open(destino, "wb") as f:
        f.write(dados)
    return True


def main() -> int:
    analise = argparse.ArgumentParser(description=__doc__)
    analise.add_argument("--listar", action="store_true")
    analise.add_argument("--fetch", action="store_true")
    args = analise.parse_args()

    ficheiros = listar()
    total = sum(t for _, t in ficheiros)
    print(f"{len(ficheiros)} ficheiros, {total / (1024 * 1024):.1f} MiB")
    if args.listar or not args.fetch:
        return 0

    ok = 0
    falhados = []
    for i, (caminho, _) in enumerate(ficheiros, 1):
        if descarregar(caminho):
            ok += 1
        else:
            falhados.append(caminho)
        if i % 50 == 0:
            print(f"  {i}/{len(ficheiros)}")
    print(f"descarregados: {ok}/{len(ficheiros)}")
    if falhados:
        print("falharam:", *falhados, sep="\n  ", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
