# Porting to the OnePlus 6T (`fajita`, SDM845)

Branch `fajita` of `tre4surehunter9/ios-sweet`. fajita is a second board next to
sweet: same kernel binary, per-board shim build (`BOARD=fajita shim/build.sh`) and
ADT (`tools/mkadt.py --board fajita`). Sweet's ADT output is byte-identical to
before; sweet's shim build is unchanged apart from reading the same constants
from `shim/board.h`.

Rules for this port: nothing is flashed. Boot only with `fastboot boot`. Never
touch bootloader, modem, persist or other firmware partitions. The phone runs
Sailfish OS on OxygenOS 11 firmware; the user runs every phone-side step.

## Milestone 1 plan: kernel prints to the framebuffer

| Step | What | Who | State |
| --- | --- | --- | --- |
| 1 | Kernel: early console takes framebuffer address and geometry from `boot_args.Video`, not sweet's constants (patch 0082) | me | done, CI run 40799749199 |
| 2 | Shim `BOARD=fajita`: framebuffer `0x9d400000` 1080×2340, no IMEM write, header v0 + `Image.gz` + appended DTBs (same layout as the phone's `boot_a`); no-payload **probe mode** | me | built: `shim/out-fajita/shim-boot.img` |
| 3 | ADT `--board fajita`: framebuffer, CPUs, high-memory range `0x100000000`+`0xfcc00000`, no UFS node | me | done |
| 4 | Read-only dump from Sailfish: live FDT, `/proc/iomem`, `/proc/cmdline`, current boot image (for the stock DTBs and header) | user | done 2026-10-28 (`dtb/fajita/`, not in git) |
| 5 | `fastboot boot` the probe image: tests `fastboot boot` support, framebuffer, continuous scanout, PSCI, GIC timer, SMP, memory map | user | waiting |
| 6 | XNU boot: `cpus=1`, every sweet driver off, no root device. Success = kernel console on screen, ending at "waiting for root device" | user | after 4, 5 and CI |

## Blocked / open

- **`fastboot boot` on the OOS 11 bootloader is unconfirmed.** edi194/ios-fajita reports
  `unknown command` on OOS 10/11 and flashes `boot_b` instead. johnpanos/UniXNU-fajita
  reports it works (and that it fails with "Load Error" when the active slot's boot
  partition holds an image). Step 5 settles it. If it fails, the port is blocked by
  our no-flash rule until the user decides otherwise.
- **Panel refresh.** The panel is a DSI **command-mode** panel ✔ (`msm_drm.dsi_display0=dsi_samsung_s6e3fc2x01_cmd_display`).
  A CPU write to the splash buffer appears only if ABL left MDP auto-refresh on.
  The probe's once-per-second heartbeat answers this. Unsure.
- **Milestone 2 conflict.** Sweet's design puts the iOS root in `userdata`. On this
  phone `userdata` holds Sailfish OS, so a UFS root there erases Sailfish. Needs a
  decision before milestone 2.
- **Payload size.** UniXNU-fajita reports that ABL refuses large ramdisks. Sweet's payload
  (kernel + ADT) is the ramdisk. Not yet measured.

## Hardware facts

Sources: M = mainline `sdm845.dtsi` / `sdm845-oneplus-common.dtsi` /
`sdm845-oneplus-fajita.dts` (torvalds/linux master, fetched 2026-10-28);
P = postmarketOS `device-oneplus-fajita/deviceinfo`; R = reported by another port,
unverified; ✔ = confirmed on this phone (none yet).

| Item | Value | Source |
| --- | --- | --- |
| SoC / CPUs | SDM845, 4× A55 + 4× A75, MPIDR `core << 8` (`0x000`–`0x700`) | M |
| GIC | GICv3, GICD `0x17a00000`, GICR `0x17a60000` ×8, same as sweet | M |
| Timer | PPIs 1/2/3/0, so virtual timer INTID 19, same as sweet | M |
| PSCI | `arm,psci-1.0`, method `smc` | ✔ live FDT |
| RAM (8 GB phone) | FDT memory `0x80000000`+4 GiB and `0x180000000`+`0xfcac0000`. `/proc/iomem` System RAM ends at `0x27cabffff`. All fixed reserved-memory regions are below 4 GiB; dynamic pools have alloc-ranges below 4 GiB | ✔ live FDT, iomem |
| Reserved (low bank) | hyp `0x85700000`, xbl, aop, smem `0x86000000`, tz `0x86200000`, rmtfs `0x88f00000` (+ OnePlus rmtfs `0xf5b00000`), qseecom `0x8ab00000`, camera, ipa, gpu, adsp `0x8c500000`, wlan, mpss `0x8e000000`–`0x957fffff`, venus, cdsp, mba, slpi, spss `0x97b00000`, splash `0x9d400000`, ramoops `0xac300000` | M |
| Framebuffer | `0x9d400000` (36 MiB region), 1080×2340, a8r8g8b8, stride 4320. Inside the same 32 MiB block as sweet's (`0x9c000000`) | M, P |
| Panel | `samsung,s6e3fc2x01` on DSI0, **command mode**. Reset GPIO6, TE GPIO10 | M, ✔ cmdline |
| Boot image (Sailfish `boot_a`, boots today) | header v0, base `0x80000000`, kernel `+0x8000`, ramdisk `+0x1000000`, tags `+0x100`, page 4096, no os_version; `Image.gz` + 3 generic DTBs (SDM845 v2 / v1 / v2.1, board-id 0) | ✔ |
| ABL board id | `<8 0 18801 41>`, msm-id `<0x141 0x20001>` (SDM845 v2.1), project 18801, hw_version 41, active slot `_a` | ✔ live FDT, cmdline |
| UFS | `ufshc@1d84000` (same as sweet), reset GPIO150, VCC `l20a` | M |
| Touch | Synaptics RMI4 (`syna,rmi4-s3706b`) at I²C12 `0xa90000` addr `0x20`, IRQ GPIO125 | M |
| Volume keys | PM8998 GPIO5 (down), GPIO6 (up); power on PM8998 PON | M |
| Debug UART | GENI SE `0xa84000` (uart9), which is the address of sweet's touch I²C. Keep `sweettouch=0` | M |
| Fuel gauge | bq27411 at I²C10 `0xa88000` addr `0x55` | M |
| IMEM restart reason | `0x146bf65c` (R); not written by the shim | R |

## Kernel boot-args for fajita (milestone 1)

`debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress sweettouch=0 sweetkeys=0 sweetlog=0`

No `rd=`, `ufsrw`, `sweetusb` or `sweetbattery`: nothing touches UFS, USB, the PMIC
or I²C. The ADT has no UFS node either.
