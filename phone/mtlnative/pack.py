#!/usr/bin/env python3
"""pack.py OUT FILE...: for each FILE a 72-byte header (name[56], raw size, packed size), then its raw DEFLATE data."""
import os, struct, sys, zlib
with open(sys.argv[1], 'wb') as out:
    for path in sys.argv[2:]:
        data = open(path, 'rb').read()
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = c.compress(data) + c.flush()
        out.write(struct.pack('<56sQQ', os.path.basename(path).encode(), len(data), len(packed)) + packed)
        print(f'{path}: {len(data)} -> {len(packed)} bytes')
