#!/usr/bin/env python3
"""Recover the kernel console ring from a Qualcomm RAM dump (Sahara 900e mode).

  sweet-ramlog.py KERNEL PAYLOAD DUMPFILE DUMP_BASE_PA [OUT.txt]

KERNEL is the Mach-O the phone ran, PAYLOAD the shim payload it was packed
into (for virt_base/phys_base), DUMPFILE one RAM segment saved by
`qdl ramdump`, DUMP_BASE_PA the physical address that segment starts at.
The kernel loads unslid at phys_base, so a symbol's physical address is
va - virt_base + phys_base. sweet_log_ring (bsd/dev/sweet_ufs.c, 4 MiB)
holds every console byte; sweet_log_total counts them, so the newest
byte n is at ring[n % size].
"""
import struct, sys

RING_SIZE = 4 << 20


def symbols(kernel, names):
    b = open(kernel, 'rb').read()
    ncmds, = struct.unpack_from('<I', b, 16)
    off, found = 32, {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', b, off)
        if cmd == 0x2:	# LC_SYMTAB
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', b, off + 8)
            for i in range(nsyms):
                nx, _, _, _, val = struct.unpack_from('<IBBHQ', b, symoff + 16 * i)
                name = b[stroff + nx:b.index(b'\0', stroff + nx)].decode()
                if name in names:
                    found[name] = val
        off += size
    return found


def main():
    kernel, payload, dump, base = sys.argv[1:5]
    out = sys.argv[5] if len(sys.argv) > 5 else None
    base = int(base, 0)
    p = open(payload, 'rb').read(0x48)
    virt_base, = struct.unpack_from('<Q', p, 0x10)
    phys_base, = struct.unpack_from('<Q', p, 0x38)
    s = symbols(kernel, {'_sweet_log_ring', '_sweet_log_total'})
    pa = lambda va: va - virt_base + phys_base
    with open(dump, 'rb') as f:
        def read(addr, n):
            f.seek(addr - base)
            data = f.read(n)
            if len(data) != n:
                raise SystemExit(f'PA {addr:#x}+{n:#x} is outside this dump segment')
            return data
        total, = struct.unpack('<Q', read(pa(s['_sweet_log_total']), 8))
        ring = read(pa(s['_sweet_log_ring']), RING_SIZE)
    print(f'sweet_log_total = {total} bytes (ring PA {pa(s["_sweet_log_ring"]):#x})', file=sys.stderr)
    if total <= RING_SIZE:
        text = ring[:total]
    else:
        start = total % RING_SIZE
        text = ring[start:] + ring[:start]
    if out:
        open(out, 'wb').write(text)
    else:
        sys.stdout.buffer.write(text)


if __name__ == '__main__':
    main()
