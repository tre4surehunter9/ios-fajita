# ios-fajita

> [!WARNING]
> **Vibecoded.** All of this port's code, patches, tools and documentation (this README included) were written by an AI. They were then tested on one phone. Nothing here has been reviewed by a human expert. It may be wrong, may break, and may damage your device or erase your data. Use it entirely at your own risk. I am under no obligation to fix bugs, answer issues, or support anyone using it.

**Apple's iOS 16 running on a OnePlus 6T.**

This project runs Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) on the OnePlus 6T (`fajita`, Qualcomm SDM845). On top of the kernel runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and the stock apps.

It is a fork of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the same for the Redmi Note 10 Pro (`sweet`, SM7150). The OnePlus 6T is a second board next to sweet. Both phones boot the **same kernel binary**. The board is chosen at runtime from the generated device tree, and the shim and the device tree are built per board. The engineering log for the 6T is [`PORTING.md`](PORTING.md); sweet's is [`NOTES.md`](NOTES.md).

As of **2026-11-01** the OnePlus 6T boots to the **iOS lock screen and home screen with GPU acceleration**: CoreAnimation renders through Metal on the **Adreno 630** (Apple's Metal API translated to Vulkan by metal2vulkan, run by Mesa's Turnip driver against our kernel GPU driver), at 710 MHz with the CPUs at full clock. The GPU scales each frame onto the panel and the display flips between buffers, so the CPU copies nothing. All the stock apps are installed. Unlocking, the touchscreen, the power and volume keys and all eight CPU cores work. It is usable but not yet fully smooth: about 20–24 frames a second while scrolling. The GPU engineering log is [`GPU-PLAN.md`](GPU-PLAN.md).

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed (see below), and Apple's security model is mostly switched off. The iOS root lives in the `userdata` partition, so installing it **erases whatever was there**.

## What works on the OnePlus 6T

| Area | State |
| --- | --- |
| Boot | Stock OnePlus bootloader (unlocked) → `fastboot boot` of our shim → XNU. Nothing has to be flashed to the boot partitions |
| CPU | All 8 cores (4× Cortex-A55, 4× Cortex-A75) via PSCI, 16K pages, GICv3 Group 1 interrupts, timers; both clusters set to their top OSM level at boot (1766 / 2649 MHz) |
| Memory | 3.95 GiB of the upper DDR bank (`0x100000000`) |
| Storage | UFS host driver (shared with sweet); the iOS root is a journaled HFS+ volume filling `userdata` (about 110 GiB), read/write |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| GPU | Adreno 630: GMU, RPMh votes, zap shader (PAS 13), command processor, SMMU context, 256 MiB GPU arena, GPU interrupts; core PLL at 710 MHz; a NativeAdreno user client (`SweetGPU`) for Mesa Turnip (Vulkan 1.3) and an IOAcceleratorES (`SweetMetal`) that loads `SweetMetal2.bundle` (metal2vulkan + Turnip) into CoreAnimation's processes |
| Display | Kernel IOSurface + IOMobileFramebuffer services. Presented to iOS as an iPhone X panel (1125×2436). Each frame is scaled by the GPU's 2D engine onto the 1080×2340 panel into one of two scan-out buffers, and the display controller's VIG0 pipe flips to it at the next refresh, without blocking the swap. iOS 16's frame-info clock (`Presentation_time`) is provided |
| GUI | backboardd, SpringBoard, lock screen, home screen, wallpaper and widget extensions, Settings (About shows "OnePlus 6T"), all 32 removable Apple apps (Calculator, Weather, Notes, Maps, ...) |
| Touch | Synaptics RMI4 (S3706B) at I²C `0x20` on QUP wrapper 1 SE4 (`0xa90000`), polled; rail PM8998 L28A voted through RPMh |
| Buttons | Power (PM8998 PON), volume up/down (PM8998 GPIO6/GPIO5), read over SPMI |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |
| USB | Device mode (DWC3) as a CDC-ECM network adapter: phone `192.168.7.2`, PC `192.168.7.1`; the live kernel console streams to the PC over UDP (`sweetusb=2`) |
| Debugging | Live console over USB; kernel console on the screen; console ring in RAM, recoverable from Qualcomm's crash-dump mode with a RAM dump; persistent log at the end of `userdata` |

## What does not work (yet)

- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS, SEP / Face ID.** Their daemons start and fail or keep relaunching. `phone/quiet` turns off voicemail's `vmd` (spun a core), `fairplayd.H2` and `ReportCrash`; `bluetoothd` stays on, because mediaserverd and launchd spin without it.
- **Some extensions still crash:** AegirPoster (the astronomy wallpaper) loops, as do `assetsd`, MusicWidgets and BooksWidgetExtension now and then.
- **No charging or battery level:** nothing drives the charger or the fuel gauge yet, and the phone can run flat with no warning.
- **Not fully smooth yet** (about 20–24 frames a second while scrolling): backboardd's render thread uses a whole core, much of it in the Metal-to-Vulkan layer.
- **Only 3.95 GiB of the 8 GiB** RAM is used.
- **The clock starts at 1970**, so anything that checks certificates fails.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection. Do not put anything personal on it.
- **Booting without a PC** means flashing the boot image to `boot_a` (`fastboot flash boot_a shim-boot.img`). This works on the author's phone. Whether the A/B retry counter eventually rejects the slot has not been tested over many boots yet (`fastboot set_active a` resets it). The USB live log only comes up when the phone is started from fastboot (`fastboot boot` or `fastboot continue`).
- **`fastboot boot` sometimes fails** with "Failed to load/authenticate boot image: Load Error" (after big flashes, warm restarts into fastboot, and sometimes even after a cold start). Flashing the image to `boot_a` and then `fastboot continue` has always worked.

## How it boots

```
ABL (stock OnePlus bootloader, unlocked)
  -> fastboot boot shim-boot.img          (temporary; header v0, Image.gz + the stock DTBs appended)
     -> shim: reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU -> UFS driver -> HFS+ root in userdata
              -> dyld + shared cache -> launchd -> daemons -> backboardd + SpringBoard
```

The bootloader leaves the command-mode panel refreshing from its splash buffer, so the kernel draws straight into memory and the panel shows it. Without the shim's payload (an empty ramdisk), `BOARD=fajita` builds a hardware probe instead. The probe prints the memory map, CPU IDs, PSCI, the secondary cores and the timer interrupts, and shows a blinking heartbeat.

## How to do it yourself (OnePlus 6T)

You need a OnePlus 6T with an **unlocked bootloader**, a Linux PC, `aarch64-linux-gnu-gcc`, `mkbootimg`, `dtc`, `fastboot`, and a GitHub account for the kernel build. These steps were tested on an 8 GB 6T on OxygenOS 11 firmware. **Everything on `userdata` is lost.** Bootloader, modem and persist partitions are never touched.

### 1. Get the stock boot image and device tree

From the running Android or Linux system (root), read the active slot's boot partition and the live device tree. Keep them out of git: `dtb/` is ignored, and the live tree contains the serial number.

```sh
cat /proc/cmdline > cmdline.txt; cat /proc/iomem > iomem.txt; cp /sys/firmware/fdt live.dtb
dd if=/dev/block/bootdevice/by-name/boot_a of=boot_a.img bs=1M   # read only
```

On the PC, extract the DTBs appended to the kernel (the shim boot image carries the same ones):

```sh
python3 tools/unpack_bootv2.py dtb/fajita/boot_a.img dtb/fajita/boot_unpacked
python3 tools/extract-appended-dtbs.py dtb/fajita/boot_unpacked/kernel dtb/fajita   # -> dtb/fajita/dtbs
```

### 2. Build the kernel

The kernel builds on GitHub's macOS runners with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). The workflow fetches xnu-8796 and applies `xnu/patches/` in order. It also stages the pinned open-source libpthread, HFS and BearSSL sources the kernel needs. The machine config is still called `SWEET`; it builds the kernel for both boards.

```sh
gh workflow run xnu.yml -R YOU/ios-fajita --ref fajita
gh run download RUN_ID -R YOU/ios-fajita -n xnu-kernel -D xnu/out
```

If a run is cancelled with "job was not acquired by Runner", GitHub's macOS arm64 runners are out of capacity: re-queue it later.

### 3. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple and extract the OS, SystemOS and AppOS disk images (`BuildManifest.plist` names them). With `apfs-fuse`, `hfsprogs` and sudo:

```sh
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 112630M   # fajita userdata minus the log area
sudo fsck.hfsplus -fy ios16-root.img
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

The OnePlus 6T's `userdata` is 112640 MiB. The kernel's persistent log uses its last 4100 KiB, so the volume must end before that; 112630M fills the rest. The image is sparse, so it takes about 12 GB on the PC and in the flash.

A final read-only `fsck.hfsplus -n` can still report "Volume header needs minor repair" after a clean `-fy`. This is a quirk of the Linux fsck, and the volume mounts fine.

The device tree builder copies `/product` from the real iPhone X device tree, which MobileGestalt needs. Unwrap `Firmware/all_flash/DeviceTree.d22ap.im4p` from the IPSW to `research/ipsw-x/d22ap.adt` with `tools/adt.py unwrap` (needs the `lzfse` command).

### 4. Build the boot image

```sh
python3 tools/mkadt.py shim/out-fajita/fajita.adt --board fajita
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out-fajita/fajita.adt shim/out-fajita/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress nointr_consio=1 rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetusb=1 sweetecon=1 sweettouch=1 sweettouchdelay=60 sweetkeys=1 sweetlog=1 sweetfb_hz=30 sweetfb_noconsole=1'
BOARD=fajita shim/build.sh ../shim/out-fajita/payload.bin      # -> shim/out-fajita/shim-boot.img
```

### 5. Flash the root and boot

```sh
fastboot -S 512M flash userdata ios16-root.simg        # about 7 minutes; do not unplug
# the first fastboot boot after a big flash can hang at "Sending": re-enter fastboot (Vol Up + Power)
fastboot boot shim/out-fajita/shim-boot.img
```

The first boot builds the LaunchServices database, which takes a few minutes on the Apple logo; later boots are faster. Swipe up to unlock. To get back to fastboot, hold Volume Up + Power.

### 6. GPU, apps and tidying (optional)

- **GPU firmware:** the boot image carries the A630 SQE/GMU firmware and the zap shader (`mkpayload.py --gpufw DIR`; the files come from postmarketOS / linux-firmware, not from git), plus `sweetgpu=3 sweetmtl=2` in the boot-args.
- **Metal for CoreAnimation:** `SweetMetal2.bundle` (built by the `metal2` workflow) goes to `/System/Library/Extensions`. Either boot once with `mkpayload.py --run mtl2install`, or write it into the root image on the PC with `tools/sweet-root-apps.py --metal2 mtl2install`.
- **All the apps:** `tools/sweet-root-apps.py` copies the 32 removable apps from `/private/var/staged_system_apps` to `/Applications` and makes LaunchServices reseed (the first-boot data migration that installs them on a real phone is skipped here).
- **All of the next two in one:** boot once with `mkpayload.py --run sweetsetup` on a fresh root, then reboot. A marker file makes later runs do nothing.
- **Wallpapers and widgets:** boot once with `mkpayload.py --run extfix` on a kernel with patch 0124. It sets the ExtensionKit flag and deletes LaunchServices' database, which then reseeds with the extensions.
- **Quieter background:** boot once with `mkpayload.py --run quiet` (the `phone` workflow builds it). It takes voicemail, FairPlay and ReportCrash out of launchd's job list on the phone; the original stays as `launchd.plist.pre-quiet`, and `unquiet` puts jobs back.

### Boot-args

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on the `userdata` partition, writable |
| `cpus=8` | Start all cores; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetecon=1` | Synchronous console straight to the screen (the console ring otherwise stops reaching the panel on fajita) |
| `sweetfb_noconsole=1` | Stop drawing kernel text once the GUI presents; it still goes to the log, and panics are drawn |
| `sweetfb_hz=30` | Panel refresh rate of the CPU copy |
| `sweettouch=1 sweettouchdelay=60` | Start touch 60 s after the root mounts; `sweetts_stage=1/2` stops after clocks / power, and `sweetts_xraw`/`sweetts_yraw` set the sensor maxima |
| `sweetkeys=1` | Power and volume keys |
| `sweetusb=0/1/2` | USB off / report only / USB network + live console log |
| `sweetsample=Proc1,Proc2` | User stack sampler for those processes (every 2 minutes); `sweetsample_delay` (s), `sweetsample_ms`, `sweetsample_n` for a fast profile |
| `sweetgpu=3` | Bring up the Adreno 630 and keep it up for clients (`1`/`2` stop at the GMU / command processor) |
| `sweetmtl=2` | GPU-rendered surfaces: SweetMetal loads SweetMetal2.bundle into CoreAnimation; the framebuffer flips and blits on the GPU |
| `sweetgpuclk=N`, `sweetgpufreq=N` | GPU level 1–7 (257–710 MHz, default 7); `sweetgpuclk=0` leaves the clock to the GMU |
| `sweetgpuirq=0` | Poll for GPU fences instead of the interrupt |
| `sweetfb_flip=0`, `sweetfb_gpublit=0`, `sweetfb_async=0` | Turn off the scan-out flip / GPU blit / non-blocking present (each falls back to the simpler path) |
| `sweetcpufreq=1/0` | Only report the CPU clock / do nothing (default: set both clusters to their top level) |
| `sweettop=N` | Per-process CPU line ("TOP Ns") every N seconds (default 10, 0 off) |

## Debugging without a serial port

- **USB live log:** boot with `sweetusb=2`. On the PC, give the phone's USB network interface `192.168.7.1/24`: a NetworkManager profile matched on the phone's host-side MAC `02:00:5e:77:ee:01` does it without sudo, and so does `tools/sweet-usbnet.sh` with sudo. Then run `tools/sweet-usblog.py logs/NAME.txt` (no sudo). The phone sends the last 64 KiB of the console first, then everything new every 100 ms.
- **Screen:** with `sweetecon=1` and without `sweetfb_noconsole`, the whole kernel console is drawn. It wraps, so the newest line is just above the wrap point.
- **RAM dump:** when the SoC resets into Qualcomm crash-dump mode (`05c6:900e`), build [linux-msm/qdl](https://github.com/linux-msm/qdl) and run `qdl ramdump --debug -o DIR 'DDRCS0_1'`. Then `tools/sweet-ramlog.py KERNEL PAYLOAD DIR/DDRCS0_1.BIN 0x100000000` rebuilds the console from the kernel's log ring. One shot: qdl resets the phone afterwards.
- **Symbolication:** mount the SystemOS cryptex read-only with `apfs-fuse`. Then `tools/dsc-a2s.py CACHE_DIR SLIDE ADDR...` resolves sampler and crash addresses to library + symbol, and `tools/dsc-dis.py CACHE_DIR IMAGE OFFSET` disassembles stripped code and resolves the strings it references.

## The Redmi Note 10 Pro (sweet)

Sweet keeps working from the same tree. Build with `BOARD=sweet` (the default) and `tools/mkadt.py` without `--board`; sweet's device tree output is byte-identical to upstream's. Follow upstream's [README](https://github.com/dipa-studio/ios-sweet) for the sweet-specific steps (OrangeFox log pull, its boot-args). The fajita patches (0082–0089) select their behaviour from the ADT compatible `oneplus,fajita`. They have not been re-booted on a sweet.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm); `board.h` holds the per-board constants, `build.sh` packs the boot image |
| `xnu/patches/` | The XNU patch series (applied in order by CI), plus staging scripts for libpthread, HFS and BearSSL |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `.github/workflows/phone.yml`, `turnip.yml`, `metal2.yml` | Phone programs; Mesa Turnip for iOS; metal2vulkan + SweetMetal2.bundle + mtl2install |
| `phone/` | Programs that run on the phone through the boot launcher (`sweetrun`, `bigrun`): GPU and Vulkan tests, `mtl2install`, `quiet`/`unquiet` |
| `tools/sweet-root-apps.py` | The removable Apple apps and SweetMetal2.bundle into a root image |
| `tools/mkadt.py` | Generates the Apple Device Tree (`--board sweet` or `fajita`) |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `sweet-root-tweaks.py` | Build the HFS+ iOS root from the IPSW; bring-up changes to it |
| `tools/extract-appended-dtbs.py` | DTBs appended to a Qualcomm `Image.gz-dtb` |
| `tools/sweet-ramlog.py` | Console ring out of a RAM dump |
| `tools/dsc-a2s.py`, `dsc-dis.py` | Shared-cache symbolication and disassembly (iOS 16 split cache) |
| `tools/sweet-usblog.py` | Receive the USB live log without sudo |
| `tools/sweet-log-pull.sh`, `sweet-log.py`, `sweet-crash.py`, `qemu-sweet.py`, `adt.py`, … | Upstream helpers |
| `PORTING.md` | OnePlus 6T plan, boot log and hardware facts with their sources |
| `GPU-PLAN.md` | GPU, display and performance log (summary first, then every step with its measurements) |
| `NOTES.md` | Sweet's engineering log; the fajita handoff is at the top |

Not in git: partition backups, live device trees, Apple firmware and root images, RAM dumps and downloaded or generated build trees. They contain device serials and calibration data, or Apple's copyrighted files.

## Kernel patches

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0055 | Diagnostics: persistent log, /dev/null tee, IOKit trace, heap strings and user stack sampler on crash; permissive Sandbox policy; HFS rename-swap; AppleKeyStore selectors |
| 0056–0057 | Throttled panel copy; no external identity resolver |
| 0058 | HID event services: Apple's IOHIDFamily-1915.140.3 user client and event queue |
| 0059 | iPhone X identity (`hw.product` / `hw.target` from the device tree) |
| 0060–0063 | sweet keys over SPMI, touch over GENI I²C, RPMh touch rail, battery power source, Goodix GT9896 |
| 0064–0065 | PRIV_WORK_INTERVAL grant; SMP via PSCI CPU_ON |
| 0066–0069 | Fault-region crash info; console mute; IOSurface lookup fix; batched log flushes |
| 0070–0071 | `pid_hibernate` syscall (SpringBoard app transitions); edge flags for touches near the panel edge |
| 0072–0077, 0081 | USB device mode (DWC3), CDC-ECM network interface, live console log over UDP, link state |
| 0078–0080 | All 8 cores; display power on/off; network configuration trace |
| 0082 | Early console takes the framebuffer from `boot_args.Video` instead of sweet's constants |
| 0083 | `sweetecon=1`: synchronous console straight to the screen |
| 0084 | USB console on fajita: SDM845 USB clock offsets, chosen from the ADT; USB also starts when UFS init fails |
| 0085 | Power the QUSB2 PHY back up when the bootloader left it powered down |
| 0086 | USB DMA in a fixed low-memory window (`sweetusbdma=`, fajita default `0x99800000`) |
| 0087 | fajita keys: PM8998 power (PON), volume up/down (GPIO6/GPIO5) |
| 0088 | fajita touch: Synaptics RMI4, L28A over the SDM845 RPMh mailbox; touch mapping follows the panel height |
| 0089 | fajita: read-only report of the apps-SMMU routing of the USB stream (0x740) |
| 0090 | fajita: route the USB stream through a translation-off SMMU context bank (Linux arm-smmu-qcom bypass quirk), which makes the USB console work |
| 0091 | `sweetbusy=N`: log IOServices that stay busy (registry never quiet); USB data-endpoint events no longer echo into the live log |
| 0092 | IOKit daemon stand-in: release the registry root busy count that waits for kernelmanagerd (absent on iOS), so the registry goes quiet (`sweetiokitd=N`, fajita default 20 s) |
| 0093 | fajita USB cold start (boot from boot_a): RPMh rails, GDSC, clocks, resets, QUSB2 init, DWC3 device mode (`sweetusbinit=0` skips) |
| 0094–0097 | Adreno 630 bring-up: GMU (legacy HFI), RPMh, SMMU, zap shader, command processor, persistent GPU with a 256 MiB arena, submit/fence (`sweetgpu=1..3`) |
| 0098–0101 | NativeAdreno user client `SweetGPU` for Turnip, UBWC, no jetsam limit for the launcher, IOAcceleratorES `SweetMetal` (loads SweetMetal2.bundle) |
| 0102–0106 | Measurement (GPU busy, presents, copy time), DDR/NoC bandwidth votes, GMU perf/BW tables + DCVS, GPU clock / fault / RSC diagnostics |
| 0107–0108, 0110–0111 | GPU core PLL: the GMU starts it for levels it can serve, our kernel latches it otherwise (PLL0 = 2× the core clock); 710 MHz by default |
| 0109 | Copy each GPU-rendered frame inside the swap, after the GPU is done (no more mixed frames) |
| 0112–0117 | Display controller inventory (VIG0, CTL0, PINGPONG0 auto-refresh), display SMMU report, VIG0 flips between two aligned scan-out buffers, GPU 2D blit scaled onto the panel (touch scaled with it); on by default |
| 0118 | GPU interrupt (SPI 300 through `sweet_spi_hook` in sleh_irq) wakes fence waiters |
| 0119–0120 | CPU clock report (OSM tables, measured per core) and both clusters to their top level (big cores were left at 825 MHz) |
| 0121 | IOMobileFramebuffer frame info (selectors 73/74, notification type 3): CoreAnimation's iOS 16 frame clock |
| 0122–0123 | Fast sampler boot-args; non-blocking present (the swap queues the blit, a worker flips); per-process CPU "TOP" line |
| 0124 | A MAC policy named "AMFI" answers lsd's boot-arg state query, so the extension catalog gets read (wallpapers, widgets) |
| 0125–0126 | Sandbox container path for the three Siri voice extensions (PlugInKit's start-up check) |

## Key hardware facts (OnePlus 6T)

- RAM: `0x80000000`+4 GiB and `0x180000000`+`0xfcac0000` on the 8 GB model. Every fixed carve-out is below 4 GiB, so the kernel uses `0x100000000`+`0xfcc00000`.
- GICv3, virtual timer INTID 19, PSCI over SMC, MPIDR = core << 8: the same as sweet. The kernel needs no changes for these.
- Panel: Samsung S6E3FC2X01, 1080×2340, DSI command mode. The bootloader leaves it refreshing from `0x9d400000`.
- configd's InterfaceNamer waits 300 s for Apple's IONetworkStack (absent) before anything can list network interfaces (Settings → About). The root tweaks set its `WaitStackTimeout`/`WaitQuietTimeout` to 5 s. Separately, the macOS-derived kernel config keeps the IORegistry root busy until kernelmanagerd checks in, and iOS has no kernelmanagerd, so patch 0092 stands in for it.
- MobileGestalt needs `/chosen mac-address-wifi0/bluetooth0/ethernet0/1` in the ADT. Without them it waits forever for a Wi-Fi controller, which blocks the LaunchServices seed and so SpringBoard.
- USB: DWC3 v3.00a at `0xa600000`, QUSB2 PHY `0x88e2000`; the bootloader powers the PHY down on handoff. The USB stream is `0x740` on the apps SMMU (`0x15000000`).
- GPU: Adreno 630 at `0x5000000`, GMU `0x506a000`, GPU SMMU `0x5040000` (its stream table is trapped by the hypervisor), interrupt SPI 300. The core clock is gpucc PLL0 (Fabia, an L/FRAC write takes only after a latch) through the gfx3d RCG at gpucc `+0x101c`, and runs at half the PLL. gpucc `0x1024`–`0x1038` traps to crash-dump mode when read.
- Display controller: MDP at `0x0ae01000`. ABL scans the splash buffer with pipe VIG0 through CTL0 and PINGPONG0, with auto-refresh on. The display's SMMU streams (`0x880`, `0xc80`) do not translate. The scan-out buffers live in ABL's 36 MiB splash reservation, because the pipe address register is 32-bit.
- CPU clock: OSM frequency domains at `0x17d43000` (cpus 0–3, up to 1766 MHz) and `0x17d45800` (cpus 4–7, up to 2649 MHz; 2707 single-core turbo); the level is set by writing `PERF_STATE` (`+0x920`).

More, with sources, in [`PORTING.md`](PORTING.md).

## Credits

Built on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) and Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), the mainline Linux device trees and drivers for SDM845 and the OnePlus 6T, [postmarketOS](https://postmarketos.org), and [linux-msm/qdl](https://github.com/linux-msm/qdl). The OnePlus 6T measurements published by [edi194/ios-fajita](https://github.com/edi194/ios-fajita) were the source for the volume-key PPIDs, several touch details and the GPU bring-up notes. The GPU path uses [Mesa](https://mesa3d.org) (Turnip; freedreno's register database and 2D blitter as references), [metal2vulkan](https://github.com/steelbrain/metal2vulkan), [UniXNU](https://github.com/UniXNU/UniXNU)'s Mesa patches, and the downstream OnePlus/LineageOS kernel's clock facts. [aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo) showed which background daemons waste CPU. iOS itself is Apple's and is not distributed here.
