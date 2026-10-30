with open('src/guest_program.cpp', 'r') as f:
    lines = f.readlines()

# Find the line with "abort-path-patch-image" and insert after the closing braces
new_lines = []
i = 0
while i < len(lines):
    new_lines.append(lines[i])
    # Look for the line after the abort-path-patch-image fprintf
    if 'abort-path-patch-image' in lines[i] and i+3 < len(lines):
        # Check if next few lines contain the closing braces
        if '            }' in lines[i+1] and '        }' in lines[i+2] and '    }' in lines[i+3]:
            # Insert after the '    }' line (i+3)
            # But we need to find where the blank line and next comment are
            pass
    i += 1

# Simpler: find the line "// Patch do epílogo de AbortImpl" and insert before it
new_lines = []
inserted = False
for line in lines:
    if not inserted and '// Patch do epílogo de AbortImpl' in line:
        # Insert probe patch before this line
        new_lines.append('\n')
        new_lines.append('        // PROBE: Patch the cbnz at 0x83aec244 (kCBNZOff = 0x35f244) to BRK #0x3F\n')
        new_lines.append('        // to trap when Hipc2ClientCoreProcessor::Process returns non-zero.\n')
        new_lines.append('        // This lets us inspect the buffer and w0 at the exact abort decision point.\n')
        new_lines.append('        if (sdk_patch_on("probe_cbnz") && std::string(m.name()) == "sdk") {\n')
        new_lines.append('            constexpr std::size_t kCBNZOff = 0x35f244;\n')
        new_lines.append('            if (kCBNZOff + 4 <= m.nso.image.size()) {\n')
        new_lines.append('                std::uint32_t insn = 0;\n')
        new_lines.append('                std::memcpy(&insn, m.nso.image.data() + kCBNZOff, 4);\n')
        new_lines.append('                const std::uint32_t kCBNZ = 0x350001A0u;  // cbnz w0, #0x34\n')
        new_lines.append('                const std::uint32_t kBrkProbe = 0xD42007E0u; // brk #0x3F\n')
        new_lines.append('                if (insn == kCBNZ) {\n')
        new_lines.append('                    std::memcpy(m.nso.image.data() + kCBNZOff,\n')
        new_lines.append('                                &kBrkProbe, 4);\n')
        new_lines.append('                    std::fprintf(stderr,\n')
        new_lines.append('                        "[probe-cbnz-patch] sdk: 0x83aec244 cbnz->brk #0x3F\\n");\n')
        new_lines.append('                }\n')
        new_lines.append('            }\n')
        new_lines.append('        }\n')
        new_lines.append('\n')
        inserted = True
    new_lines.append(line)

with open('src/guest_program.cpp', 'w') as f:
    f.writelines(new_lines)
print('Done' if inserted else 'NOT INSERTED')