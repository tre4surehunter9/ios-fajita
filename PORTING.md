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
| 5 | `fastboot boot` the probe image: tests `fastboot boot` support, framebuffer, continuous scanout, PSCI, GIC timer, SMP, memory map | user | **done 2026-10-28**: `fastboot boot` OKAY; red probe screen, heartbeat counting |
| 6 | XNU boot: `cpus=1`, every sweet driver off, no root device. Success = kernel console on screen, ending at "waiting for root device" | user | **done 2026-10-28** (image `shim-boot-0083e.img`) |

## Boot log

- **0082f** (CI 40799749199 kernel, `cpus=1`, drivers off, 2026-10-28): `fastboot boot`
  of the 17 MB image OK. Console reaches `VM bootstrap done: 85 maps, 165 entries and
  582 holes left` and shows nothing newer after 2 min. Likely cause: the ADT CPU nodes said
  only `qcom,kryo385`, but SweetCPU matches `qcom,kryo470`. Without a CPU nub,
  `kperf_init_early()` waits forever in `ml_wait_max_cpus()`. edi194 hit the same stop.
  The visible console may lag behind the real stop point.
- **0082b**: same kernel; the ADT CPU compatible is now `qcom,kryo385`, `qcom,kryo470`.
  **The CPU nub fix works.** The kernel passes CPU init and reaches `bsd_init`; the screen
  then shows `dlil_init: Waiting for all the created dlil kernel threads to get scheduled
  at least once.` Sweet passed this point. Unknown whether the kernel is stuck or the
  console ring stopped reaching the panel.
- **0083e** (CI 40801501272 kernel, ADT 0082b, `sweetecon=1`, 2026-10-28): **MILESTONE 1
  REACHED.** The console wraps (newest at the top). In order: SWEET CPU 1/1 initialized, SGI1
  verified; MAC framework; SWEET AMFI / Sandbox / HFS registered; mbinit; DLIL SUM16 PASSED;
  dlil threads scheduled (so the earlier stop was only the screen being stale, and `sweetecon=1`
  fixes that); Skywalk self-tests all PASSED; `bsd_autoconf: calling kminit`; ifnet lo0/gif0/stf0;
  `Waiting on <dict>IOProviderClass IOMedia, Content Apple_HFS</dict>`;
  `Failed to write boot-picker-bringup-reason to NVRAM.` (expected, there is no NVRAM). AppleCLCD
  reports `1080x2340 stride 4320 at 0x9d400000, presented as 1125x2436`. No panic.

## USB live console (in progress, patch 0084)

PC side, no sudo: NetworkManager profile `ios-usb` (created 2026-10-28) matches the
phone's host-side MAC `02:00:5e:77:ee:01` and sets `192.168.7.1/24`. To remove it:
`nmcli con delete ios-usb`. Capture with `tools/sweet-usblog.py logs/NAME.txt --secs N`.
Phone side: boot-arg `sweetusb=2`.

### Background

Being able to read the console from the PC instead of photos. Sweet already has
this: patches 0072-0077, boot-arg `sweetusb=2`. The phone appears as a CDC-ECM
network device and streams the console as UDP to 192.168.7.1:5140, which
`tools/sweet-usbnet.sh` captures. On fajita:
- Same: DWC3 core `0xa600000` (✔ cmdline `androidboot.usbcontroller=a600000.dwc3`),
  Qualcomm wrapper/QSCRATCH `0xa6f8800`, GCC base `0x100000` (M).
- Must check: the GCC USB clock/GDSC register offsets, which differ between SM7150 and
  SDM845. Also the QUSB2 PHY (on SDM845 mainline, USB 2 only is used, with
  `qcom,select-utmi-as-pipe-clk`). The first step is sweet's read-only probe
  (0072) with fajita offsets.

## Blocked / open

- ~~`fastboot boot` unconfirmed~~ **Works ✔** (probe boot 2026-10-28, `Sending OKAY / Booting OKAY`). Earlier note: edi194/ios-fajita reports
  `unknown command` on OOS 10/11 and flashes `boot_b` instead. johnpanos/UniXNU-fajita
  reports it works (and that it fails with "Load Error" when the active slot's boot
  partition holds an image). Step 5 settles it. If it fails, the port is blocked by
  our no-flash rule until the user decides otherwise.
- **Panel refresh.** The panel is a DSI **command-mode** panel ✔ (`msm_drm.dsi_display0=dsi_samsung_s6e3fc2x01_cmd_display`).
  A CPU write to the splash buffer appears only if ABL left MDP auto-refresh on.
  ✔ It does: the probe's heartbeat keeps updating, so ABL leaves the panel refreshing from `0x9d400000`.
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
