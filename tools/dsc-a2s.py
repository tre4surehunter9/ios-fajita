#!/usr/bin/env python3
"""Resolve unslid shared-cache addresses to image + nearest local symbol.

  dsc-a2s.py CACHE_DIR SLIDE ADDR...

CACHE_DIR holds dyld_shared_cache_arm64 and its .symbols file (iOS 16 split
cache). ADDR values are as printed by the sampler/crash log (slid); SLIDE is
the process's shared-cache slide from the same log ("slide 0x...").
Addresses outside the cache are printed as-is. A small stand-in for
`ipsw dyld a2s` (tools/sweet-crash.py) that needs nothing but Python.
"""
import bisect, os, struct, sys


def load(cache_dir):
    main = open(os.path.join(cache_dir, 'dyld_shared_cache_arm64'), 'rb').read()
    base, = struct.unpack_from('<Q', main, 0xe0)
    img_off, img_cnt = struct.unpack_from('<II', main, 0x1c0)
    images = []
    for i in range(img_cnt):
        addr, _, _, path_off = struct.unpack_from('<QQQI', main, img_off + 32 * i)
        path = main[path_off:main.index(b'\0', path_off)].decode()
        images.append((addr, path))
    images.sort()
    sym = open(os.path.join(cache_dir, 'dyld_shared_cache_arm64.symbols'), 'rb').read()
    lso, = struct.unpack_from('<Q', sym, 0x48)
    nl_off, nl_cnt, str_off, _, ent_off, ent_cnt = struct.unpack_from('<6I', sym, lso)
    per_image = {}
    for i in range(ent_cnt):
        dylib_off, start, count = struct.unpack_from('<QII', sym, lso + ent_off + 16 * i)
        per_image[base + dylib_off] = (start, count)

    def symbols(image_addr):
        start, count = per_image.get(image_addr, (0, 0))
        out = []
        for j in range(start, start + count):
            strx, ntype, _, _, value = struct.unpack_from('<IBBHQ', sym, lso + nl_off + 16 * j)
            if ntype & 0xe0 or not value:	# skip stabs and undefined
                continue
            s = lso + str_off + strx
            out.append((value, sym[s:sym.index(b'\0', s)].decode(errors='replace')))
        out.sort()
        return out

    return base, images, symbols


def main():
    cache_dir, slide = sys.argv[1], int(sys.argv[2], 16)
    base, images, symbols = load(cache_dir)
    starts = [a for a, _ in images]
    cache = {}
    for arg in sys.argv[3:]:
        addr = int(arg, 16)
        un = addr - slide
        i = bisect.bisect_right(starts, un) - 1
        if not base <= un < base + 0x180000000 or i < 0:
            print(f'{addr:#x}  (not in shared cache)')
            continue
        img_addr, path = images[i]
        if img_addr not in cache:
            cache[img_addr] = symbols(img_addr)
        syms = cache[img_addr]
        k = bisect.bisect_right([v for v, _ in syms], un) - 1
        where = f'{syms[k][1]} + {un - syms[k][0]:#x}' if k >= 0 else f'+{un - img_addr:#x}'
        print(f'{addr:#x}  {os.path.basename(path)}  {where}')


if __name__ == '__main__':
    main()
