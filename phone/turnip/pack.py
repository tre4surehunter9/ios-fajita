#!/usr/bin/env python3
"""pack.py DYLIB OUT: OUT = 8-byte little-endian size + raw DEFLATE of DYLIB (COMPRESSION_ZLIB in vktest.c)."""
import struct, sys, zlib
data = open(sys.argv[1], 'rb').read()
c = zlib.compressobj(9, zlib.DEFLATED, -15)
packed = c.compress(data) + c.flush()
open(sys.argv[2], 'wb').write(struct.pack('<Q', len(data)) + packed)
print(f'{sys.argv[1]}: {len(data)} -> {len(packed)} bytes')
