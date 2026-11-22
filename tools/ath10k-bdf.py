#!/usr/bin/env python3
"""List or extract boards from an ath10k board-2.bin.

  ath10k-bdf.py BOARD-2.BIN                 list the board names and sizes
  ath10k-bdf.py BOARD-2.BIN NAME OUT        write board NAME's data to OUT

Format as mainline ath10k (core.c, hw.h): "QCA-ATH10K-BOARD\\0" padded to 4 bytes, then IEs
{u32 id, u32 len, data padded to 4}; id 0 = BOARD holding sub-IEs 0 NAME and 1 DATA.
For WCN3990 (snoc, QMI) the names are "bus=snoc,qmi-board-id=%x[,qmi-chip-id=%x]";
phone/wifi/wlfw.c picks the same way on the phone from the ids the firmware reports.
"""
import struct, sys

MAGIC = b'QCA-ATH10K-BOARD\0'


def ies(buf, off, end):
    while off + 8 <= end:
        ie, n = struct.unpack_from('<II', buf, off)
        body = off + 8
        if body + n > end:
            raise SystemExit(f'truncated IE {ie} at {off:#x}')
        yield ie, buf[body:body + n]
        off = body + ((n + 3) & ~3)


def boards(path):
    buf = open(path, 'rb').read()
    if not buf.startswith(MAGIC):
        raise SystemExit(f'{path}: not a board-2.bin (bad magic)')
    start = (len(MAGIC) + 3) & ~3
    for ie, body in ies(buf, start, len(buf)):
        if ie != 0:
            continue
        name, data = None, None
        for sid, sbody in ies(body, 0, len(body)):
            if sid == 0:
                name = sbody.decode(errors='replace')
            elif sid == 1:
                data = sbody
        yield name, data


def main(a):
    if len(a) == 2:
        for name, data in boards(a[1]):
            print(f'{len(data) if data else 0:7d}  {name}')
    elif len(a) == 4:
        for name, data in boards(a[1]):
            if name == a[2]:
                open(a[3], 'wb').write(data)
                print(f'{a[3]}: {len(data)} bytes')
                return
        raise SystemExit(f'no board named {a[2]!r}')
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
