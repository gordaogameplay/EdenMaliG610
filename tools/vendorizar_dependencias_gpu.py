#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Traz as dependências de compilação do `video_core` do Eden.

O `video_core` do upstream declara (no seu `CMakeLists.txt`):

    target_link_libraries(video_core PUBLIC
        Vulkan::UtilityHeaders GPUOpen::VulkanMemoryAllocator)

Ou seja, duas dependências — e nenhuma delas é o PanVK ✗ (que é um ICD
carregado em *runtime* ✓ e que o Pi já tem ✓).

Este script traz:
  - KhronosGroup/Vulkan-Headers            -> vendor/third_party/Vulkan-Headers
  - GPUOpen-GameFoundations/VulkanMemoryAllocator
                                           -> vendor/third_party/VulkanMemoryAllocator

E o `shader_recompiler` precisa ainda de:
  - KhronosGroup/SPIRV-Headers
  - KhronosGroup/SPIRV-Tools
  - KhronosGroup/SPIRV-Reflect
(esse é o que converte o GLSL da NVIDIA em SPIR-V ✗ — o `ShaderCache` da
casa é hoje um *placeholder* ✗).

Uso:
    python3 tools/vendorizar_dependencias_gpu.py --listar
    python3 tools/vendorizar_dependencias_gpu.py --fetch
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESTINO = os.path.join(RAIZ, "vendor", "third_party")

# (repositorio, destino) — o branch e' o de omissão de cada um, lido
# da API: chutar as tags e' uma fonte de 404.
REPOS = [
    ("KhronosGroup/Vulkan-Headers", "Vulkan-Headers"),
    ("GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator", "VulkanMemoryAllocator"),
    ("KhronosGroup/SPIRV-Headers", "SPIRV-Headers"),
    ("KhronosGroup/SPIRV-Reflect", "SPIRV-Reflect"),
    ("KhronosGroup/SPIRV-Tools", "SPIRV-Tools"),
]

IGNORAR = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".webp",
    ".mp4", ".webm", ".ogg", ".wav", ".zip", ".gz", ".xz", ".7z", ".tar",
    ".pdf", ".psd", ".ai",
}


def api(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=60) as resposta:
        return json.load(resposta)


def branch_por_omissao(repo: str) -> str:
    return api(f"https://api.github.com/repos/{repo}").get("default_branch", "master")


def listar(repo: str, branch: str) -> list[tuple[str, int]]:
    url = f"https://api.github.com/repos/{repo}/git/trees/{branch}?recursive=1"
    arvore = api(url)
    saida = []
    for entrada in arvore.get("tree", []):
        if entrada.get("type") != "blob":
            continue
        caminho = entrada["path"]
        if caminho.startswith("third_party/") or "/third_party/" in caminho:
            continue
        if os.path.splitext(caminho)[1].lower() in IGNORAR:
            continue
        saida.append((caminho, entrada.get("size", 0)))
    return saida


def descarregar(repo: str, branch: str, caminho: str, destino: str) -> bool:
    url = f"https://raw.githubusercontent.com/{repo}/{branch}/{caminho}"
    alvo = os.path.join(destino, caminho)
    os.makedirs(os.path.dirname(alvo), exist_ok=True)
    for tentativa in range(3):
        try:
            with urllib.request.urlopen(url, timeout=120) as resposta:
                dados = resposta.read()
            break
        except (urllib.error.URLError, TimeoutError) as erro:
            if tentativa == 2:
                print(f"  FALHOU {repo}/{caminho}: {erro}", file=sys.stderr)
                return False
    with open(alvo, "wb") as f:
        f.write(dados)
    return True


def main() -> int:
    analise = argparse.ArgumentParser(description=__doc__)
    analise.add_argument("--listar", action="store_true")
    analise.add_argument("--fetch", action="store_true")
    args = analise.parse_args()

    total_ficheiros = 0
    total_bytes = 0
    for repo, nome in REPOS:
        try:
            branch = branch_por_omissao(repo)
            ficheiros = listar(repo, branch)
        except urllib.error.URLError as erro:
            print(f"{repo}: {erro}", file=sys.stderr)
            continue
        tamanho = sum(t for _, t in ficheiros)
        total_ficheiros += len(ficheiros)
        total_bytes += tamanho
        print(f"{nome:28} {branch:>12}  {len(ficheiros):>5} ficheiros  {tamanho / (1024 * 1024):>7.1f} MiB")
        if args.fetch:
            destino = os.path.join(DESTINO, nome)
            ok = 0
            for i, (caminho, _) in enumerate(ficheiros, 1):
                if descarregar(repo, branch, caminho, destino):
                    ok += 1
                if i % 50 == 0:
                    print(f"    {i}/{len(ficheiros)}")
            print(f"    {nome}: {ok}/{len(ficheiros)}")

    print(f"total: {total_ficheiros} ficheiros, {total_bytes / (1024 * 1024):.1f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
