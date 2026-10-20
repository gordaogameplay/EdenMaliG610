#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mede o fecho de headers vendorizado e diz o que é peso morto.

Porquê existe: o `vendorizar.py` copia um `.cpp` e segue os `#include` dele.
Isso garante que nada falta, mas nada garante que não **sobe**. Na primeira
vez que o corremos ele trouxe `k_page_table_base.h`, `k_scheduler.h` e afins —
o emulador, em forma de cabeçalho, que nunca chega a ser compilado mas fica
no repositório e no `README.md` a dizer que está lá.

A regra que este script impõe é a mesma do projecto: **não excluir uma
hipótese sem a testar**. Um header é removível se, e só se, **nenhuma fonte
que o build realmente compila** o alcança.

## O defeito que esta versão corrige

A primeira versão media a partir de uma lista de raízes escrita à mão: os
`.cpp` de `kernel/` e de `service/`, mais a ponte. **Não** incluía os `.cpp`
que estão na lista do build, e por isso apanhou `common/assert.cpp` como
órfão e apagou-o — e o build parou a ligar com `undefined reference to
AssertFailSoftImpl`.

Um podador que mede a partir de outra lista **apaga o que a build precisa**,
e pior: apaga-o com a confiança de quem fez a medição. Por isso as raízes
agora são lidas do `CMakeLists.txt` — a mesma lista, palavra por palavra. Se
tiver de ser mudada à mão, é porque alguém mudou o build à mão, e o podador
vê.

Uso:
    podar_vendor.py              imprime o que é removível
    podar_vendor.py --podar      apaga
    podar_vendor.py --raiz X     acrescenta uma raiz extra (para testes)
"""
import os
import re
import sys

ROOT = os.path.expanduser("~/EdenMaliG610")
VENDOR = os.path.join(ROOT, "vendor", "eden")
INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def lista_do_build():
    """As fontes vendorizadas que o build compila, lidas do CMakeLists.

    Ler a lista em vez de a reescrever aqui é o ponto inteiro: as duas listas
    não podem divergir, porque não há duas — há uma, escrita num sítio, e este
    script lê-a.
    """
    p = os.path.join(ROOT, "CMakeLists.txt")
    if not os.path.exists(p):
        return []
    with open(p, encoding="utf-8") as f:
        texto = f.read()
    m = re.search(r"set\(IA_EDEN_SOURCES\n(.*?)\n\)", texto, re.S)
    if not m:
        return []
    return [ln.strip() for ln in m.group(1).split("\n")
            if ln.strip().endswith(".cpp")]


def raizes_de_include():
    return [ROOT, os.path.join(ROOT, "include"), os.path.join(ROOT, "vendor", "third_party")]


def incluir_de(fich, visitados, raizes):
    """Segue os includes de `fich`, em profundidade."""
    if fich in visitados or not os.path.exists(fich):
        return
    visitados.add(fich)
    try:
        with open(fich, encoding="utf-8", errors="replace") as f:
            linhas = f.readlines()
    except OSError:
        return
    for linha in linhas:
        m = INC_RE.match(linha)
        if not m:
            continue
        alvo = m.group(1)
        if alvo.startswith("vendor/eden/"):
            incluir_de(os.path.join(ROOT, alvo), visitados, raizes)
        elif alvo.startswith(".."):
            seguir = os.path.normpath(os.path.join(os.path.dirname(fich), alvo))
            incluir_de(seguir, visitados, raizes)
        else:
            for r in raizes:
                cand = os.path.join(r, alvo)
                if os.path.exists(cand):
                    incluir_de(cand, visitados, raizes)
                    break


def alcancaveis(extra=()):
    raizes = raizes_de_include()
    visitados = set()

    # 1) Tudo o que o build compila de fora do vendor: as nossas fontes, que
    #    incluem as cabeçalhos da ponte.
    for rel in ("src/eden_bridge.cpp", "src/eden_log.cpp",
                "src/eden_synchronization_object.cpp",
                "include/g610/eden_bridge.hpp", "include/g610/svc_dispatcher.hpp",
                "include/g610/ipc_min.hpp") + tuple(extra):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            incluir_de(p, visitados, raizes)

    # 2) A lista do build, lida do CMakeLists.
    for rel in lista_do_build():
        incluir_de(os.path.join(ROOT, rel), visitados, raizes)

    # 3) As quatro peças da ponte, mesmo que nenhuma fonte as inclua ainda.
    for rel in ("vendor/eden/core/system_ref.h",
                "vendor/eden/core/memory_bridge.h",
                "vendor/eden/core/hle/kernel/k_kernel_core_bridge.h",
                "vendor/eden/core/hle/kernel/k_process_bridge.h"):
        incluir_de(os.path.join(ROOT, rel), visitados, raizes)
    return visitados


def todos():
    out = set()
    for dirpath, _, files in os.walk(VENDOR):
        for p in files:
            if p.endswith((".h", ".cpp")):
                out.add(os.path.join(dirpath, p))
    return out


def main():
    extra = []
    if "--raiz" in sys.argv:
        i = sys.argv.index("--raiz")
        extra = tuple(sys.argv[i + 1:])
    alc = alcancaveis(extra)
    orfaos = sorted(todos() - alc)
    rel = lambda p: os.path.relpath(p, ROOT)

    if "--podar" in sys.argv:
        for p in orfaos:
            os.remove(p)
        for dirpath, _, _ in sorted(os.walk(VENDOR), reverse=True):
            if not os.listdir(dirpath) and dirpath != VENDOR:
                os.rmdir(dirpath)
        print("Podados: %d ficheiros, de %d alcancados" % (len(orfaos), len(alc)))
        for p in orfaos:
            print("  - %s" % rel(p))
        return 0

    print("Raizes: %d .cpp da lista do build + as nossas fontes" % len(lista_do_build()))
    print("Alcancados: %d" % len(alc))
    print("Orfaos:     %d\n" % len(orfaos))
    linhas = 0
    for p in orfaos:
        with open(p, encoding="utf-8", errors="replace") as f:
            n = f.read().count("\n")
        linhas += n
        print("  %-62s %5d" % (rel(p), n))
    print("\nTotal em orfaos: %d linhas" % linhas)
    return 0


if __name__ == "__main__":
    sys.exit(main())