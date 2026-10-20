#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Corta a page table de `k_server_session.cpp`.

Este ficheiro é vendorizado do Eden, com a SPDX original intacta. Os cortes
ficam marcados no próprio código com `PONTE/CORTE` e a razão ao lado, para que
seja possível distinguir o que é do Eden do que é nosso — e para que um
leitor não conclua que o Eden não usa page tables.

Ver `vendor/eden/core/memory_bridge.h` para a peça do lado do destino: a
`Core::Memory::Memory` nossa tem o `GetPointer` que aqui substitui as três
traduções que o Eden faz.
"""
import sys

P = "vendor/eden/core/hle/kernel/k_server_session.cpp"

TROCAS = [
    # ── CleanupServerHandles ────────────────────────────────────────────────
    (
        """    u32* msg_ptr;
    if (message) {
        msg_ptr = kernel.System().DeviceMemory().GetPointer<u32>(message_paddr);
    } else {""",
        """    u32* msg_ptr;
    if (message) {
        // PONTE/CORTE: o Eden vai buscar a DeviceMemory pelo endereco fisico
        // (`message_paddr`). Aqui nao ha DeviceMemory nem endereco fisico: o
        // `message` e' um endereco do guest, e a reserva e' um `mmap` de gama
        // fixa feito pelo motor -- o endereco do guest E' o endereco do host.
        // Ver `memory_bridge.h`, `GetPointer`.
        msg_ptr = GetCurrentMemory(kernel).GetPointer<u32>(message);
    } else {""",
    ),
    # ── ReceiveRequest: as duas page tables ────────────────────────────────
    (
        """    auto& dst_page_table = dst_process.GetPageTable();
    auto& src_page_table = src_process.GetPageTable();""",
        """    // PONTE/CORTE: no Eden estas duas sao page tables, e traduzem o endereco
    // virtual de cada processo para memoria fisica.
    //
    // Aqui nao ha page table nem memoria fisica: a reserva do guest e' um
    // `mmap` de gama fixa, e **o motor tem um so processo** -- `src_process` e
    // `dst_process` sao o mesmo objecto. A memoria do processo de origem e' a
    // memoria do processo de destino, e `GetCurrentMemory(kernel)` e' a
    // resposta certa para as duas.
    //
    // Isto nao e' perda de funcao. Um "user buffer" do guest neste motor e'
    // um endereco dentro da mesma reserva, e traduzi-lo seria a identidade.
    // (void)dst_process; (void)src_process;  // mantem o par usado""",
    ),
    (
        """    if (dst_message_buffer) {
        dst_msg_ptr = kernel.System().DeviceMemory().GetPointer<u32>(dst_message_paddr);
        dst_user = true;
    } else {
        dst_msg_ptr = dst_page_table.GetMemory().GetPointer<u32>(dst_thread.GetTlsAddress());""",
        """    if (dst_message_buffer) {
        // PONTE/CORTE: ver `CleanupServerHandles`. O `dst_message_buffer` e'
        // um endereco do guest e traduzi-lo e' a identidade.
        dst_msg_ptr = GetCurrentMemory(kernel).GetPointer<u32>(dst_message_buffer);
        dst_user = true;
    } else {
        dst_msg_ptr = GetCurrentMemory(kernel).GetPointer<u32>(dst_thread.GetTlsAddress());""",
    ),
    (
        """        src_msg_ptr = src_page_table.GetMemory().GetPointer<u32>(src_message_buffer);
        src_user = true;
    } else {
        src_msg_ptr = src_page_table.GetMemory().GetPointer<u32>(src_thread.GetTlsAddress());""",
        """        src_msg_ptr = GetCurrentMemory(kernel).GetPointer<u32>(src_message_buffer);
        src_user = true;
    } else {
        src_msg_ptr = GetCurrentMemory(kernel).GetPointer<u32>(src_thread.GetTlsAddress());""",
    ),
]


def main():
    s = open(P, encoding="utf-8").read()
    for velho, novo in TROCAS:
        if velho not in s:
            sys.exit("NAO ENCONTRADO:\n" + velho[:160])
        s = s.replace(velho, novo, 1)
    open(P, "w", encoding="utf-8").write(s)
    print("%d cortes aplicados em %s" % (len(TROCAS), P))


if __name__ == "__main__":
    main()