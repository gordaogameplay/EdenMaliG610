with open('src/ipc_min.cpp', 'rb') as f:
    content = f.read()

# Find and fix the duplicate
idx = content.find(b'returned=%d\n", ok);\n        }\n", ok);')
if idx >= 0:
    print(f'Found duplicate at {idx}')
    content = content.replace(
        b'returned=%d\n", ok);\n        }\n", ok);\n',
        b'returned=%d\n", ok);\n        }\n'
    )
    with open('src/ipc_min.cpp', 'wb') as f:
        f.write(content)
    print('Fixed duplicate')
else:
    print('Pattern not found')
    for search in [b'returned=%d', b'ok);\n        }\n"']:
        idx2 = content.find(search)
        if idx2 >= 0:
            print(f'Found {search} at {idx2}: {repr(content[idx2:idx2+80])}')