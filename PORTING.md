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

- **0084u** (CI 40803175177, ADT with UFS + D22AP `/product`, `rd=ufsroot0` read-only,
  `sweetusb=2 sweetecon=1`, 2026-10-28): **UFS reads work.** The kernel finds `userdata` as
  `ufsroot0` and reads it. HFS reports `unknown Volume Signature` (Sailfish ext4, expected),
  `mount(0..9) failed`. USB: the phone drops off the host when it leaves fastboot and never
  re-enumerates. The kernel prints `SWEET USB: endpoint 0 setup failed`; the QUSB2 reads are
  byte-replicated (`PLL_STATUS 0x64646464`, `PWR_CTRL1 0x23232323`, `PORT_TUNE1 0x55555555`,
  `TEST1 0x90909090`), suspicious. The earlier lines (GDSC/clocks/soft reset/ep0 status)
  scrolled off. Unlike edi194 (flashed boot, USB GDSC off), after `fastboot boot` the USB
  block is still powered.

- **0084u again, photographed:** `"userdata" on LUN 0: first block 0x186f40, 112640 MiB`. USB GDSC
  `0xf8222000` (on), all 7 clocks on. DWC3 `GSNPSID 0x5533300a` (v3.00a), `GCTL 0x00112000`
  (device), `GUSB2PHYCFG 0x2400`, `DCTL 0x10f00000` (halted), ABL event buffer `0x99876000`.
  Soft reset done in 1 ms, DEPSTARTCFG OK, then `ep0 cmd 0x1 timeout` (SETEPCONFIG).
  QUSB2 `PWR_CTRL1 0x23` is exactly Linux's qusb2_v2 `disable_ctrl` (VREF_SUPPLY_TRIM |
  CLAMP_N_EN | POWER_DOWN), and `PLL_STATUS` bit 0 (CORE_READY) is 0. ABL powers the PHY
  down on handoff. **0085** clears POWER_DOWN like `qusb2_phy_init()` before the gadget
  starts. Open risk: if ABL also turned off the PHY LDOs (l1a/l12a/l24a), CORE_READY stays 0
  and we need RPMh votes.

- **Root image v1** (`work/root/ios16-root-20H392-v1.img`, sha256 `a5566340…4e90`, built by
  `work/build-root.sh` = README step 2; tweaks applied, data ark skipped: no
  mobileactivationd container in a fresh image). **Flashed to `userdata` 2026-10-28**
  (user-approved; Sailfish erased): 24 sparse chunks, all OKAY, 395 s. Each "Writing" reported
  0.001 s, so this ABL seems to write while the next chunk arrives. Unverified until the
  kernel mounts it.
- **0085r** prepared: kernel 0085, real root `rd=ufsroot0 ufsrw=1 rootrw=1`, `sweetusb=2
  sweetecon=1 sweetlog=1 nointr_consio=1`, `cpus=1`, touch/keys off.

- **0085r booted 2026-10-28 21:28:59** (after re-entering fastboot by hand: the first
  `fastboot boot` after the userdata flash hung at "Sending", like sweet's ABL). About 40 s later
  (21:29:39) the phone dropped into **Qualcomm memory-dump mode** (`05c6:900e` "QUSB_BULK",
  Sahara; NOT EDL 9008). No USB network ever appeared; live log empty. New in this boot:
  the QUSB2 power-up (0085) makes the DWC3 start DMA (prime suspect: SMMU fault if ABL left
  the USB stream translated/faulting), plus UFS writes (`ufsrw`, `sweetlog`). Exit dump mode:
  hold Power + Vol Up + Vol Down ~15 s.
- **0085n** prepared for the bisect: same as 0085r but `sweetusb=1` (USB report-only, no DMA).

- **RAM dump of the 0085r crash** (`work/ramdump-0085r/`, linux-msm `qdl ramdump`, Sahara memory
  debug works on this secboot phone; regions OCIMEM, AOP, KMSG, PMIC PON, RST_STAT, DDR
  0x80000000-0x27fffffff). `tools/sweet-ramlog.py` rebuilds the console from `sweet_log_ring`
  (PA = VA - virt_base + phys_base, here 0x101c52718) → `logs/ram-0085r.txt` (7148 bytes). It ends:
  `QUSB2 powered up: PWR_CTRL1 0x22 PLL_STATUS 0x17 (core ready)` (0085 works), ue0 up,
  `soft reset done after 1 ms`, then nothing. The next step points GEVNTADR at kernel RAM
  `0x1041b8000` and starts the controller. **The first USB DMA above 4 GiB resets the SoC.**
  ABL's own event buffer was at `0x99869000`. **0086**: fixed low-bank DMA window `0x99800000`
  (write-combining, no cache maintenance), boot-arg `sweetusbdma=` to change or `=0` to disable.
  The UFS root itself mounted read-write (`SWEET UFS: ... read-write`, log at block 0x1d06bfa).
- How to get a dump: phone in `05c6:900e` → `work/tools/qdl/build/qdl ramdump --debug -o DIR`
  (one shot: it resets the phone afterwards; the device node is world-writable, no sudo).

- **0086r** (CI 40808729608, DMA window `0x99800000`): **same crash**. Dump mode about 45 s after
  boot; RAM log (`logs/ram-0086r.txt`) again ends at `soft reset done`, after `DMA area 0x99800000
  (fixed, uncached)`. So the DMA address was not the trigger. The bootstrap thread never returns
  from the gadget start (no `BSD root:`). Remaining suspects: the USB controller once the PHY
  runs (0084u, PHY off, read-only: no crash), or the first UFS write (`sweetlog` flush 10 s after
  start; 0084u had no writes). Bisect image **0086n** = same with `sweetusb=1`.

- **0086n** (`sweetusb=1`, real rw root, 2026-10-28 ~22:03): **MILESTONE 2 REACHED: UFS root +
  iOS userland.** No dump mode after 3+ min, so **the USB device start is what reset the SoC**
  (USB is now a separate task; the UFS write path is fine). The screen shows launchd boot
  tasks: MSUEarlyBootTask, `cc_fips_test` (corecrypto FIPS POST all PASSED), `exec keybagd`,
  SMP sampler samples 0-2. Expected noise: BindFS lookup of `/private/var/hardware/Factory...`
  and `System/Library/Pearl/ReferenceFrames failed error: 2` (no factory/Face ID data).

- **0087s** (CI 40811113076, `cpus=8 sweetkeys=1 sweetusb=1`, real root, 2026-10-28): **keys
  work.** Vol up `key=1 gpio=0` → HID `0xe9`, vol down `key=2 gpio2=0` → `0xea`, power `key=0 pon=0x1`
  → `0x30`, press and release each (`0 clients` that early). Passed CPU init with `cpus=8`, no panic;
  the `8/8 CPUs initialized` line is not yet seen. launchd boot tasks run (mount-phase-1/2,
  data-protection, keybag, usermanagerd, init_featureflags, dirs_cleaner).

- 0087s later: **Apple logo shown** (backboardd drew), then SpringBoard and backboardd running
  (SMP sampler stacks of both). bluetoothd / fairplayd.H2 relaunch constantly, which is expected
  without hardware (same as sweet). The kernel console kept drawing over the GUI because
  `sweetfb_noconsole=1` was missing. **0087g** = 0087s + `sweetfb_noconsole=1 sweetfb_hz=30`.

- **0087g** (`sweetfb_noconsole=1`): Apple logo, then no change for 10+ min (GUI never past the
  logo).
- **0088t** (CI 40814660954, `cpus=8 sweetkeys=1 sweettouch=1 sweettouchdelay=60`, console
  visible, 2026-10-28): **TOUCH WORKS.** `SWEET HID: touch 0 down at 735,1892` / `up`, so RMI4
  over wrapper1 SE4 with L28A, the GPIO88 switch and the reset pulse all work. No dump mode in
  5+ min. **Milestone 3 hardware (8 CPUs, keys, touch) is in place.** Remaining: the GUI stays
  on the logo. SpringBoard[26] and backboardd[56] are alive (SMP sampler); their stacks need
  symbolicating.

- **GUI blocked on LaunchServices (0088t, symbolicated with `tools/dsc-a2s.py`).** SpringBoard[26],
  slide 0x5222c000. t0 (main): `-[CCSModuleRepository _initWithDirectoryURLs:...]` →
  `-[LSApplicationRecord initWithBundleIdentifier:...]` → `LaunchServices::Database::Context::_get` →
  `_LSContextInitCommon` → `os_unfair_lock_lock_slow`. t1 holds it: `_LSContextInitCommon` →
  `__LAUNCH_SERVICES_IS_RESEEDING_ITS_DATABASE_AND_MAY_BLOCK__` → sync NSXPC to `lsd`.
  Fresh root image, so lsd builds the app database on first boot. Either slow or lsd stuck.
  Next: sample lsd (`sweetsample=SpringBoard,lsd`).
- Symbolication: mount the SystemOS cryptex read-only as a normal user
  (`apfs-fuse work/sysos.dmg work/mnt-sysos`, the dmg symlinked to a name without commas), then
  `tools/dsc-a2s.py work/mnt-sysos/root/System/Library/Caches/com.apple.dyld SLIDE ADDR...`.

- **0088l** (2026-10-29, `sweetsample=SpringBoard,lsd`): lsd[100] alive, slide 0x2db74000. t4 serves
  SpringBoard in `__LAUNCH_SERVICES_CLIENTS_ARE_WAITING_FOR_THE_DATABASE_TO_FINISH_SEEDING__`. t1 (the
  seed): `-[MIInstalledInfoGatherer _populatePlaceholderRecordValues:...]` →
  `+[MIInstallationIdentity installationIdentityForBundle:...]` → `_GenerateBuiltInAppInstallSessionID`
  → libMobileGestalt → `dispatch_semaphore_wait`. t6: MobileGestalt CFRunLoop worker. `tools/dsc-dis.py`
  shows the wait in `MGIOKitSupport.c`, matching **`IONetworkController`** after checking
  `IODeviceTree:/chosen` and sdio/wlan/marconi-wifi nodes, i.e. MobileGestalt wants the **Wi-Fi MAC**.
  The D22AP tree has `/chosen mac-address-wifi0/bluetooth0/ethernet0/ethernet1` (iBoot fills
  them); our ADT had none. **0088m**: fajita ADT gains synthetic locally administered MACs
  02:66:6a:00:00:01..04 (sweet ADT unchanged).

- **0088m** (2026-10-29): **SPRINGBOARD WORKS** (user report). The `/chosen` MAC addresses
  unblocked MobileGestalt → lsd seed → SpringBoard. **0088q** = 0088m + `sweetfb_noconsole=1`
  (console muted over the GUI; still in the log ring, panics still drawn); the current daily image.

- **0088q** (2026-10-29): **MILESTONE 4: iOS GUI usable on fajita.** Lock screen, unlock, touch,
  power/volume keys, 8 CPUs (user report: "everything seems to work"). Known issue: Settings →
  General → About freezes Settings (unverified guess: synchronous CoreTelephony query to the
  removed CommCenter, or the HFS capacity crash sweet saw). Next: `sweetsample=Preferences,SpringBoard`
  and symbolicate.

- **Decision (2026-10-29):** once the USB console works, flash a stable image to `boot_a` (user
  approved in principle; confirm right before). Watch for: A/B retry counter exhaustion (kernel
  may need to mark the slot successful) and `fastboot boot` possibly refused with an image on the
  active slot (UniXNU report, unverified here).
- 2026-10-29: GitHub macOS arm64 runners out of capacity ("job was not acquired by Runner"), so
  0089 CI runs 40889556591 (x2), 40895082214, 40899448896, 40903778296, 40907164548 (push to
  main) and 40908014652 were all cancelled before compiling. Paused; re-run `gh workflow run
  xnu.yml -R tre4surehunter9/ios-fajita --ref fajita` once GitHub has macOS capacity again.
  0089 itself is untested (never compiled).

- **2026-10-29: `shim-boot-0088q.img` flashed to `boot_a` by the user; it boots iOS without a PC.**
  Still to observe: whether the A/B retry counter eventually rejects slot a (fix:
  `fastboot set_active a`; long-term the kernel marks the slot successful), and whether
  `fastboot boot` of test images still works with an image on the active slot. Restore with the
  original `dtb/fajita/boot_a.img` (Sailfish kernel) if ever needed.

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
