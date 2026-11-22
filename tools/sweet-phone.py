#!/usr/bin/env python3
"""Files and commands on the phone over USB Ethernet (192.168.7.2).

  sweet-phone.py put LOCAL REMOTE [MODE]   copy a file to the phone (MODE octal, default: local mode)
  sweet-phone.py get REMOTE LOCAL          copy a file from the phone
  sweet-phone.py read REMOTE OFF LEN OUT   byte range of any file or device (decimal or 0x)
  sweet-phone.py mkdir REMOTE [MODE]       (exists = OK)
  sweet-phone.py rm REMOTE
  sweet-phone.py chmod REMOTE MODE
  sweet-phone.py ln TARGET REMOTE          symlink
  sweet-phone.py run PROG [ARGS...]        run as root; output streams here; exits with its code

Files: the kernel's file server, tcp 2223 (bsd/dev/sweet_files.c, kernel 0132, boot-arg sweetusb=2).
Commands: phone/sweetcmd, tcp 2224 (boot payload: mkpayload.py --run sweetcmd).
The PC side of the link needs 192.168.7.1/24 (NetworkManager profile or tools/sweet-usbnet.sh).
Protocol of port 2223 as ios-flame's tools/flame-files.py (MIT).
"""
import os, socket, sys

HOST = os.environ.get('SWEET_PHONE', '192.168.7.2')
TRAILER = b'\n@@SWEETCMD '


def connect(port):
    s = socket.create_connection((HOST, port), timeout=10)
    s.settimeout(None)
    return s


def status_line(s):
    line = b''
    while not line.endswith(b'\n'):
        c = s.recv(1)
        if not c:
            raise SystemExit('connection closed before a reply')
        line += c
    line = line.decode().strip()
    if line.startswith('ERR'):
        code = int(line.split()[1])
        raise SystemExit(f'phone: {os.strerror(code)} (errno {code})')
    return line


def simple(cmd):
    with connect(2223) as s:
        s.sendall(cmd.encode() + b'\n')
        status_line(s)


def recv_exact(s, n, out):
    while n > 0:
        b = s.recv(min(n, 1 << 16))
        if not b:
            raise SystemExit(f'short transfer: {n} bytes missing')
        out.write(b)
        n -= len(b)


def num(x):
    return int(x, 0)


def main(a):
    if len(a) < 2:
        raise SystemExit(__doc__)
    cmd = a[1]
    if cmd == 'put' and len(a) in (4, 5):
        data = open(a[2], 'rb').read()
        mode = int(a[4], 8) if len(a) == 5 else os.stat(a[2]).st_mode & 0o7777
        if ' ' in a[3]:
            raise SystemExit('remote paths cannot contain spaces')
        with connect(2223) as s:
            s.sendall(f'PUT {a[3]} {mode:o} {len(data)}\n'.encode() + data)
            status_line(s)
        print(f'{a[3]}: {len(data)} bytes, mode {mode:o}')
    elif cmd == 'get' and len(a) == 4:
        with connect(2223) as s:
            s.sendall(f'GET {a[2]}\n'.encode())
            n = int(status_line(s).split()[1])
            with open(a[3], 'wb') as f:
                recv_exact(s, n, f)
        print(f'{a[3]}: {n} bytes')
    elif cmd == 'read' and len(a) == 6:
        with connect(2223) as s:
            s.sendall(f'READ {a[2]} {num(a[3])} {num(a[4])}\n'.encode())
            n = int(status_line(s).split()[1])
            with open(a[5], 'wb') as f:
                recv_exact(s, n, f)
        print(f'{a[5]}: {n} bytes')
    elif cmd == 'mkdir' and len(a) in (3, 4):
        simple(f'MKDIR {a[2]} {a[3] if len(a) == 4 else "755"}')
    elif cmd == 'rm' and len(a) == 3:
        simple(f'RM {a[2]}')
    elif cmd == 'chmod' and len(a) == 4:
        simple(f'CHMOD {a[2]} {a[3]}')
    elif cmd == 'ln' and len(a) == 4:
        simple(f'LINK {a[2]} {a[3]}')
    elif cmd == 'run' and len(a) >= 3:
        args = a[2:]
        if any('\n' in x for x in args):
            raise SystemExit('arguments cannot contain newlines')
        with connect(2224) as s:
            s.sendall(f'RUN {len(args)}\n'.encode() + ''.join(x + '\n' for x in args).encode())
            tail = b''
            while True:
                b = s.recv(1 << 16)
                if not b:
                    break
                tail += b
                # Hold back anything that could be the start of the trailer.
                keep = len(tail) - len(TRAILER) - 32
                if keep > 0 and TRAILER not in tail:
                    sys.stdout.buffer.write(tail[:keep])
                    sys.stdout.buffer.flush()
                    tail = tail[keep:]
            i = tail.rfind(TRAILER)
            if i < 0:
                sys.stdout.buffer.write(tail)
                raise SystemExit('phone: connection closed without an exit status')
            sys.stdout.buffer.write(tail[:i])
            sys.stdout.buffer.flush()
            what, value = tail[i + len(TRAILER):].decode().split()[:2]
            if what == 'EXIT':
                sys.exit(int(value))
            if what == 'SIGNAL':
                print(f'\n[killed by signal {value}]', file=sys.stderr)
                sys.exit(128 + int(value))
            raise SystemExit(f'phone: cannot run {args[0]}: {os.strerror(int(value))}')
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
