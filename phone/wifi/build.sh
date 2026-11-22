#!/bin/sh
# wland is built by the "phone" workflow (.github/workflows/phone.yml, artifact phone-bin), as the
# other phone programs. ios-flame built it on a Mac with work/userland/bin/ios-cc:
#   ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c \
#          rmtfs.c pdmapper.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c
echo "build wland with the phone workflow (gh workflow run phone.yml --ref fajita)" >&2
exit 1
