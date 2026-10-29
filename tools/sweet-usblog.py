#!/usr/bin/env python3
"""Receive the phone's live kernel console (sweetusb=2) without sudo.

  sweet-usblog.py LOGFILE [--secs N]

The phone streams its console as UDP to 192.168.7.1:5140 (patch 0077). The
PC address comes from a NetworkManager profile matched on the phone's fixed
host-side MAC (see PORTING.md), so nothing here needs root. Appends every
datagram to LOGFILE as it arrives; exits after N seconds if --secs is given.
"""
import socket, sys, time

log = sys.argv[1]
secs = float(sys.argv[sys.argv.index('--secs') + 1]) if '--secs' in sys.argv else None
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', 5140))
s.settimeout(1.0)
end = time.monotonic() + secs if secs else None
with open(log, 'ab') as f:
    while end is None or time.monotonic() < end:
        try:
            data, _ = s.recvfrom(65536)
        except socket.timeout:
            continue
        f.write(data)
        f.flush()
