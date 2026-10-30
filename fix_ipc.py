with open('src/ipc_min.cpp', 'r') as f:
    lines = f.readlines()

# Fix lines around 652-653
new_lines = []
i = 0
while i < len(lines):
    line = lines[i]
    if 'eden_send_sync_request returned=%d' in line and line.strip().endswith('%d'):
        # This is the broken line, replace it and skip the next line if it's the continuation
        new_lines.append('            std::fprintf(stderr, "[ipc-probe] eden_send_sync_request returned=%d\\n", ok);\n')
        i += 1
        # Skip the next line if it's the continuation
        if i < len(lines) and '", ok);' in lines[i]:
            i += 1
        continue
    new_lines.append(line)
    i += 1

with open('src/ipc_min.cpp', 'w') as f:
    f.writelines(new_lines)
print('Done')