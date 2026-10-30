with open('src/guest_program.cpp', 'r') as f:
    content = f.read()

marker = '''                        std::fprintf(stderr,
                        "[abort-path-patch-image] sdk: 0x83aec8c0 adrp->add sp,#0xb0; 0x83aec8d4 bl->ret\\n");
                }
            }
        }

        // Patch do epílogo de AbortImpl'''

new_block = marker + '''

        // PROBE: Patch the cbnz at 0x83aec244 (kCBNZOff = 0x35f244) to BRK #0x3F
        // to trap when Hipc2ClientCoreProcessor::Process returns non-zero.
        // This lets us inspect the buffer and w0 at the exact abort decision point.
        if (sdk_patch_on("probe_cbnz") && std::string(m.name()) == "sdk") {
            constexpr std::size_t kCBNZOff = 0x35f244;
            if (kCBNZOff + 4 <= m.nso.image.size()) {
                std::uint32_t insn = 0;
                std::memcpy(&insn, m.nso.image.data() + kCBNZOff, 4);
                const std::uint32_t kCBNZ = 0x350001A0u;  // cbnz w0, #0x34
                const std::uint32_t kBrkProbe = 0xD42007E0u; // brk #0x3F
                if (insn == kCBNZ) {
                    std::memcpy(m.nso.image.data() + kCBNZOff,
                                &kBrkProbe, 4);
                    std::fprintf(stderr,
                        "[probe-cbnz-patch] sdk: 0x83aec244 cbnz->brk #0x3F\\n");
                }
            }
        }

        // Patch do epílogo de AbortImpl'''

content = content.replace(marker, new_block)
with open('src/guest_program.cpp', 'w') as f:
    f.write(content)
print('Done')