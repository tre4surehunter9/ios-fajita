#!/usr/bin/env python3
"""Disassemble a window of the iOS 16 split shared cache and resolve strings.

  dsc-dis.py CACHE_DIR IMAGE_NAME OFFSET [LENGTH]

IMAGE_NAME is a basename such as libMobileGestalt.dylib, OFFSET the
"+0x..." offset that tools/dsc-a2s.py prints for stripped images (from the
image's __TEXT start), LENGTH the bytes to show (default 0x200, centred on
OFFSET). Uses aarch64-linux-gnu-objdump; adrp+add/ldr pairs that point at a
C string in the cache get the string appended as a comment.
"""
import os, re, struct, subprocess, sys, tempfile


def mappings(cache_dir):
    """(address, size, file, file_offset) for the main cache and every subcache."""
    out = []
    for name in sorted(os.listdir(cache_dir)):
        if not name.startswith('dyld_shared_cache_arm64') or name.endswith('.symbols'):
            continue
        path = os.path.join(cache_dir, name)
        with open(path, 'rb') as f:
            head = f.read(0x200)
            moff, mcnt = struct.unpack_from('<II', head, 0x10)
            f.seek(moff)
            raw = f.read(32 * mcnt)
        for i in range(mcnt):
            addr, size, foff = struct.unpack_from('<QQQ', raw, 32 * i)
            out.append((addr, size, path, foff))
    return out


def reader(maps):
    files = {}

    def read(addr, n):
        for a, size, path, foff in maps:
            if a <= addr < a + size:
                f = files.setdefault(path, open(path, 'rb'))
                f.seek(foff + addr - a)
                return f.read(min(n, a + size - addr))
        return b''
    return read


def image_base(cache_dir, image):
    main = open(os.path.join(cache_dir, 'dyld_shared_cache_arm64'), 'rb').read()
    img_off, img_cnt = struct.unpack_from('<II', main, 0x1c0)
    for i in range(img_cnt):
        addr, _, _, path_off = struct.unpack_from('<QQQI', main, img_off + 32 * i)
        path = main[path_off:main.index(b'\0', path_off)].decode()
        if os.path.basename(path) == image:
            return addr
    raise SystemExit(f'{image} not in cache')


def cstring(read, addr):
    data = read(addr, 200)
    end = data.find(b'\0')
    if end < 2:
        return None
    s = data[:end]
    return s.decode() if all(32 <= c < 127 for c in s) else None


def main():
    cache_dir, image, off = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    length = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x200
    read = reader(mappings(cache_dir))
    base = image_base(cache_dir, image)
    start = base + off - length // 2
    code = read(start, length)
    with tempfile.NamedTemporaryFile(suffix='.bin') as t:
        t.write(code)
        t.flush()
        dis = subprocess.run(['aarch64-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'aarch64',
                              f'--adjust-vma={start:#x}', t.name], capture_output=True, text=True).stdout
    page = {}
    for line in dis.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)', line)
        if not m:
            continue
        pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
        note = ''
        regs = re.findall(r'\bx(\d+)\b', args)
        if op == 'adrp' and regs:
            t = re.search(r'0x([0-9a-f]+)', args)
            if t:
                page[regs[0]] = int(t.group(1), 16)
        elif op in ('add', 'ldr') and len(regs) >= 2 and regs[1] in page:
            imm = re.search(r'#0x([0-9a-f]+)', args)
            if imm:
                target = page[regs[1]] + int(imm.group(1), 16)
                if op == 'ldr':
                    ptr = read(target, 8)
                    target = struct.unpack('<Q', ptr)[0] & 0xfffffffff if len(ptr) == 8 else 0
                s = cstring(read, target) if target else None
                if target and not s:
                    # CFString constant: {isa, flags, const char *ptr, length}
                    raw = read(target + 16, 8)
                    if len(raw) == 8:
                        ptr = struct.unpack('<Q', raw)[0] & 0xfffffffff
                        cf = cstring(read, ptr) if ptr else None
                        s = f'@{cf}' if cf else None
                note = f'   ; {target:#x}' + (f' "{s}"' if s else '')
        mark = '=>' if pc == base + off else '  '
        print(f'{mark} +{pc - base:#07x}  {op:8s} {args}{note}')


if __name__ == '__main__':
    main()
