# ios-fajita

> [!WARNING]
> **Vibecoded.** All of this port's code, patches, tools and documentation (this README included) were written by an AI. They were then tested on one phone. Nothing here has been reviewed by a human expert. It may be wrong, may break, and may damage your device or erase your data. Use it entirely at your own risk. I am under no obligation to fix bugs, answer issues, or support anyone using it.

**Apple's iOS 16 running on a OnePlus 6T.**

This project runs Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) on the OnePlus 6T (`fajita`, Qualcomm SDM845). On top of the kernel runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and the stock apps.

It is a fork of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the same for the Redmi Note 10 Pro (`sweet`, SM7150). The OnePlus 6T is a second board next to sweet. Both phones boot the **same kernel binary**. The board is chosen at runtime from the generated device tree, and the shim and the device tree are built per board. The engineering log for the 6T is [`PORTING.md`](PORTING.md); sweet's is [`NOTES.md`](NOTES.md).

As of **2026-10-29** the OnePlus 6T boots to the **iOS lock screen and home screen**. Unlocking, the touchscreen, the power and volume keys and all eight CPU cores work. Rendering runs entirely on the CPU; there is no GPU driver.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed (see below), and Apple's security model is mostly switched off. The iOS root lives in the `userdata` partition, so installing it **erases whatever was there**.

## What works on the OnePlus 6T

| Area | State |
| --- | --- |
| Boot | Stock OnePlus bootloader (unlocked) → `fastboot boot` of our shim → XNU. Nothing has to be flashed to the boot partitions |
| CPU | All 8 cores (4× Cortex-A55, 4× Cortex-A75) via PSCI, 16K pages, GICv3 Group 1 interrupts, timers |
| Memory | 3.95 GiB of the upper DDR bank (`0x100000000`) |
| Storage | UFS host driver (shared with sweet); the iOS root is a journaled HFS+ volume filling `userdata` (about 110 GiB), read/write |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer services; CoreAnimation renders in software; frames are copied into the bootloader's splash buffer (`0x9d400000`, 1080×2340). Presented to iOS as an iPhone X panel (1125×2436) |
| GUI | backboardd, SpringBoard, lock screen, home screen, Settings (About shows "OnePlus 6T") |
| Touch | Synaptics RMI4 (S3706B) at I²C `0x20` on QUP wrapper 1 SE4 (`0xa90000`), polled; rail PM8998 L28A voted through RPMh |
| Buttons | Power (PM8998 PON), volume up/down (PM8998 GPIO6/GPIO5), read over SPMI |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |
| USB | Device mode (DWC3) as a CDC-ECM network adapter: phone `192.168.7.2`, PC `192.168.7.1`; the live kernel console streams to the PC over UDP (`sweetusb=2`) |
| Debugging | Live console over USB; kernel console on the screen; console ring in RAM, recoverable from Qualcomm's crash-dump mode with a RAM dump; persistent log at the end of `userdata` |

## What does not work (yet)

- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS, SEP / Face ID, GPU.** Their daemons start and fail or keep relaunching.
- **The clock starts at 1970**, so anything that checks certificates fails.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection. Do not put anything personal on it.
- **Booting without a PC** means flashing the boot image to `boot_a` (`fastboot flash boot_a shim-boot.img`). This works on the author's phone. Whether the A/B retry counter eventually rejects the slot has not been tested over many boots yet (`fastboot set_active a` resets it).

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
| `sweetsample=Proc1,Proc2` | User stack sampler for those processes (every 2 minutes) |

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
| `tools/mkadt.py` | Generates the Apple Device Tree (`--board sweet` or `fajita`) |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `sweet-root-tweaks.py` | Build the HFS+ iOS root from the IPSW; bring-up changes to it |
| `tools/extract-appended-dtbs.py` | DTBs appended to a Qualcomm `Image.gz-dtb` |
| `tools/sweet-ramlog.py` | Console ring out of a RAM dump |
| `tools/dsc-a2s.py`, `dsc-dis.py` | Shared-cache symbolication and disassembly (iOS 16 split cache) |
| `tools/sweet-usblog.py` | Receive the USB live log without sudo |
| `tools/sweet-log-pull.sh`, `sweet-log.py`, `sweet-crash.py`, `qemu-sweet.py`, `adt.py`, … | Upstream helpers |
| `PORTING.md` | OnePlus 6T plan, boot log and hardware facts with their sources |
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

## Key hardware facts (OnePlus 6T)

- RAM: `0x80000000`+4 GiB and `0x180000000`+`0xfcac0000` on the 8 GB model. Every fixed carve-out is below 4 GiB, so the kernel uses `0x100000000`+`0xfcc00000`.
- GICv3, virtual timer INTID 19, PSCI over SMC, MPIDR = core << 8: the same as sweet. The kernel needs no changes for these.
- Panel: Samsung S6E3FC2X01, 1080×2340, DSI command mode. The bootloader leaves it refreshing from `0x9d400000`.
- configd's InterfaceNamer waits 300 s for Apple's IONetworkStack (absent) before anything can list network interfaces (Settings → About). The root tweaks set its `WaitStackTimeout`/`WaitQuietTimeout` to 5 s. Separately, the macOS-derived kernel config keeps the IORegistry root busy until kernelmanagerd checks in, and iOS has no kernelmanagerd, so patch 0092 stands in for it.
- MobileGestalt needs `/chosen mac-address-wifi0/bluetooth0/ethernet0/1` in the ADT. Without them it waits forever for a Wi-Fi controller, which blocks the LaunchServices seed and so SpringBoard.
- USB: DWC3 v3.00a at `0xa600000`, QUSB2 PHY `0x88e2000`; the bootloader powers the PHY down on handoff. The USB stream is `0x740` on the apps SMMU (`0x15000000`).

More, with sources, in [`PORTING.md`](PORTING.md).

## Credits

Built on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) and Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), the mainline Linux device trees and drivers for SDM845 and the OnePlus 6T, [postmarketOS](https://postmarketos.org), and [linux-msm/qdl](https://github.com/linux-msm/qdl). The OnePlus 6T measurements published by [edi194/ios-fajita](https://github.com/edi194/ios-fajita) were the source for the volume-key PPIDs and several touch details. iOS itself is Apple's and is not distributed here.
