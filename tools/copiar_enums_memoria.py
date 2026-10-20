#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-
"""Copia os tres enums de memoria do Eden para dentro da ponte, **no sitio**.

Este script ja foi executado: os enums estao escritos em
`vendor/eden/core/hle/kernel/k_page_table_bridge.h` e o ficheiro e' o
canonico. O script fica por causa da **procedencia** — e porque voltar a
rodar uma geracao é pior do que o que se ganha.

## O que se passou, para não se repetir

A primeira versao inseria os enums por um marcador e o build corria o script.
Dava em que cada `tar xzf` do ficheiro trazia o marcador de volta e os enums
desapareciam, e a correcao era sempre a mesma: extrair, correr o script,
compilar. **Tres vezes seguidas, na mesma tarde.**

A versao que fica tem os enums **escritos no ficheiro**. O script passa a ser
uma forma de *auditar* a copia, nao de a gerar: `copiar_enums_memoria.py
--verificar` diz se o que esta no ficheiro bate com o Eden, e nao muda nada.

A regra que sai: **um ficheiro que o build depende tem de estar escrito no
ficheiro.** Gerar durante o build obriga a ter o gerador no caminho certo do
build, e esse caminho foi o que falhou tres vezes.
"""
import re
import sys

ORIGEM = "/tmp/kmb.h"          # copiado de ~/eden/src/core/hle/kernel/k_memory_block.h
DESTINO = "vendor/eden/core/hle/kernel/k_page_table_bridge.h"
NOMES = ("KMemoryState", "KMemoryPermission", "KMemoryAttribute")

CABECALHO = """// ── os enums, copiados do Eden ──────────────────────────────────────
//
// De `core/hle/kernel/k_memory_block.h`, **inteiros e com os valores tal e
// qual**. Este bloco foi escrito por `tools/copiar_enums_memoria.py` e é
// canónico: o build não o regenera.
//
// Ver o `docstring` do script para o que aconteceu quando o build o gerava.
//
// Tudo o resto daquele ficheiro (o gestor de blocos, a árvore vermelha-
// preta, o alinhamento) fica de fora: não é preciso para copiar memória, e é
// o que arrastaria o emulador.
"""


def extrair(texto, nome):
    m = re.search(r"enum class %s\s*:[^;{]*\{" % re.escape(nome), texto)
    if not m:
        sys.exit("enum %s nao encontrado" % nome)
    i = m.start()
    j = texto.index("};", m.end()) + 2
    bloco = texto[i:j]
    k = j
    while k < len(texto) and texto[k] in "\n\r":
        k += 1
    if texto.startswith("DECLARE_ENUM_FLAG_OPERATORS(%s)" % nome, k):
        bloco += "\n" + texto[k:texto.index(";", k) + 1]
    return bloco


def verificar():
    """Compara o que está no ficheiro com o que está no Eden."""
    destino = open(DESTINO, encoding="utf-8").read()
    origem = open(ORIGEM, encoding="utf-8").read()
    mau = 0
    for nome in NOMES:
        bloco = extrair(origem, nome)
        if bloco not in destino:
            print("DIVERGE: %s — o Eden mudou e o ficheiro nao" % nome)
            mau += 1
        else:
            print("ok: %s" % nome)
    return 1 if mau else 0


def escrever():
    origem = open(ORIGEM, encoding="utf-8").read()
    destino = open(DESTINO, encoding="utf-8").read()
    destino = re.sub(r"// ── os enums, copiados do Eden ─+.*?(?=\n// O destino|\nclass KPageTableBase)",
                     CABECALHO, destino, flags=re.S)
    partes = [CABECALHO]
    for nome in NOMES:
        partes.append(extrair(origem, nome))
        partes.append("")
    bloco = "\n".join(partes)
    # inserido imediatamente antes de `class KPageTableBase`
    destino = destino.replace("class KPageTableBase {", bloco + "\nclass KPageTableBase {", 1)
    open(DESTINO, "w", encoding="utf-8").write(destino)
    print("escritos: %s" % ", ".join(NOMES))


if __name__ == "__main__":
    sys.exit(verificar() if "--verificar" in sys.argv else escrever())