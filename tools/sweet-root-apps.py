#!/usr/bin/env python3
"""Add the rest of the iOS apps (and, optionally, SweetMetal2) to an iOS root made by tools/mkiosroot.sh.

  sweet-root-apps.py ROOT [--metal2 MTL2INSTALL] [--extract-only DIR]

On a real iPhone the removable Apple apps (Calculator, Weather, Notes, Clock, Maps, Music, Files, ... 32 in
20H392) ship in /private/var/staged_system_apps and are installed by the first-boot data migration, which
tools/sweet-root-tweaks.py skips (com.apple.migration LastSystemVersion). Here they are copied to
/Applications instead, as system apps: they are Apple-signed and listed in the IPSW's static trust cache like
everything else on the OS volume. LaunchServices' database (com.apple.LaunchServices-*.csstore in lsd's data
container) is removed so lsd seeds it again on the next boot and picks them up; that first boot takes longer.

--metal2 takes the mtl2install binary (artifact of the "metal2" workflow) and writes what it carries, as
mtl2install itself does on the phone: /System/Library/Extensions/SweetMetal2.bundle, /var/local (0777) with
mtl2-clients = "SpringBoard" and an empty mtl2.log (0666). --extract-only DIR only unpacks the bundle into DIR
(a check that needs no root).

Run as root on a mounted root. The staged apps are mobile:mobile (501); the copies are root:admin (0:80) with
their modes kept, as the apps already in /Applications.
"""
import argparse, glob, os, shutil, stat, struct, sys, zlib

STAGED = 'private/var/staged_system_apps'
BUNDLE = 'System/Library/Extensions/SweetMetal2.bundle'


def macho_symbols(data, names):
    """File offsets of the given symbols in a thin 64-bit Mach-O."""
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != 0xfeedfacf:
        sys.exit('mtl2install: not a thin 64-bit Mach-O')
    off, segs, symtab = 32, [], None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x19:                                         # LC_SEGMENT_64
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, off + 24)
            segs.append((vmaddr, vmsize, fileoff))
        elif cmd == 0x2:                                        # LC_SYMTAB
            symtab = struct.unpack_from('<IIII', data, off + 8)
        off += size
    symoff, nsyms, stroff, _ = symtab
    found = {}
    for i in range(nsyms):
        strx, _, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
        name = data[stroff + strx:data.index(b'\0', stroff + strx)].decode()
        if name in names:
            for vmaddr, vmsize, fileoff in segs:
                if vmaddr <= value < vmaddr + vmsize:
                    found[name] = fileoff + value - vmaddr
    missing = set(names) - set(found)
    if missing:
        sys.exit(f'mtl2install: symbols not found: {sorted(missing)}')
    return found


def metal2_entries(binary):
    """(relative path, mode, bytes) of each file in mtl2install's SWAR archive (phone/mtl2install/pack.py)."""
    data = open(binary, 'rb').read()
    sym = macho_symbols(data, ['_install_blob_start', '_install_blob_end'])
    blob = data[sym['_install_blob_start']:sym['_install_blob_end']]
    size, = struct.unpack_from('<Q', blob, 0)
    ar = zlib.decompress(blob[8:], -15)                        # COMPRESSION_ZLIB is raw DEFLATE
    if len(ar) != size or ar[:4] != b'SWAR':
        sys.exit('mtl2install: bad archive')
    count, = struct.unpack_from('<I', ar, 4)
    off, out = 8, []
    for _ in range(count):
        plen, = struct.unpack_from('<H', ar, off)
        rel = ar[off + 2:off + 2 + plen].decode()
        mode, fsize = struct.unpack_from('<IQ', ar, off + 2 + plen)
        start = off + 14 + plen
        if '..' in rel or start + fsize > len(ar):
            sys.exit(f'mtl2install: bad entry {rel!r}')
        out.append((rel, mode, ar[start:start + fsize]))
        off = start + fsize
    return out


def write_bundle(dest, entries):
    for rel, mode, body in entries:
        path = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(path), mode=0o755, exist_ok=True)
        with open(path, 'wb') as f:
            f.write(body)
        os.chmod(path, mode & 0o777)
    print(f'SweetMetal2.bundle: {len(entries)} files, {sum(len(b) for _, _, b in entries)} bytes -> {dest}')


def copy_owned(src, dst):
    """copytree that keeps symlinks and modes, owned root:admin (0:80) as the other /Applications apps."""
    shutil.copytree(src, dst, symlinks=True, copy_function=shutil.copy2)
    for base, dirs, files in os.walk(src):
        rel = os.path.relpath(base, src)
        for name in [''] + dirs + files:
            s = os.path.join(base, name) if name else base
            d = os.path.join(dst, rel, name) if name else os.path.join(dst, rel)
            st = os.lstat(s)
            os.lchown(d, 0, 80)
            if not stat.S_ISLNK(st.st_mode):
                os.chmod(d, stat.S_IMODE(st.st_mode))


def staged_apps(root):
    src_dir = os.path.join(root, STAGED)
    apps = sorted(n for n in os.listdir(src_dir) if n.endswith('.app'))
    added = []
    for name in apps:
        dst = os.path.join(root, 'Applications', name)
        if os.path.exists(dst):
            print(f'apps: {name} already in /Applications, left alone')
            continue
        copy_owned(os.path.join(src_dir, name), dst)
        added.append(name)
    print(f'apps: {len(added)} of {len(apps)} staged apps copied to /Applications: {" ".join(added)}')
    return added


def reseed_launchservices(root):
    stores = glob.glob(os.path.join(root, 'private/var/containers/Data/System/*/Library/Caches/'
                                          'com.apple.LaunchServices*.csstore'))
    for s in stores:
        os.unlink(s)
        print(f'launchservices: removed {os.path.relpath(s, root)}')
    if not stores:
        print('launchservices: no database found (lsd seeds one on first boot anyway)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root', nargs='?')
    ap.add_argument('--metal2', help='mtl2install binary carrying SweetMetal2.bundle')
    ap.add_argument('--extract-only', help='only unpack the bundle into this directory')
    a = ap.parse_args()
    if a.extract_only:
        if not a.metal2:
            sys.exit('--extract-only needs --metal2')
        write_bundle(a.extract_only, metal2_entries(a.metal2))
        return
    if not a.root or not os.path.isdir(os.path.join(a.root, STAGED)):
        sys.exit(f'{a.root}: not an iOS root ({STAGED} missing)')
    if os.geteuid() != 0:
        sys.exit('run as root (owners must be kept)')
    staged_apps(a.root)
    reseed_launchservices(a.root)
    if a.metal2:
        write_bundle(os.path.join(a.root, BUNDLE), metal2_entries(a.metal2))
        local = os.path.join(a.root, 'private/var/local')
        os.makedirs(local, exist_ok=True)
        os.chmod(local, 0o777)
        with open(os.path.join(local, 'mtl2-clients'), 'w') as f:
            f.write('SpringBoard\n')
        log = os.path.join(local, 'mtl2.log')
        open(log, 'a').close()
        os.chmod(log, 0o666)
        print('metal2: /var/local/mtl2-clients = SpringBoard, mtl2.log 0666')


if __name__ == '__main__':
    main()
