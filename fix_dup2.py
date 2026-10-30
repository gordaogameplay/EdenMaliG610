with open('src/ipc_min.cpp', 'rb') as f:
    content = f.read()

# Find and fix the duplicate
old = b'returned=%d\n", ok);\n", ok);\n        }\n'
new = b'returned=%d\n", ok);\n        }\n'

idx = content.find(old)
if idx >= 0:
    print(f'Found at {idx}')
    content = content.replace(old, new)
    with open('src/ipc_min.cpp', 'wb') as f:
        f.write(content)
    print('Fixed!')
else:
    print('Not found')
    # Show context
    idx2 = content.find(b'returned=%d')
    if idx2 >= 0:
        print(repr(content[idx2:idx2+100]))