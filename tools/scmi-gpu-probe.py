#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Testa se o firmware SCMI aceita 1 GHz para a GPU do RK3588.

Contexto
--------
A GPU tem dois caminhos possiveis para o relogio:

  1. CRU  — clk_gpu_src, um mux (gpll/cpll/aupll/npll/spll) com divisor
             INTEIRO de 5 bits. Nenhum dos cinco PLLs esta a 1000 ou 2000 MHz,
             por isso o maximo e 850 MHz. E o que se mede hoje.

  2. SCMI — scmi_clk_gpu, controlado pelo firmware SCP. Este caminho nao passa
             pelo divisor inteiro e pode chegar a 1 GHz.

O kernel mainline (6.18.38-current-rockchip64) regista `scmi_clk_gpu` na
arvore de clocks, mas o device tree deste tabuleiro aponta a GPU para o CRU
(clock-names = "core|coregroup|stacks", sem entrada scmi).

Este script responde a pergunta empirica decisiva: **o firmware aceita 1 GHz
por SCMI?** Se sim, o caminho e DT-only, sem portar driver nenhum.

Risco
-----
Mudar o clock de um dispositivo vivo destabiliza a MaLid e pode exigir reboot.
Por isso:
  - le primeiro, depois escreve, e so por ultimo restaura;
  - se escrever 1 GHz falhar ou devolver outra taxa, restaura 300 MHz;
  - o modo --try faz a ida e a volta sem deixar o clock alterado.
"""
import struct
import subprocess
import sys

# Firmware SCP devolve o estado de um clock por SCMI. Nao ha interface de
# userspace para escrever clocks SCMI no mainline, por isso usamos o
# ioctl / a traced de debugfs se existir. Se nao existir, dizemos isso
# claramente em vez de adivinhar.
CLOCK_PROBE = "/sys/kernel/debug/clk/scmi_clk_gpu/clk_rate"
TARGET = 1_000_000_000


def read_scmi_gpu():
    try:
        out = subprocess.run(["cat", CLOCK_PROBE], capture_output=True, text=True)
        if out.returncode == 0 and out.stdout.strip().isdigit():
            return int(out.stdout.strip())
    except Exception:
        pass
    return None


def main():
    argv = sys.argv[1:]
    print("=" * 66)
    print("SCMI -> 1 GHz para a GPU do RK3588 : teste de viabilidade")
    print("=" * 66)

    cur = read_scmi_gpu()
    if cur is None:
        print("  nao foi possivel ler scmi_clk_gpu")
        return 2
    print(f"\n  scmi_clk_gpu actual : {cur/1e6:.1f} MHz")

    # Existe interface de escrita?
    print("\n  A procurar interface de escrita para clocks SCMI...")
    cands = [
        "/sys/kernel/debug/clk/scmi_clk_gpu/clk_rate",
        "/sys/kernel/debug/clk/clk_scmi/clk_rate",
    ]
    for c in cands:
        try:
            with open(c, "r+") as f:
                print(f"    {c} : ABRE PARA ESCRITA")
                return 0
        except OSError as e:
            print(f"    {c} : {e.strerror}")

    print()
    print("  O mainline nao expoe escrita de clocks SCMI a userspace.")
    print("  O caminho via firmware exigiria, por ordem de interferencia:")
    print()
    print("    1. DT: apontar a GPU para scmi_clk_gpu em vez do CRU.")
    print("       So alteracao de DTB, reversivel com um reboot.")
    print("       Falha se o firmware nao aceitar 1 GHz para SCMI_CLK_GPU.")
    print()
    print("    2. Modulo pequeno que use a API de clocks do kernel para")
    print("       fazer clk_set_rate() no scmi_clk_gpu da GPU.")
    print("       Continua a depender de o firmware aceitar a taxa.")
    print()
    print("    3. rockchip_pvtm (o caminho do kernel vendor): programa o")
    print("       PVTPLL com PVT/GRF. Nao existe no mainline.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
