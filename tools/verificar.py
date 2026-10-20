#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Conta o que é nosso e o que é do Eden, e mede o emulador que ficou de fora.

Este script existe por causa de uma coisa que aconteceu três vezes nesta
costura: uma edição que não aplicou, ou que aplicou a meio, e que só se
manifestou muito mais tarde, num erro de compilação que não a apontava.

Regra: **qualquer alteração que muda código tem de ser seguida de uma
medição, e a medição tem de ser feita no ficheiro do Pi**, que é onde o
projecto é compilado e onde o jogo corre. Um script que conta localmente e
nunca confirma não é uma medição, é uma intenção.

Uso:
    verificar.py            -- o resumo
    verificar.py --ponte    -- só as três classes da ponte
    verificar.py --cortes   -- os PONTE/CORTE dentro do vendorizado
"""
import os
import re
import subprocess
import sys

ROOT = os.path.expanduser("~/EdenMaliIA")
VENDOR = os.path.join(ROOT, "vendor", "eden")


def contar(ficheiros):
    n = linhas = 0
    for f in ficheiros:
        n += 1
        with open(f, encoding="utf-8", errors="replace") as fh:
            linhas += fh.read().count("\n")
    return n, linhas


def arvore(subp):
    d = os.path.join(VENDOR, subp)
    out = []
    for dirpath, _, files in os.walk(d):
        for f in files:
            if f.endswith((".h", ".cpp")):
                out.append(os.path.join(dirpath, f))
    return out


def ponte():
    """As três classes que são nossas, e as três substituições que fazem."""
    ficheiros = [
        "vendor/eden/core/hle/kernel/k_kernel_core_bridge.h",
        "vendor/eden/core/hle/kernel/k_process_bridge.h",
        "vendor/eden/core/system_ref.h",
        "vendor/eden/core/memory_bridge.h",
    ]
    return [(f, os.path.join(ROOT, f)) for f in ficheiros if os.path.exists(os.path.join(ROOT, f))]


def cortes():
    """Todos os `PONTE/CORTE` dentro do código vendorizado.

    Um corte que não está escrito no código é um corte que não aconteceu —
    quem lê o ficheiro depois de seis meses não tem como saber. Por isso se
    conta, e o número é o que o README tem de bater.
    """
    achados = []
    for f in arvore("."):
        with open(f, encoding="utf-8", errors="replace") as fh:
            for i, linha in enumerate(fh, 1):
                if "PONTE/CORTE" in linha or "PONTE:" in linha:
                    achados.append((os.path.relpath(f, ROOT), i, linha.strip()[:70]))
    return achados


def emulador():
    """Os cabeçalhos do emulador que NÃO estão no vendor.

    Lista negra escrita à mão, a partir do que foi visto ser arrastado: page
    tables, escalonador, memórias de dispositivo, VFS, NCA. Se algum destes
    aparecer no vendor, o script diz — e a resposta tem de ser "porquê".
    """
    proibidos = [
        "k_page_table_base.h", "k_process_page_table.h", "k_page_table_manager.h",
        "k_scheduler.h", "k_priority_queue.h", "global_scheduler_context.h",
        "k_process.cpp", "k_thread.cpp", "k_scheduler.cpp", "cpu_manager.h",
        "k_device_address_space.h", "k_device_memory.h", "page_table.h",
        "k_dynamic_page_manager.h", "k_memory_block_manager.h", "fs_util.h",
        "audio_out.h", "video_core.h", "nca.h",
    ]
    achados = []
    for f in arvore("."):
        b = os.path.basename(f)
        if b in proibidos:
            achados.append(os.path.relpath(f, ROOT))
    return sorted(achados)


def sintaxe():
    """Compila cada `.cpp` vendorizado sozinho. É o teste que diz a verdade."""
    mau = []
    for f in sorted(arvore(".")):
        if not f.endswith(".cpp"):
            continue
        cmd = ["g++", "-std=c++20", "-I" + ROOT, "-I" + os.path.join(ROOT, "include"),
               "-I" + os.path.join(ROOT, "vendor", "third_party"),
               "-DANKERL_UNORDERED_DENSE_STD_MODULE=1", "-fsyntax-only", f]
        r = subprocess.run(cmd, capture_output=True, text=True)
        n = r.stderr.count("error:")
        if n:
            mau.append((os.path.relpath(f, ROOT), n))
    return mau


def main():
    if "--ponte" in sys.argv:
        for nome, caminho in ponte():
            n, l = contar([caminho])
            print("%-58s %5d linhas" % (nome, l))
        return 0

    if "--cortes" in sys.argv:
        c = cortes()
        for f, i, l in c:
            print("%s:%d  %s" % (f, i, l))
        print("\n%d cortes marcados no código" % len(c))
        return 0

    vh = arvore(".")
    vn, vl = contar(vh)
    ph, pl = contar([c for _, c in ponte()])
    print("─── Eden vendorizado ───")
    print("  %d ficheiros, %d linhas" % (vn, vl))
    for sub in ("core/hle/kernel", "core/hle/service", "core/hle/service/sm", "common"):
        n, l = contar(arvore(sub))
        if n:
            print("    %-22s %3d ficheiros %6d linhas" % (sub, n, l))
    print("\n─── a ponte (nosso) ───")
    print("  %d ficheiros, %d linhas" % (ph, pl))
    for nome, _ in ponte():
        n, l = contar([os.path.join(ROOT, nome)])
        print("    %-54s %5d" % (nome, l))

    c = cortes()
    print("\n─── cortes dentro do vendorizado ───")
    print("  %d sitios marcados com PONTE/CORTE" % len(c))

    e = emulador()
    print("\n─── emulador dentro do vendor ───")
    if e:
        print("  PRESENTE (isto e' um erro):")
        for x in e:
            print("    !! %s" % x)
    else:
        print("  nenhum. A lista negra tem %d entradas e nenhuma apanhou." % 19)

    print("\n─── sintaxe ───")
    mau = sintaxe()
    if mau:
        for f, n in mau:
            print("  %-56s %d erros" % (f, n))
        print("  %d de %d .cpp nao compilam" % (len(mau), sum(1 for f in vh if f.endswith(".cpp"))))
    else:
        print("  todos os .cpp vendorizados compilam")
    return 0


if __name__ == "__main__":
    sys.exit(main())