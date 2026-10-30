with open('src/ipc_min.cpp', 'r') as f:
    content = f.read()

old = '''        bool ok = eden_send_sync_request(eden_->Kernel(), *mem_, handle, ctx.x[1]);
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            std::fprintf(stderr, "[ipc-probe] eden_send_sync_request returned=%d\\n", ok);
        }
        if (ok) {'''

new = '''        bool ok = eden_send_sync_request(eden_->Kernel(), *mem_, handle, ctx.x[1]);
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            std::fprintf(stderr, "[ipc-probe] eden_send_sync_request returned=%d\\n", ok);
            // Dump buffer at SVC return
            std::uint32_t w[8] = {0};
            std::string err;
            if (mem_->read(ctx.x[1], w, sizeof w, err)) {
                std::fprintf(stderr,
                    "[ipc-probe] svc_return addr=0x%llx: %08x %08x %08x %08x %08x %08x %08x %08x\\n",
                    (unsigned long long)ctx.x[1],
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        if (ok) {'''

content = content.replace(old, new)
with open('src/ipc_min.cpp', 'w') as f:
    f.write(content)
print('Done')