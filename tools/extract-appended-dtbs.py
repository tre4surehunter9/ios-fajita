#!/usr/bin/env python3
"""Extract the DTBs appended to a Qualcomm kernel blob (Image.gz-dtb).

  extract-appended-dtbs.py KERNEL OUTDIR

KERNEL is the `kernel` file from tools/unpack_bootv2.py of a header v0/v1
boot image. Writes OUTDIR/dtb0.dtb, dtb1.dtb, ... and OUTDIR/dtbs (all of them
concatenated, in order, as ABL expects them after Image.gz), and prints each
DTB's model, compatible, qcom,msm-id and qcom,board-id: ABL picks the one
whose ids match the SoC revision and board.
"""
import os, struct, sys, zlib

FDT_MAGIC = 0xd00dfeed


def appended(blob):
    """Bytes after the kernel: after the gzip stream, or after image_size of a raw Image."""
    if blob[:2] == b'\x1f\x8b':
        d = zlib.decompressobj(31)
        d.decompress(blob)
        return d.unused_data
    if blob[56:60] == b'ARM\x64':
        image_size, = struct.unpack_from('<Q', blob, 16)
        return blob[image_size:]
    raise SystemExit('kernel is neither gzip nor an arm64 Image')


def split(data):
    dtbs, off = [], 0
    while off + 40 <= len(data):
        magic, total, _, _, _, version = struct.unpack_from('>6I', data, off)
        if magic != FDT_MAGIC or version < 16 or total < 40 or off + total > len(data):
            break
        dtbs.append(data[off:off + total])
        off += total
    if off != len(data) and any(data[off:]):
        print(f'warning: {len(data) - off} trailing bytes after {len(dtbs)} DTBs', file=sys.stderr)
    return dtbs


def root_props(dtb):
    """Properties of the root node only."""
    struct_off, strings_off = struct.unpack_from('>2I', dtb, 8)
    p, depth, props = struct_off, 0, {}
    while True:
        tok, = struct.unpack_from('>I', dtb, p)
        p += 4
        if tok == 1:		# BEGIN_NODE
            depth += 1
            p = (dtb.index(b'\0', p) + 4) & ~3
            if depth > 1:
                return props
        elif tok == 3:		# PROP
            ln, nameoff = struct.unpack_from('>2I', dtb, p)
            name = dtb[strings_off + nameoff:dtb.index(b'\0', strings_off + nameoff)].decode()
            props[name] = dtb[p + 8:p + 8 + ln]
            p = (p + 8 + ln + 3) & ~3
        elif tok == 4:		# NOP
            continue
        else:
            return props


def show(props, name):
    v = props.get(name)
    if v is None:
        return '-'
    if name in ('model', 'compatible'):
        return ' | '.join(s.decode(errors='replace') for s in v.split(b'\0') if s)
    return ' '.join(hex(x) for x in struct.unpack('>%dI' % (len(v) // 4), v))


def main():
    kernel, out = sys.argv[1], sys.argv[2]
    dtbs = split(appended(open(kernel, 'rb').read()))
    if not dtbs:
        raise SystemExit('no appended DTBs found')
    os.makedirs(out, exist_ok=True)
    for i, d in enumerate(dtbs):
        open(os.path.join(out, f'dtb{i}.dtb'), 'wb').write(d)
        props = root_props(d)
        print(f'dtb{i}: {len(d)} bytes')
        for name in ('model', 'compatible', 'qcom,msm-id', 'qcom,board-id'):
            print(f'  {name}: {show(props, name)}')
    open(os.path.join(out, 'dtbs'), 'wb').write(b''.join(dtbs))
    print(f'{out}/dtbs: {len(dtbs)} DTBs')


if __name__ == '__main__':
    main()
