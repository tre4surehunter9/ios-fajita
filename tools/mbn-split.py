#!/usr/bin/env python3
"""Split a squashed Qualcomm firmware image (NAME.mbn, one ELF) into NAME.mdt + NAME.bNN.

  mbn-split.py IN.mbn OUTDIR [NAME]     NAME defaults to IN's base name ("modem")

The reverse of linux-msm/pil-squasher. NAME.mdt = ELF header + program headers + the hash segment
(the metadata TrustZone checks); NAME.bNN = segment NN's file bytes (none for empty segments).
phone/wifi/wland.c loads the modem from this layout. Also prints the segments (type, flags, paddr,
sizes) so their addresses can be checked against the modem region.
"""
import os, struct, sys


def main(a):
    if len(a) not in (3, 4):
        raise SystemExit(__doc__)
    data = open(a[1], 'rb').read()
    name = a[3] if len(a) == 4 else os.path.splitext(os.path.basename(a[1]))[0]
    if data[:4] != b'\x7fELF' or data[4] != 1:
        raise SystemExit('not a 32-bit ELF')
    phoff, = struct.unpack_from('<I', data, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
    if phentsize != 32:
        raise SystemExit(f'unexpected program header size {phentsize}')
    ph = [struct.unpack_from('<8I', data, phoff + 32 * i) for i in range(phnum)]
    hdr_end = phoff + 32 * phnum
    hash_i = next((i for i, p in enumerate(ph) if (p[6] >> 24) & 7 == 2), None)
    if hash_i is None:
        raise SystemExit('no hash segment')
    os.makedirs(a[2], exist_ok=True)
    h = ph[hash_i]
    mdt = bytearray(data[:hdr_end])
    if h[1] > len(mdt):
        mdt += bytes(h[1] - len(mdt))
    mdt[h[1]:h[1] + h[4]] = data[h[1]:h[1] + h[4]]
    open(os.path.join(a[2], name + '.mdt'), 'wb').write(mdt)
    lo = min((p[3] for p in ph if p[0] == 1 and p[5]), default=0)
    hi = max((p[3] + p[5] for p in ph if p[0] == 1 and p[5]), default=0)
    print(f'{name}.mdt: {len(mdt)} bytes, {phnum} segments, hash = {hash_i}, '
          f'loadable paddr {lo:#x}..{hi:#x} ({(hi - lo) >> 20} MiB)')
    for i, (ptype, off, vaddr, paddr, filesz, memsz, flags, align) in enumerate(ph):
        if filesz:
            if off + filesz > len(data):
                raise SystemExit(f'segment {i} runs past the end of the file')
            open(os.path.join(a[2], f'{name}.b{i:02d}'), 'wb').write(data[off:off + filesz])
        print(f'  {i:2d} type {ptype} flags {flags:#010x} paddr {paddr:#010x} file {filesz:#9x} mem {memsz:#9x}')


if __name__ == '__main__':
    main(sys.argv)
