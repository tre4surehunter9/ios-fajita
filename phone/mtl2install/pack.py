#!/usr/bin/env python3
"""pack.py BUNDLE_DIR OUT: OUT = u64 size + raw DEFLATE of {"SWAR", u32 count, entries} (see mtl2install.c)."""
import os, struct, sys, zlib
root = sys.argv[1]
entries = []
for dirpath, _, files in os.walk(root):
    for name in sorted(files):
        full = os.path.join(dirpath, name)
        rel = os.path.relpath(full, root).encode()
        data = open(full, 'rb').read()
        entries.append(struct.pack('<H', len(rel)) + rel + struct.pack('<IQ', os.stat(full).st_mode & 0o777, len(data)) + data)
        print(f'  {rel.decode()}: {len(data)} bytes')
ar = b'SWAR' + struct.pack('<I', len(entries)) + b''.join(entries)
c = zlib.compressobj(9, zlib.DEFLATED, -15)
z = c.compress(ar) + c.flush()
open(sys.argv[2], 'wb').write(struct.pack('<Q', len(ar)) + z)
print(f'{root}: {len(ar)} -> {len(z)} bytes')
