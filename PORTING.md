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
| 1 | Kernel: early console takes framebuffer address and geometry from `boot_args.Video`, not sweet's constants (patch 0082) | me | done, CI build pending |
| 2 | Shim `BOARD=fajita`: framebuffer `0x9d400000` 1080×2340, no IMEM write, header v1 + `Image.gz` + appended DTBs; no-payload **probe mode** | me | done, compile-checked with clang only (needs `aarch64-linux-gnu-gcc` to link) |
| 3 | ADT `--board fajita`: framebuffer, CPUs, high-memory range, no UFS node | me | done; memory range unverified |
| 4 | Read-only dump from Sailfish: live FDT, `/proc/iomem`, `/proc/cmdline`, current boot image (for the stock DTBs and header) | user | waiting |
| 5 | `fastboot boot` the probe image: tests `fastboot boot` support, framebuffer, continuous scanout, PSCI, GIC timer, SMP, memory map | user | waiting |
| 6 | XNU boot: `cpus=1`, every sweet driver off, no root device. Success = kernel console on screen, ending at "waiting for root device" | user | after 4, 5 and CI |

## Blocked / open

- **`fastboot boot` on the OOS 11 bootloader is unconfirmed.** edi194/ios-fajita reports
  `unknown command` on OOS 10/11 and flashes `boot_b` instead. johnpanos/UniXNU-fajita
  reports it works (and that it fails with "Load Error" when the active slot's boot
  partition holds an image). Step 5 settles it. If it fails, the port is blocked by
  our no-flash rule until the user decides otherwise.
- **Panel refresh.** The panel (`samsung,s6e3fc2x01`) may be a DSI command-mode panel.
  Then a CPU write to the splash buffer appears only if ABL left MDP auto-refresh on.
  The probe's once-per-second heartbeat answers this. Unsure.
- **Milestone 2 conflict.** Sweet's design puts the iOS root in `userdata`. On this
  phone `userdata` holds Sailfish OS, so a UFS root there erases Sailfish. Needs a
  decision before milestone 2.
- **Payload size.** UniXNU-fajita reports that ABL refuses large ramdisks. Sweet's payload
  (kernel + ADT) is the ramdisk. Not yet measured.
- Local toolchain: `aarch64-linux-gnu-gcc` is not installed (Arch: `sudo pacman -S aarch64-linux-gnu-gcc`).

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
| PSCI | `enable-method = "psci"`. Conduit assumed SMC as on sweet; probe checks | M / assumed |
| RAM | `memory@80000000` size filled by bootloader. High bank above 4 GiB size unknown | M |
| Reserved (low bank) | hyp `0x85700000`, xbl, aop, smem `0x86000000`, tz `0x86200000`, rmtfs `0x88f00000` (+ OnePlus rmtfs `0xf5b00000`), qseecom `0x8ab00000`, camera, ipa, gpu, adsp `0x8c500000`, wlan, mpss `0x8e000000`–`0x957fffff`, venus, cdsp, mba, slpi, spss `0x97b00000`, splash `0x9d400000`, ramoops `0xac300000` | M |
| Framebuffer | `0x9d400000` (36 MiB region), 1080×2340, a8r8g8b8, stride 4320. Inside the same 32 MiB block as sweet's (`0x9c000000`) | M, P |
| Panel | `samsung,s6e3fc2x01-ams641rw` on DSI0. Reset GPIO6, TE GPIO10. Video/command mode: unsure | M |
| Boot image | header v1 (R), `Image.gz` + appended DTBs (P: `append_dtb`), base 0, kernel `0x8000`, ramdisk `0x1000000`, tags `0x100`, page 4096 | P, R |
| ABL board id | `qcom,board-id = <8 0 18801 41>` | M |
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
