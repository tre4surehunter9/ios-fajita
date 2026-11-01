# GPU acceleration on the OnePlus 6T (Adreno 630)

Goal: Metal (and through it CoreAnimation, so the whole UI) on the Adreno 630, the way
[ios-flame](https://github.com/NWylynko/ios-flame) does it on the Pixel 4's Adreno 640 (same a6xx family,
same ios-sweet base, same iOS 16.7.16 20H392 userland): an in-kernel GPU core plus Mesa's Turnip
(Vulkan) on iOS, then metal2vulkan and a Metal plug-in bundle.

Decisions (user, 2026-10-29): the zap shader comes from postmarketOS (open source); all builds,
userland included, run on GitHub Actions (no Mac available).

## Status (2026-11-01): GPU milestone done, paused here (user)

The home screen and apps are composited by the Adreno 630 through Metal. It is "decent smooth" (user) but not
60 fps: 21-24 presents/s while scrolling.

| Piece | Patches | State |
| --- | --- | --- |
| GPU bring-up (GMU, RPMh, SMMU, zap, CP, arena, fences) | 0094-0097 | done, `sweetgpu=3` |
| Turnip (Vulkan 1.3) through `SweetGPU`, UBWC | 0098-0099 | `VKTEST ALL PASS` |
| Metal for CoreAnimation: `SweetMetal` + SweetMetal2.bundle (metal2vulkan + Turnip) | 0101 | home screen on the GPU |
| GPU clock: GMU perf table + DCVS, PLL0 latched by us when the GMU leaves it unlocked | 0104, 0107-0111 | 9.6 -> **710 MHz** |
| Frames copied at swap time (no mixed frames) | 0109 | artifacts gone |
| Present path: VIG0 flips between two aligned scan-out buffers, GPU 2D blit scales 1125x2436 onto 1080x2340, touch follows | 0112-0117 | CPU copy gone, edges fit (Phase 5) |
| GPU interrupt wakes fence waiters | 0118 | irqs = submits |
| CPUs to their top OSM level | 0119-0120 | big cores 825 -> **2649 MHz** |
| IOMobileFramebuffer frame info (iOS 16 CoreAnimation frame clock) | 0121 | backboardd subscribes |
| Non-blocking present, fast sampler, per-process TOP | 0122-0123 | **14 -> 21-24 presents/s** |
| Background load: `phone/quiet` (vmd, fairplayd.H2, ReportCrash off), `phone/unquiet` (bluetoothd back) | - | idle CPU ~160 % -> 6-12 % of a core |

Where the rest of the frame time goes: backboardd's render thread is CPU-bound during use (TOP: 50-108 % of a
core) and its busy samples land in SweetMetal2/Turnip code (outside the shared cache, not symbolicated yet).
Open options, to ask the user about after PosterBoard:
- the sampler logs where SweetMetal2/Turnip load, so their hot functions can be named;
- three scan-out buffers;
- UBWC for the surfaces;
- ios-apollo's OpenGL ES bridge as a comparison (43-54 fps there, but Metal-only parts break).

Disabled jobs to restore with their features: bluetoothd is on again; vmd + CommCenter (cellular),
fairplayd.H2, ReportCrash stay off. The original job list is `/System/Library/xpc/launchd.plist.pre-quiet`
on the phone.

Booting: `fastboot boot` sometimes fails with "Load Error" (even known-good images, even after a cold start).
Flash the image to boot_a, then `fastboot continue`. The USB log only comes up when started from fastboot.

## Sources and how each may be used

| Source | Licence | Use |
| --- | --- | --- |
| [ios-flame](https://github.com/NWylynko/ios-flame) (Pixel 4, A640) | MIT | Code: kernel GPU core (`sweet_a6xx.c`), `/dev/pmem`, accelerator service + user client (patches 0095-0124), `phone/turnip`, `phone/metal2`, build scripts. With attribution |
| [edi194/ios-fajita](https://github.com/edi194/ios-fajita) (this phone, A630) | none (all rights reserved) | Facts only: A630 register values, power sequence, what broke (patches 0099-0174, NOTES-FAJITA.md). No code |
| Mainline Linux `drivers/gpu/drm/msm/adreno/a6xx_gmu.c`, `a6xx_gpu.c`, `sdm845.dtsi` | GPL-2.0 | Facts and sequences. No code |
| [iOSPlus10](https://github.com/akshaynexus/iOSPlus10) (A730) | see repo | Design notes (`docs/gpu-*.md`, `metal-design.md`), comparison of all ports |
| [metal2vulkan](https://github.com/steelbrain/metal2vulkan) | GPL-2+ or LGPL-3+ | Built as ios-flame does |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (Turnip, freedreno `fd6_blitter.cc`, `freedreno_devices.py`, register XML) | MIT | Turnip built and patched; the 2D blit sequence and A630 values as facts |
| Downstream OnePlus/LineageOS `android_kernel_oneplus_sdm845` (`gpucc-sdm845.c`, `clk-alpha-pll.c`) | GPL-2.0 | Facts only: PLL0/gfx3d RCG layout, Fabia latch |
| Mainline `qcom-cpufreq-hw.c`, DPU catalog (`dpu_4_0_sdm845.h`, `dpu_hw_*.c`) | GPL-2.0 | Facts only: OSM tables, MDP layout |
| iOS 16 IOMobileFramebuffer / QuartzCore (shared cache, 20H392) | Apple | Read for interoperability only: selector numbers, the frame-info protocol |
| [aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo) (A650) | see repo | Findings only: background CPU hogs, present design |

## Firmware (outside git, `research/gpu/fw/`)

| File | Source | sha256 |
| --- | --- | --- |
| `a630_zap.mbn` | postmarketOS `firmware-oneplus-sdm845` @ `176ca71` (pinned by pmaports), `lib/firmware/qcom/sdm845/oneplus6/` | `97809567…80df9` |
| `a630_sqe.fw` | linux-firmware `qcom/` | `1c21b527…aa90c` |
| `a630_gmu.bin` | linux-firmware `qcom/` | `da8d9b1b…173a6` |

The OnePlus 6 and 6T share the zap (mainline's fajita DT uses the enchilada paths).

## Phases

1. **Kernel: GPU powered and running microcode.** CX/GX rails and GDSCs, GPU clocks, GMU boot with
   `a630_gmu.bin` (legacy GMU on A630: different from A640), HFI, SMMU context for the GPU, SQE microcode,
   zap shader through TrustZone (`a630_zap.mbn`), then a first command-processor submit (a fence
   write). Template: ios-flame's core; A630 differences from mainline and edi194's notes. Logged over
   the USB console (fastboot boot) and the RAM dump on SoC resets.
2. **Kernel: userland interface.** ios-flame's `/dev/pmem`, IOAccelerator service, the user client
   that Turnip's msm-style backend talks to, buffer objects, fences, GPU present into the panel.
3. **Userland via GitHub Actions:** Turnip for iOS (`phone/turnip`), metal2vulkan bridge and
   `SweetMetal2.bundle` (`phone/metal2`), plus the small test programs (`vktest`, `m2vtest`). Add them
   to the root image (root tweaks or a separate overlay step).
4. **System-wide:** `sweetmtl=2`, so CoreAnimation renders through Metal; measure, then fix.

Each phase gets its own boot-arg gate, so a broken GPU stage never stops the CPU-rendered GUI from booting.

## Phase 2 plan: the GPU usable from iOS userland (written 2026-10-30)

Goal: a process on the phone allocates GPU memory, submits a command buffer and waits on a fence; then
Mesa's Turnip (Vulkan) clears an image on the A630. That is the base metal2vulkan (Phase 3) needs.

Template: ios-flame's stack (MIT): kernel core + IOKit accelerator service + user client speaking the
**NativeAdreno ABI v1** (UniXNU, MIT; `phone/turnip/NativeAdrenoABI.h`), which Turnip's "darwin" kernel
backend uses unchanged. Our A630 power-up (legacy GMU, HFI v1) stays ours; ring, page-table, submit and
user-client code can come from ios-flame with A640 -> A630 changes.

**Constraint from edi194 (same kernel lineage):** XNU's page allocator here can return pages below gPhysBase
(`kmem_alloc_contig`, and `IOBufferMemoryDescriptor` in their user client), and the first pmap operation on
one panics (`phys_to_kv_range: illegal PA`). So GPU memory comes from **our own arena in the low DDR bank**,
outside XNU's RAM (XNU runs at 0x100000000; the low bank above 0xb0000000 up to rmtfs at 0xfde01000 is unused
by XNU, ~1.2 GiB), mapped write-combining like today's pool. Step 2.1 measures the exact free range from the
live FDT before choosing a size; whether our tree has the allocator bug at all is a side check, not a
dependency.

Steps, each its own patch and phone test (`fastboot boot`, USB log):

- **2.1 Persistent GPU.** Bring-up once at boot and keep GX up (no slumber), one lock around the GPU, ring
  with a real fence (`CP_EVENT_WRITE CACHE_FLUSH_TS` into a fence page), `CP_INDIRECT_BUFFER` submits,
  timeout + recovery (re-run the ladder after a hang). The arena: allocator for buffer objects, GPU page
  tables with 4 KiB pages for the CP context, CPU mapping WC. Kernel self-test: an IB that writes memory,
  fence waited. Gate: `sweetgpu=3`.
- **2.2 Accelerator service + user client.** NativeAdreno ABI v1: GetDeviceInfo (chip_id 0x06030001, GMEM
  1 MiB at 0x100000, UCHE trap base 0x1fffffffff000, UBWC per a630), BoAlloc / BoMap at the client's IOVA /
  BoCpuMap (WC) / BoImport (later), Submit (N IBs + kernel-side cache flush), WaitFence, per-connection VA
  windows (Turnip chooses addresses). Test tool `gputest` (C, built on CI): alloc, map, submit an IB that
  writes a value, wait, read it back from userland.
- **2.3 Turnip for iOS on GitHub Actions.** A workflow (macos-15, Xcode 16 iPhoneOS SDK, meson) that builds
  Mesa at ios-flame's pin with its darwin-KMD patches for arm64-apple-ios16.0, plus `vktest` (create device,
  clear an image, read back). Put the dylib and the test on the root (root tweak or overlay). **Milestone:
  vktest's clear is read back correct on the phone.**
- **2.4 Presentation path** (from ios-flame 0095/0119): GPU blit of a surface to the panel framebuffer, so a
  Vulkan image can reach the screen. Needed by Phase 3, can slip until then.

Then Phase 3 (metal2vulkan + SweetMetal2.bundle on CI) and Phase 4 (`sweetmtl=2`) as above.

**metal2vulkan (Phase 3) is the user's priority** (clarified 2026-10-30: "vulkan2metal" meant metal2vulkan,
Metal implemented on Vulkan, the stack ios-flame runs).

## Phase 3 plan: Metal on the GPU through metal2vulkan (written 2026-10-31)

Goal: backboardd composites the home screen through Metal on the A630, then SpringBoard and the rest.
Stack (ios-flame's, which renders SpringBoard and apps this way on the A640): Metal.framework finds an
**IOAcceleratorES** service whose `MetalPluginName` names a bundle in /System/Library/Extensions, loads it and
asks its `MetalPluginClassName` for a device. **SweetMetal2.bundle** = UniXNU's Metal frontend
(NativeBridgeMetalFrontend.mm, MIT, patched for iOS 16 by ios-flame) + the **metal2vulkan bridge** (Rust static
library: steelbrain/metal2vulkan 720ef1ce, GPL-2.0+/LGPL-3.0+, + UniXNU's and ios-flame's patches; AIR -> SPIR-V
on the phone, the Metal API on Vulkan) + **Turnip** (Phase 2) inside the bundle. CoreAnimation's ~270 pipelines
are translated and compiled on the phone (3-30 ms each), cached per process.

Steps, each its own build and phone test:

- **3.1 The bridge on CI + m2vtest.** A workflow job with Rust 1.98 (`aarch64-apple-ios`, deployment 16.0) and
  llvm-as (bridge fixtures) builds `libnative_metal_bridge.a` from metal2vulkan 720ef1ce + the patches in
  phone/metal2/bridge/patches. `m2vtest` (the bridge's self-test: translate a Metal shader and run it on
  Turnip) rides in a boot image like vktest, Turnip embedded. Proves the translator and runtime on our GPU
  before Metal.framework is involved.
- **3.2 Kernel 0101.** `sweetmtl=2` (on top of `sweetgpu=3`) publishes the GPU service as an `IOAcceleratorES`
  with `MetalPluginName` SweetMetal2 / `MetalPluginClassName` SweetMTL2Device (ios-flame 0116); without it Metal
  finds no device and CoreAnimation keeps its CPU renderer, as now. **Surface coherence:** our IOSurfaces are
  cacheable kernel memory (SweetSurface, IOBufferMemoryDescriptor) and SweetFramebuffer copies them to the panel
  with the CPU; once the GPU writes them (BoImport), the copy must invalidate the surface range first.
- **3.3 SweetMetal2.bundle on CI + installing it without a root rebuild.** The bundle builds from the bridge,
  the frontend and Turnip. A small installer program rides in a boot image (sweetrun runs it as root, the root
  is writable): it unpacks the bundle into /System/Library/Extensions, writes /var/local/mtl2-clients
  (backboardd first) and restarts backboardd so it picks up the device. The bundle then stays on the root.
- **3.4 Milestone: the home screen composited by the GPU.** backboardd through Metal (MTL2 log lines,
  pipelines compiled on the phone, a photo), then SpringBoard (the unlock swipe needs it), then every process.
- **Phase 5 (user, 2026-10-31): the screen fits the panel.** The panel is 1080x2340; iOS gets 1125x2436
  (iPhone X) and SweetFramebuffer copies 1:1 with an offset (fOffX/fOffY), so ~45 px of width and ~96 px of
  height are cut off. (1) The GPU present blit (Phase 4's CPU-copy replacement) scales 1125x2436 -> 1080x2340
  (0.96) in the same pass: nothing cut off, no 3.8 ms CPU copy. (2) Later, as an experiment: report the native
  1080x2340 (framebuffer + MobileGestalt main-screen-width/height/scale); iOS 16 layouts may not accept a
  non-iPhone size.
- **After 0118 (user asked what else speeds it up, 2026-10-31).** GPU busy is 1-17 % at 710 MHz, so the rest is
  CPU-side: (1) **CPU clock**: nothing drives the SDM845 CPU DVFS (cpufreq-hw/OSM), the cores likely run at ABL's
  clock; measure first (PMU cycle counter vs the timer), then drive it. (2) **Don't block the swap on the
  flip**: three scan-out buffers, return after the blit, flip latches in the background. (3) Profile
  backboardd/SpringBoard (sampler). (4) UBWC frame buffers etc., small while the GPU idles.
- **When the GPU work ends (user, 2026-10-31): docs pass before anything else.** README.md (what fajita
  does now, build/boot recipe), PORTING.md (GPU phases done, hardware facts: GMU/RPMh/PLL0/MDP, what is open),
  GPU-PLAN.md condensed to a summary + patch list (0094..), then fast-forward main to the GPU milestone.
- **After the GPU work (user, 2026-10-31): the wallpaper.** PosterBoard (iOS 16 wallpaper renderer) dies
  with SIGTRAP on start, on CPU- and GPU-rendered boots alike, so the wallpaper is black; root v5 does not
  change that. Needs its crash log symbolicated (tools/dsc-a2s.py).
- **After the GPU work (user, 2026-10-31): all of the DDR.** The phone is the 8 GB 6T (Sailfish /proc/iomem:
  System RAM up to 0x27cabffff); XNU uses 0x100000000 + 0xfcc00000 (~3.95 GB, mkadt.py --highmem). First extend
  the high range to ~0x27cac0000 (~5.9 GB: no fixed reservations above 4 GB in the live DT; edi194's XNU on this
  phone ran with size 0x17c7a0000), then look at the low bank (carve-outs; part of it is the GPU pool/arena).
- **Before Wi-Fi: the battery gauge (user, 2026-11-01).** The phone ran flat with no warning during a boot.
  bq27411 at I2C10 `0xa88000` addr `0x55` (mainline, unverified here; PORTING.md); sweet has a battery power
  source service to build on (patches 0060-0063). CPUs stay locked at their top level (0120), so clock scaling
  when idle belongs with this too.
- **Before Wi-Fi, after the battery gauge: SSH over USB (user, 2026-11-01).** `ue0` (192.168.7.2) is a real ifnet in
  bsd/dev/sweet_usb.c, so no kernel work: dropbear, dash and toybox built by the `phone` workflow (Bergamota's
  docs/SSH_OVER_USB.md uses the same upstream software), installed by a run-once program plus a launchd job.
  Key-only, USB link only.
- **Then Wi-Fi (user, 2026-10-31).** WCN3990: its firmware runs on the modem DSP, so: boot the modem (firmware
  loaded into RAM from the stock images, partitions never written), QRTR/QMI, an ath10k-style driver, and an en0
  for iOS's Wi-Fi stack; ios-flame's `wland` path (phone/wifi, kernel 0123) is the reference.
- **Afterwards (Phase 4):** frame rate and presentation (the GPU blit to the panel instead of the CPU copy,
  ios-flame 0095/0119), memory per process, apps.

Licences: our code and ios-flame's/UniXNU's MIT files keep MIT with attribution; the metal2vulkan patches
fall under its licence, and a built SweetMetal2.bundle links it statically (GPL-2.0+ or LGPL-3.0+ terms).

## Log

- **Stage 1 (shim, 2026-10-29):** `GPUPROBE=1 BOARD=fajita shim/build.sh` builds a no-payload shim that runs
  `shim/gpuprobe.c`: GCC GPU clocks, CX GDSC on, gmu_clk_src at XO, cxo/cx_gmu/ahb clocks, a GMU
  register read and an ITCM write/read-back test. Output on screen; no SMMU or GX access. Boot it
  with `fastboot boot shim/out-fajita-gpu/shim-boot.img` (iOS is not involved).
- **Stage 1 result (2026-10-29): PASS.** CX GDSC `0x00222001` → `0xF8222000`; gmu_clk_src XO (cfg 1, update
  done); gpucc cxo/cx_gmu (`0x7F21`)/ahb on; GCC gpu_cfg_ahb and ddrss_gpu_axi on; GCC gpu_memnoc_gfx
  (`0x80004221`) and gpu_snoc_dvm_gfx (`0x80000001`) stay halted (GX side not up: expected). GMU SYSRESET 1,
  FW_INIT_RESULT 0. **ITCM write/read-back OK** (old `0xB189F2A3`, wrote/read `0xA5A5F00D`).
- **Stage 2 result (2026-10-30, cold boot): PASS, the GMU firmware runs.** gmu_clk 200 MHz (cfg `0x602`,
  GCC vote `0x01619D7B`), 0x2000 firmware words written, 0 of the sampled words bad. Every register in the
  list reads back as written. After SYSRESET release: **`FW_INIT_RESULT = 0xBABEFACE`**, SYSRESET 0, then
  the GMU is parked in reset again. Next: stage 3, the GPU SMMU (vendor impl-defs, exact SMRs for streams
  5/4, context bank 0) + HFI queues, then the HFI INIT/FW_VER/TEST handshake.
- **Stage 3 (shim, written 2026-10-30):** GPU SMMU (micro-MMU halt + attach-impl-defs, CB0 at +0x8000 with
  our 4 KiB tables in a 64 KiB pool at PA 0xb0000000, S2CR 0x300 + exact SMRs for streams 5/4, TLB sync),
  mainline HFI v1 table (3 queues, legacy ids 0/4/5) mapped at 0x60100000, GMU boot, then
  HFI_CTRL_INIT → status 1 → INIT / FW_VERSION / TEST each waiting for an ACK. Needs a cold boot.
- **Stage 3 first run (2026-10-30): SoC reset into dump mode.** The framebuffer, read back from a RAM dump
  (`qdl ramdump 'DDRCS0_0'`, fb at 0x9d400000), shows micro-MMU ctrl 0x2378 / sCR0 0x200626 and CB0 SCTLR
  0xC01067 / TCR 0x802020, then nothing: the first S2CR/SMR write reset the SoC. The SMMU's bus clock (gcc
  gpu_memnoc_gfx) was halted (stage 1). Reordered: boot the GMU first (edi194's order), check memnoc_gfx, and
  only then touch the stream table.
- **Stage 3 second run (2026-10-30): same reset, but memnoc_gfx running** (`cbcr 0x4221`, snoc `0x1`), so the
  clock was not the cause. The disassembly was: the S2CR read-back compiled to `ldr w4, [x20], #4`
  (post-index writeback). The hypervisor traps the GPU SMMU's stream table (sCR0 bit 21) and emulates
  accesses from the abort syndrome; writeback forms have no valid syndrome (ISV=0), so it resets the SoC.
  **Rule: every MMIO access the GPU code makes to the SMMU must be a plain `ldr/str [xN]`** (the shim now
  uses inline-asm accessors; the kernel patches must do the same, never plain volatile pointers).
- **Stage 3 result (2026-10-30): PASS, the GMU talks HFI through our SMMU.** SMR0 `0x80000005`, SMR1
  `0x80000004`, both S2CR `0x300`, sGFSR 0, TLB sync 0. `HFI_CTRL_STATUS` 1 (version info 0 = legacy v1).
  INIT ACK `0x0002047E` error 0; FW_VERSION ACK `0x0012047E`, **firmware version `0x10060000` (1.6)**; TEST ACK
  `0x0022047E` data `0x0E550E55`. GMU parked in reset after. Next: stage 4, the GX side (GMU boot OOB / GX
  rail on, SPTPRAC, then GPU core registers RBBM_STATUS / CP_ALWAYS_ON readable).
- **Stage 4 (shim, written 2026-10-30):** PDC/RSC microcode (mainline a630 values; PDC 0xb280000, PDC seq
  0xb480000, RSCC GMU+0x23000; gfx ARC address 0x30050 from this phone's cmd-db per edi194, not Linux's 0x30080)
  and RSC wake before the firmware load; after HFI: boot OOB with gfx/mx ARC index 3 (257 MHz, edi194's
  cmd-db reading), SPTPRAC on (`0x778000`, status & 0x38 == 0x28), GX GDSC check, then RBBM_STATUS and the
  GMU always-on counter (only if SPTPRAC is up and the GX GDSC is on), then the legacy slumber. The ARC
  index and gfx address are hardcoded from edi194's log, not read from cmd-db by us (to do in the kernel).
- **Stage 4 result (2026-10-30): the GPU core answers.** PDC enabled (`0x80000001`); RSC wake ack 0, sequencer
  idle (as edi194). Boot OOB acked: pstate `0xF`, SPTPRAC `0x28`; GX GDSCR `0xF8222000` (on by itself);
  **`RBBM_STATUS = 0x1`** (idle) with no bus hang. The always-on tick count and the slumber detail ran off the
  screen edge; the slumber reported incomplete. Labels shortened and the slumber split into SPTPRAC-off and
  OOB lines for the next run.
- **Stage 4 re-run (2026-10-30):** GMU always-on counter `0x948E` = 38030 ticks in 2 ms ≈ 19.0 MHz (the 19.2 MHz
  XO; ticking). SPTPRAC off OK (status `0xD4`, bit 2), GX GDSCR `0x00222801` (collapsed, retention bit 11 set).
  Slumber OOB **not acknowledged**: RPMH_POWER_STATE `0xF` (slumber), GMU2HOST_INTR_INFO `0x00800000` =
  **CM3_FAULT** (bit 23): the GMU firmware faulted on the slumber request. edi194 saw the same missing ACK on
  this phone (state 0xF, treated as done). Suspect: the RSC wake ACK that never comes. Not blocking (the GMU
  is reset afterwards); must be fixed before the kernel driver idles the GPU at runtime.
- **Kernel patch 0094 (written 2026-10-30):** `bsd/dev/sweet_a630.c`, called from `sweet_ufs_init` after the root is
  found; runs only with `sweetgpu=1` on fajita, in its own thread. Stages 1-4 of the shim probe, same values:
  clocks + CX GDSC + gmu_clk 200 MHz, PDC/RSC, GMU firmware from `/chosen` `gpufw,a630_gmu.bin`
  (`tools/mkpayload.py --gpufw research/gpu/fw`), SMMU CB0 + SIDs 5/4, HFI INIT/FW_VERSION/TEST, GX rail +
  SPTPRAC + RBBM_STATUS, slumber, GMU back in reset; logs `SWEET GPU:` lines. GMU memory: 64 KiB at 0xb0000000
  (low bank, outside XNU's RAM; `sweetgpupool=` overrides) mapped write-combining like the USB DMA window.
  Register access only through inline-asm plain `ldr/str [xN]`. Without `sweetgpu`, behaviour is unchanged.
- **CI on macos-15 (2026-10-30): success** (run 41116893474, Xcode 16 with patch 0095; 0094 included). Test
  image `shim/out-fajita/shim-boot-0095g.img`: kernel `xnu/out-0095`, ADT `fajita-0093.adt` (as on boot_a),
  `--gpufw research/gpu/fw`, the standard boot-args + `sweetgpu=1`. `fastboot boot` only, cold start first.
- **0094 on the phone (2026-10-30, image 0095g, USB log): PASS, the whole ladder runs inside iOS in 119 ms.**
  CX GDSC `0x00222001` → `0xF8222000`; gpu_memnoc_gfx / snoc_dvm_gfx report HALTED at that point (as in the shim,
  harmless); gmu_clk 200 MHz; PDC `0x80000001`, RSC no ack; firmware 8192 words, 0 bad (full read-back);
  `0xBABEFACE`; SMR0/1 `0x80000005`/`0x80000004`, S2CR `0x300`, sGFSR 0; HFI up, INIT `0x2047E` err 0,
  FW_VERSION 1.6, TEST `0x0E550E55`; GX rail on, SPTPRAC `0x28`, GX GDSC on; **RBBM_STATUS 0x1**, counter ticking;
  slumber as in the shim (SPTPRAC off, OOB not acked, CM3 fault); GMU back in reset. iOS then boots to
  SpringBoard as usual. Phase 1 next: the CP (SQE, CB1 for SID 0, zap via SCM PAS 13, first submit).
- **Kernel patch 0096 (written 2026-10-30): the CP, `sweetgpu=2` only** (`sweetgpu=1` is unchanged). Pool grows to
  256 KiB at 0xb0000000: CB1 tables at +0x10000, ring +0x14000 (GPU VA 0x40000000, 32 KiB), scratch +0x1c000
  (0x40020000), SQE +0x20000 (0x40010000, read-only, first dword dropped as a6xx_ucode_load), zap metadata
  +0x30000. CB1: CBAR/CBA2R(1), ASID 2, SMR2 = VALID|0, S2CR2 0x301. OOB GPU_SET (legacy 16/24), mainline's a630
  hw_init subset (same list and values as edi194's 0144), RB_CNTL 0x0800020c, SQE_CNTL 1, CP_ME_INIT (8 dwords),
  zap from `a630_zap.mbn` (metadata = first segment + hash segment, as qcom_mdt_load; PAS 13 INIT_IMAGE /
  MEM_SETUP 0x8c415000 / AUTH_AND_RESET, SMC with the QCOM_SCM_INTERRUPTED resume), CP_SET_SECURE_MODE 0,
  NOP + CP_MEM_WRITE 0x5ee70001 + CACHE_FLUSH_TS 0x600d0001 read back from scratch, PAS SHUTDOWN, GPU_SET clear,
  then the existing slumber.
- **0096 on the phone (2026-10-30, image 0096g, `sweetgpu=2`): PHASE 1 COMPLETE, the GPU runs our commands.**
  CB1 SMR2 `0x80000000` / S2CR2 `0x301`, sGFSR 0; OOB GPU_SET acked; CP before: all zero, cp_status1 `0x1000000`;
  configured rb_cntl `0x800020c`, sqe_base `0x40010000`, rb_base `0x40000000`; SQE on (`0x81004000`);
  **CP_ME_INIT rptr = wptr = 9**; zap from the pmOS `.mbn`: INIT_IMAGE (6684-byte metadata) / MEM_SETUP 0x8c415000
  size 0x1000 / AUTH_AND_RESET all `0 result 0`; CP_SET_SECURE_MODE 0 (rptr 11); NOP + MEM_WRITE +
  CACHE_FLUSH_TS (rptr 21): **mem_write 0x5ee70001, cache_flush_ts 0x600d0001, rbbm_int_0 0, hw_fault 0**; zap
  shutdown 0. With GPU_SET cleared first, **the slumber OOB is now acked and there is no CM3 fault** (the fault
  in stage 4 came from slumbering without the GPU_SET/clear pair). Whole bring-up 163 ms; iOS reaches
  SpringBoard. USB came up later than usual on this boot (user saw it after a few minutes); not yet explained.
- **Kernel patch 0097 (written 2026-10-30): Phase 2.1, `sweetgpu=3`.** After the CP test passes: no zap shutdown,
  GPU_SET kept, no slumber, GMU left running. CB1 widened to T0SZ 25 (39-bit GPU VA). Arena 256 MiB at
  0xb1000000 (`sweetgpuarena=` size, max 1 GiB; free per the live DT: nothing reserved above 0xad200000 except
  Linux-only dynamic pools), WC: first 4 MiB page tables (on-demand L2/L3), then 64 KiB buffer chunks. Kernel API
  for 2.2: `sweet_gpu_bo_alloc/free`, `sweet_gpu_map/unmap` (4 KiB, CB1 TLB flush), `sweet_gpu_submit`
  (CP_INDIRECT_BUFFER + CACHE_FLUSH_TS fence at scratch+0x100, ring-space wait), `sweet_gpu_wait` (hang report
  with CB1 FSR/FAR). Self-test: 100 sequential IBs (two MEM_WRITEs each) + 50 queued IBs, then READY.
- **0097g on the phone (2026-10-30): kernel panic**, `kmem_alloc(…, 268435456, 0x100021): failed with 3
  @vm_kern.c:178` in kernel_task: the 256 MiB `ml_io_map_wcomb` of the whole arena (KERN_NO_SPACE in the I/O
  mapping space; that allocation panics instead of failing). Fix folded into 0097: only the first 16 MiB (page
  tables + kernel buffers) get a kernel mapping; userland buffers come from the rest and are zeroed through a
  temporary mapping; 2.2 maps them into the client tasks.
- **0097h (2026-10-30): same panic with 16 MiB** (`kmem_alloc(…, 16777216, …) failed with 3`). Cause:
  `ml_io_map*` allocate from `io_submap`, which is **8 MiB for all drivers together** (osfmk/arm/io_map.c,
  `IO_MAP_SIZE`). Fix in 0097: the arena is mapped whole with `ml_io_map_unmappable(pa, size, VM_WIMG_WCOMB)`
  (kernel_map, KMA_DATA; ios-flame maps its GPU windows the same way). Rule: anything big goes through
  ml_io_map_unmappable, never ml_io_map/ml_io_map_wcomb.
- **0097 on the phone (2026-10-31, image 0097g, `sweetgpu=3`): PHASE 2.1 PASS.** CP test as before, then
  `persistent: arena 0xb1000000-0xc1000000 (256 MiB, kernel_map WC), cp context 39-bit`; **self-test sequential
  100/100 IBs correct, 51 us each; queued 50/50 correct, fence 150**; `READY`, bring-up 145 ms, GPU kept up, and
  iOS reaches SpringBoard with GX on. CB1 FSR reads `0x400` (bit 10, not one of the fault bits 1-8; to look up).
  Next: 2.2, the IOKit accelerator service + NativeAdreno user client.
- **Kernel patch 0098 (written 2026-10-31): Phase 2.2 kernel side.** `iokit/Kernel/arm/SweetGPU.cpp`: service
  `SweetGPU` on IOResources (sweetgpu>=3, fajita; deliberately not IOAcceleratorES, so Metal ignores it until
  Phase 3), user client `SweetGPUUserClient` speaking NativeAdreno ABI v1 (`SweetGPUABI.h`, MIT, unchanged):
  per-connection 4 GiB windows from 0x2_0000_0000, BoAlloc from the arena via `withPhysicalAddress` (WC to the
  CPU through clientMemoryForType, token = handle; mapped-while-freed memory waits as a zombie, as ios-flame),
  BoImport (pinned caller pages), BoMap (4 KiB page lists), Submit (<=16 IBs, CCU depth/colour invalidate +
  CACHE_FLUSH_TS, as msm's a6xx_submit), WaitFence (a timeout is no longer a hang), timestamp = GMU AO counter.
  GetDeviceInfo: chip 0x06030001, GMEM 1 MiB @0x100000, trap base 0x1fffffffff000; **UBWC left unconfigured
  (version 0): to settle before Turnip (2.3)**. Also the ios-flame pmap fix (`pmap_sync_page_attributes_phys`
  lost bit 32 of page addresses above 4 GiB).
- **Test programs without root rebuilds (2026-10-31, user chose the launcher):** `phone/sweetrun` is installed
  once on the root (`tools/sweet-root-tweaks.py --sweetrun`, LaunchDaemon `com.sweet.run`, RunAtLoad, once;
  `work/update-root-v4.sh` makes root v4 from v3). At boot it reads `/chosen` `sweetrun` through the
  IODeviceTree plane (every ADT property is in the registry unless the node is kernel-only), writes it to
  /private/var/tmp/sweetrun/<name> and runs it with stdout/stderr on /dev/console (USB log). Programs go into
  boot images with `tools/mkpayload.py --run PROG`. `phone/gputest` exercises the whole ABI from userland
  (Query, DeviceInfo, BoAlloc + CPU map + BoMap, 100 single submits, one 16-IB submit, BoImport of its own
  page, timestamp/status) and ends with `GPUTEST PASSED`. Built by the new `phone` workflow (macos-15, iPhoneOS
  SDK, ad-hoc signed; artifact `phone-bin`).
- **0098 + root v4 on the phone (2026-10-31, image 0098g): PHASE 2.2 PASS, a normal iOS process drives the GPU.**
  Root v4 (launcher) flashed; boot to SpringBoard as usual. Kernel self-test 100/100 + 50/50 (74 us), READY, then
  launchd ran `sweetrun`, which ran `gputest` from the payload: client opened (type 2), window
  0x200000000-0x300000000; ABI v1, 39-bit VA, 36-bit PA; chip 0x06030001, GMEM 1 MiB at 0x100000; **100/100 single
  submits correct, 123 us round trip from userland; one 16-IB submit 16/16 (fence 251); BoImport of its own page
  written by the GPU and read back by the CPU (EL0 dc civac works); timestamp advanced, faults 0; GPUTEST PASSED**,
  exit 0. Every ABI method used once (1, 15, 7, 2, 13, 3, 5, 6, 16, 4, 8). Note: the USB log has binary bytes,
  so search it with `grep -a`. Next: 2.3, Turnip for iOS on GitHub Actions + vktest; settle UBWC first.
- **2.3 written (2026-10-31).** Kernel **0099**: UBWC programmed as mainline's a6xx_set_ubwc_config with SDM845's
  qcom ubwc_config entry (UBWC 2.0, hbb 15, no flags): RB/TPL1/SP_NC_MODE_CNTL 0x4, UCHE_MODE_CNTL 0x400000,
  RBBM_NC_MODE_CNTL 0; GetDeviceInfo ubwc_version 2, swizzle 0x6, hbb 15, macrotile 0. Workflow **turnip**:
  Mesa 85fda3b3 + UniXNU's 9 patches/mesa (pinned UniXNU 67df14de, fetched, applied in the order of its
  adreno-compiler-inputs.json), meson cross build for arm64-apple-ios16.0 with ios-flame's options;
  **phone/turnip/vktest** (from ios-flame, MIT) carries Turnip raw-deflated inside the executable (blob.S),
  unpacks it to /private/var/tmp/sweetrun and dlopens it (the payload carries one program), with
  TU_DARWIN_SERVICE=SweetGPU (Turnip derives gpu_id 630 from chip 0x06030001). Last line "VKTEST ALL PASS".
- **0099v on the phone (2026-10-31): launchd killed `sweetrun`** (`CRASH sweetrun sig 9`, reason namespace 1
  OS_REASON_JETSAM, code 7 JETSAM_REASON_MEMORY_PERPROCESSLIMIT) while reading the 3.3 MB vktest out of the
  registry: launchd's default daemon limit (no system-wide max_task_pmem on this boot). gputest (69 KB) fit.
  Kernel **0100**: a process named `sweetrun` gets no per-process memory limit, wherever launchd sets one
  (memorystatus_update at spawn, memorystatus_set_memlimit_properties_internal later); its children start
  without one anyway (kern_fork.c). No root change needed.
- **0100 + vktest on the phone (2026-10-31, image 0100v): PHASE 2 MILESTONE, Vulkan on the A630.** sweetrun no
  longer killed; vktest unpacked Turnip (3.2 -> 18 MB in 343 ms), dlopen 1251 ms, opened SweetGPU (type 0x4147,
  tunnel), **`PASS device: Turnip Adreno (TM) 630, Vulkan 1.3.354`**, device created in 213 ms; **empty submit round
  trip mean 0.268 ms (best 0.194); fill+copy 1 MB 0 wrong words; compute shader compiled on the phone in 71 ms,
  0 wrong of 65536; 256x256 triangle (pipeline compiled on the phone in 30 ms) correct, centre red, corner blue;
  `VKTEST ALL PASS: 0 failed`**. UBWC 2.0 config in use. Unrelated background crashes seen on every recent boot
  (driverkitd jetsam kill; occasional sig10 of a random daemon, here lsd). Next: Phase 3, metal2vulkan +
  the Metal plug-in bundle (sweetmtl), then presentation (2.4) for CoreAnimation.
- **3.1 written (2026-10-31).** phone/metal2: ios-flame's copy of UniXNU's bridge (MIT) + its 8 metal2vulkan
  patches, and m2vtest (from ios-flame, adapted like vktest: Turnip embedded, TU_DARWIN_SERVICE=SweetGPU, cache in
  /private/var/tmp/sweetrun; translates every QuartzCore kernel/vertex/fragment pair from the root's
  default.metallib on the phone). Workflow **metal2**: Rust 1.98 aarch64-apple-ios (deployment 16.0), metal2vulkan
  720ef1ce + patches, llvm-as fixtures, the bridge static lib, Turnip from the latest turnip run. Last line
  "M2VTEST PASS".
- **0100m (2026-10-31): the 44.7 MB m2vtest image never left ABL**, twice (stays on the fastboot splash, still
  enumerates as fastboot; "Booting OKAY" notwithstanding): ABL places the ramdisk in a slot of about 20 MB (a
  19.1 MiB payload booted, 41 MiB did not). Fix in the shim: **compressed payloads**. `tools/mkpayload.py
  --compress` writes "SWEETZ01" + u64 size + raw DEFLATE; `shim/inflate.c` (own RFC 1951 decoder, verified
  byte-identical on the real payloads and on stored/fixed/dynamic blocks) unpacks it in `loader.c` to 0xc8000000
  (free low RAM above the GPU arena) with the I-cache on, then loads as before (uncompressed payloads unchanged).
  vktest image 21.3 -> 10.2 MB, m2vtest image 44.7 -> 18.5 MB. The screen shows `UNZIP ... ms=` timing.
- **0100vz (2026-10-31): compressed payloads work.** ABL put the ramdisk at 0x83700000 (header says 0x81000000:
  ABL picks its own addresses); `UNZIP in=0x88A39F out=0x131DE50` (8.9 -> 20.0 MB), slow with the MMU off (a few
  minutes, later item) but correct: XNU booted from it and vktest gave the same VKTEST ALL PASS as uncompressed.
- **Faster unpacking (2026-10-31, user moved it up):** the shim's loader now runs the decoder with the MMU on
  (mmu.c: shim image write-back, the unpack area 0xc8000000+ as write-back 32 MB blocks of free RAM, everything
  else Device as before), then `mmu_disable_clean` cleans image + unpack area and turns MMU and caches off in one
  leaf asm sequence (no stack writes in between), so the loader and XNU's entry run as before. Falls back to the
  uncached path (screen line "UNZIP NC") if mmu_enable refuses.
- **0100mz (2026-10-31): fast unpack works on the phone** (MMU path, quick; XNU booted), but `sweetrun: nothing to
  run`: IOKit serializes a registry property's length in 24 bits (`kOSSerializeDataMask` 0x00FFFFFF,
  OSSerializeBinary.h), so the 26.7 MB /chosen property never reached userland (3.3 MB vktest did). Fix without
  root or kernel change: **phone/bigrun** stub; `mkpayload.py --run PROG --bigrun STUB` stores programs over
  15 MB as /chosen `bigrun-0..N` (8 MB parts) + `bigrun-name` and makes the stub the program sweetrun runs; the
  stub joins the parts, runs the program and returns its status.
- **3.1 PASS (2026-10-31, image 0100mz with bigrun):** fast unpack, then sweetrun -> bigrun (26.7 MB m2vtest from
  4 parts) -> m2vtest: Turnip device 5.8 s (unpack + dlopen + device), fixture library, **compute 0 bad of 1000,
  render 0 bad of 64, import 0 bad of 4096**; QuartzCore default.metallib (141 functions) translated on the phone:
  kernels 3 ok / 4 refused, vertex/fragment pairs 13 ok / 8 refused, 1.1 s (refusals are the functions that need
  CoreAnimation's function constants or tile functions; the bridge counts them as expected); **M2VTEST PASS**.
  metal2vulkan works on the A630. Next: 3.2 (kernel 0101: IOAcceleratorES for Metal, surface coherence) and
  3.3 (SweetMetal2.bundle + installer).
- **Kernel patch 0101 (written 2026-10-31): 3.2.** With `sweetmtl=2` (and `sweetgpu=3`, fajita) the class
  `SweetMetal : IOAcceleratorES` is published on IOResources with MetalPluginName/IOGLBundleName SweetMetal2,
  MetalPluginClassName SweetMTL2Device, model "Adreno 630", user client SweetGPUUserClient (also what Turnip opens
  by default: service IOAcceleratorES, type 0x4147). SweetFramebuffer, with sweetmtl=2, cleans+invalidates the
  source rows of a surface (flush_dcache64 by VA) before its CPU copy to the panel.
- **3.3 written (2026-10-31).** phone/metal2: ios-flame's SweetMetal2.mm, NativeBridgeMetalFrontend.mm/.h (UniXNU's
  frontend, MIT) and Info.plist; phone/include/IOKit/IOKitLib.h stands in for the SDK's missing IOKit header (only
  IORegistryEntryGetRegistryEntryID is used). The metal2 workflow builds SweetMetal2.bundle (frontend + bridge, +
  Turnip; Mach-O files signed individually) and **phone/mtl2install**, which carries the bundle as one raw-DEFLATE
  archive, writes it to /System/Library/Extensions/SweetMetal2.bundle, creates /var/local (mtl2-clients =
  SpringBoard; backboardd always) and SIGKILLs backboardd and SpringBoard so launchd restarts them with Metal.
  Last line "MTL2INSTALL DONE".
- **PHASE 3 MILESTONE (2026-10-31, image 0101i): the home screen is composited by the A630 through Metal.**
  mtl2install wrote SweetMetal2.bundle (SweetMetal2 21.6 MB, Turnip 17.3 MB, Info.plist, CodeResources) and
  restarted backboardd and SpringBoard; both matched IOAcceleratorES -> SweetMetal, opened SweetGPU (windows
  0x2_0000_0000 and 0x3_0000_0000) and got `device Turnip Adreno (TM) 630` (backboardd 1.7 s, SpringBoard 4.8 s;
  pipeline caches in /var/local/m2v-cache/<process>). Photo: lock -> home screen with icons, labels, search pill,
  dock, badge. **Very slow** (user). Open: black wallpaper (PosterBoard crashes with sig 5, also on CPU-rendered
  boots), widgets "Waiting..." (chronod not in mtl2-clients), unanswered device queries (gpuCoreCount,
  maxComputeThreadgroupMemory, ...). The fastboot transfer needs the USB-A cable (USB-C to C stalls).
  Next: Phase 4, measure first (frame times, where backboardd's time goes), then the GPU present path.
- **Kernel patch 0102 (written 2026-10-31): Phase 4 measurement.** Each submission is wrapped in two
  `CP_REG_TO_MEM` of CP_ALWAYS_ON_COUNTER (64-bit, msm's get_stats_counter) into 64 slots of 128 B (pool +0x1d000,
  GPU VA 0x40021000); a slot's span counts as GPU busy once its fence has passed. A stats thread prints every 5 s
  while there is activity: `SWEET GPU: 5s: N submits, GPU busy X ms (Y%), W waits Z ms, P presents (R/s) copy C us
  each`; SweetFramebuffer counts presents and times its flush + CPU copy. Fence waits: on this kernel `delay()`
  always blocks (cpu_idle_latency is 0), so the old loop slept/woke every 10 us; now they poll for 50 us, then
  sleep 200 us at a time (`sweetgpuwait=0` restores the old loop for comparison).
- **0102 on the phone (2026-10-31, image 0102s, user's activity script): 0.2-4.8 presents/s** (mostly 1-2/s),
  CPU copy to the panel ~4.3 ms each (not the bottleneck), but **fence waits ~300 ms per submission** (e.g. 16
  submits, 224 waits, 4880 ms waited in 5 s). "GPU busy" read 0: CP_ALWAYS_ON_COUNTER (0x980) gives nothing on
  a6xx (mainline's a6xx timestamps use the GMU AO counter). Stack samples: mostly idle threads in syscalls.
- **Kernel patch 0103 (written 2026-10-31):** stamps read the GMU always-on counter as the CP sees it (0x1f888);
  **DDR (BCM MC0) and the memory NoC (SH0) voted to the top** at GPU start, as ios-flame's 0122 (`sweetddr=0`
  off): sweet_rpmh.c's lookup takes the cmd-db slave type (BCM 5, VRM 4), rh_send takes N commands, new
  `sweet_rpmh_bcm_vote_top` (allowlist MC0/SH0, fajita) sends BCM_TCS_CMD(commit, valid, 0x3fff, 0x3fff).
- **0103 on the phone (2026-10-31, image 0103s):** bus votes done (MC0 0x50000, SH0 0x5003c, TCS complete), CPU
  copy 4.3 -> 3.6 ms, but no visible change: **GPU busy 70-97 %, ~300-550 ms per submission** at 1-1.6 presents/s.
  The GPU itself is slow, by a factor that fits its core running at the power-on clock: nothing ever sent the GMU
  a performance table or a frequency vote.
- **Kernel patch 0104 (written 2026-10-31): the GPU clock.** In the HFI v1 handshake, between FW_VERSION and TEST
  as mainline's a6xx_hfi_start_v1: PERF_TABLE (8 GPU levels 0/257/342/414/520/596/675/710 MHz at RPMh 0/64..416,
  3 GMU levels 0/200/400 MHz at 0/48/128; votes built as a6xx_gmu_rpmh_arc_votes_init from this phone's cmd-db
  gfx.lvl/mx.lvl/cx.lvl, read by the new `sweet_rpmh_arc_levels`) and BW_TABLE (mainline's a630 single "off"
  entry). Once GX is up: legacy DCVS (DCVS_PERF_SETTING (3<<28)|index, BW 0xff, OOB DCVS set 23 / ack 31, read
  DCVS_RETURN) to the top index (`sweetgpufreq=N` for another). Logs gpucc 0x101c/0x1020 (downstream's gfx3d RCG,
  unverified) before/after.
- **0104 on the phone (2026-10-31, image 0104s):** cmd-db gfx.lvl 10 levels (top 416), mx.lvl 5 (top 384), cx.lvl 8;
  **PERF_TABLE err 0** (votes 0x400303 for 257 MHz .. 0x1a00409 for 710), BW_TABLE err 0, **DCVS 710 MHz acked,
  return 0**; the guessed gfx3d RCG (gpucc 0x101c) stayed 0x2/0x0. User: "sorta smoother but barely any
  improvement": GPU busy 42-81 %, ~270-450 ms per submission, 1-1.6 presents/s. Either the clock did not change
  or it is not the limit. Note ~3-5 x 109 ms: RBBM_INTERFACE_HANG_INT_CNTL 0x1fffff = 2.1M XO cycles = 109 ms.
- **Kernel patch 0105 (written 2026-10-31): diagnostics** on each 5-second line: real GPU clock (CP perf
  counter 0 = PERF_CP_ALWAYS_COUNT vs the GMU AO counter), RBBM_INT_0 bits latched since the last line
  (then cleared), CP_HW_FAULT, CB1 FSR/FAR/FSYNR0 (then cleared).
- **0105 on the phone (2026-10-31, image 0105s): the GPU core runs at ~9.6 MHz.** `gpu 9 MHz` in every window
  (CP perf counter 0 / GMU AO counter, whatever the load), no RBBM_INT_0 bits, CP_HW_FAULT 0, CB1 FSR 0x400 with
  FAR 0: a healthy GPU ~74x below 710 MHz. gpucc 0x101c/0x1020 (downstream's gfx3d RCG) stays 0x2/0x0 = XO. The
  GMU acks the DCVS vote but never moves the clock. Suspect: the GMU cannot do RPMh (RSC wake never acked, as
  edi194; mainline fails hard on that; RPMH_POWER_STATE reads 0xf "slumber"); also our PDC GFX address is
  0x30050 (cmd-db) where mainline hardcodes 0x30080 for a630.
- **Kernel patch 0106 (written 2026-10-31): RSC/PDC diagnostics, read-only:** the cmd-db ARC list, RSCC
  0x4..0x12/0x100..0x101/0x180..0x184 and PDC enable/TCS1/TCS3/seq read back before the wake, RSCC_CONTROL req/ack
  and RSC status after it; before/after DCVS: RPMH_POWER_STATE, RPMH_CTRL, G2H info, DCVS registers, gpucc PLL0
  (0x0) / PLL1 (0x100) / GX GDSC + gfx3d registers, the first 128 bytes of the GMU log.
- **0106 on the phone (2026-10-31, images 0106s/0106c): dump mode (05c6:900e) during bring-up**, before USB, so
  read from the on-screen console (0106c = no sweetfb_noconsole). The last line printed was `gpucc pll1`, so the
  read of gpucc 0x100c..0x1038 (12 words) is what trapped: some register in 0x1010..0x1038 is protected
  (0x100c, 0x101c and 0x1020 read fine on their own). Findings before that:
  - cmd-db ARC tail: mss.tmr 0x30064, ddr.lvl 0x30070, ddr.tmr 0x30074, **xo.lvl 0x30080**, xo.tmr 0x30084; gfx.lvl
    is 0x30050. Mainline's hard-coded a630 PDC address 0x30080 is xo.lvl on this phone.
  - RSCC reads back what we write (0x4 = 0x01000000, 0x8..0xa = 0x4520/0x4510/0x4514, seq a7a586a0 ...); PDC enable
    reads 0x00000001 (bit 31 not readable), TCS1/TCS3 hold our commands, pdc seq mem febea1e1 a5a4a3a2 ...
  - RSC wake: req 0x2, ack 0x0, RSC status 0x01000000, busy 0. **Mainline never sends this wake on a cold boot**
    (a6xx_rpmh_start returns early unless GMU_STATUS_PDC_SLEEP, set only by rpmh_stop), so the missing ack is
    expected and not the fault.
  - Before DCVS: RPMH_POWER_STATE 0xf, RPMH_CTRL 0xf11, G2H info 0, DCVS perf/bw/ret 0.
  - **gpucc PLL0 @0x0: 01000005 00000000 00000000 00000007 00004804 20485699 00002067 40000000: L_VAL 0, no lock
    bit: the GPU core PLL is off**; PLL1 @0x100: mode 0, L 0 (off too). gfx3d RCG 0x101c/0x1020 = 0x2/0x0 = XO.
  - Conclusion: the GMU acks DCVS but never brings up PLL0; the RSC-ack suspicion is cleared.
- **Route 2 chosen (user, 2026-10-31): we run the GPU clock ourselves.** Facts from downstream
  gpucc-sdm845.c (LineageOS android_kernel_oneplus_sdm845 lineage-22.2, ref/downstream/): gpu_cc_pll0 = Fabia
  at gpucc +0x0 (VCO 249.6 MHz..2 GHz, MX LOW up to 1066 MHz); gx_gfx3d_clk_src RCG at +0x101c/+0x1020, mnd 0
  (so 0x1024.. do not exist: the 0106 trap), parent map 2 with input 1 = crc_div = pll0_out_even / 1; the sdm845
  v2 table 180/257/342/414/520/596/675/710 MHz all at crc_div /1, so PLL0 runs at the GPU frequency.
- **Kernel patch 0107 (written 2026-10-31): `sweetgpuclk=N`** (1..7 = 257..710 MHz, absent = off, as before).
  The GMU boot vote (GMU_GX_VOTE/MX_VOTE) takes level N's perf-table indices (N=1: 3/3, the old fixed value;
  N=7: gfx 9 / mx 4). After the DCVS vote: mainline's alpha_pll_fabia_enable order (OUTCTRL off, OPMODE
  STANDBY, L + 16-bit FRAC, RESET_N, OPMODE RUN, wait LOCK_DET <= 1 ms, USER_CTL outputs + even /1,
  OUTCTRL on), then gfx3d CFG = 0x101 + CMD UPDATE. No lock = RCG stays on XO. Removes 0106's dumps.
  Log: `clk before: ...`, `clk: PLL0 locked at N MHz ...`; the 5-second `gpu N MHz` shows the result.
- **0107 on the phone (2026-10-31).** `sweetgpuclk=1 sweetgpufreq=1` (257 MHz): **the GMU started PLL0 itself**
  (mode 0xd1000005 locked, L 0x1b frac 0xcfae = 534 MHz, gfx3d RCG 0x101) and the core measured **266 MHz**:
  up to 10.8 presents/s, ~15 ms GPU per submission, no faults. So the core = PLL0 / 2 (XO gave 9.6 MHz).
  `sweetgpuclk=7 sweetgpufreq=7` (boot vote gfx 9 / mx 4): the GMU switched the RCG to PLL0 but left PLL0
  **unlocked** (mode 0x01000005, L 0x1 frac 0x890a): back to 9 MHz.
- **Kernel patch 0108 (written 2026-10-31): take over PLL0 when the GMU fails.** If PLL0 is locked and the RCG
  is on it, leave it. Otherwise: APPS ACTIVE votes `gfx.lvl` = the level's index and `mx.lvl` = its top
  (sweet_rpmh_arc_vote, fajita gfx/mx only), RCG to XO, PLL0 at **2 x f** (710 MHz: 1420 MHz, L 0x49 frac
  0xf555), RCG 0x101.
- **0107 at 257 MHz, quiet (2026-10-31): user "way faster", but "a shit ton of artifacts"** (photo: pieces of
  different moments mixed, a tear through a widget). Log: no GPU faults; swaps alternate surfaces 2/3, full
  frame (a = b = 0,0 1125x2436). Cause: the swap returned at once and the vsync timer copied the latest surface
  up to 33 ms later, while CoreAnimation was already redrawing (only the damage) into it.
- **Kernel patch 0109 (written 2026-10-31): copy at swap time.** With sweetmtl=2 (boot-arg `sweetfb_sync=0`
  restores the timer path), AppleCLCD::swap waits for every GPU fence submitted so far
  (`sweet_gpu_wait_all_ns`, 100 ms cap), then copies before returning, under one mutex.
- **0109 at 710 (2026-10-31):** APPS rail votes gfx.lvl 9 / mx.lvl 4 completed (cmd-status 0x10101), but PLL0
  did not lock at 1420 MHz (mode 0x01000004): gfx3d stayed on XO. Downstream clk-alpha-pll.c (Fabia): an L/FRAC
  write only takes after a latch (UPDATE_LOGIC_BYPASS bit 23; UPDATE bit 22, ACK_LATCH bit 29, then 100 us).
- **Kernel patch 0110 (written 2026-10-31):** `pll0_start()` with the latch; if no lock, again via a reset
  cycle (RESET_N 0 -> 1, recalibrates); lock wait 1.5 ms; then steps down 675/596/520/414/342/257 until one
  locks. Each try logs `clk: PLL0 N MHz latched|after reset: latch acked, LOCKED|not locked (...)`.
- **0110 on the phone (2026-10-31): 710 MHz.** PLL0 latch acked, LOCKED at 1420 MHz on the first try (10 us,
  mode 0xd1800005, L 0x49 frac 0xf555), gfx3d on it; stats `gpu 709-710 MHz`, up to 19.6 presents/s, GPU busy
  1-17 %, no faults or hangs. User: "way faster". With 0109's swap-time copy: no more artifacts (user, at 257).
- **Kernel patch 0111 (2026-10-31): 710 MHz is the default** (sweetgpuclk default 7, sweetgpufreq default 7
  already; `sweetgpuclk=0` = the old behaviour). Next: GPU present blit (with Phase 5 scaling), GPU interrupts.
- **Present blit, started (user, 2026-10-31).** Mesa fd6_blitter.cc: the A6xx 2D engine scales (separate
  GRAS_A2D_SRC_XMIN..YMAX / GRAS_A2D_DEST_TL/BR rectangles, TPL1_A2D_SRC_TEXTURE_INFO filter bit; render mode
  RM6_BLIT2DSCALE, CCU bypass, CP_BLIT(BLIT_OP_SCALE)); A630 offsets from a6xx.xml (RB_A2D_BLT_CNTL 0x8c00,
  GRAS_A2D_BLT_CNTL 0x8400, GRAS_A2D_SRC_* 0x8401-0x8404 (24.8 at bits 8..24), DEST_TL/BR 0x8405/0x8406,
  SP_A2D_OUTPUT_INFO 0xacc0, RB_A2D_PIXEL_CNTL 0x8c01, TPL1_A2D_SRC_TEXTURE_INFO/SIZE/BASE/PITCH 0xb4c0-0xb4c4,
  RB_A2D_DEST_BUFFER_INFO/BASE/PITCH 0x8c17-0x8c1a). **Blocker: RB_A2D_DEST_BUFFER_PITCH is in 64-byte units
  and the panel's pitch is 4320** (ABL's MDP pipe, never reprogrammed by us). Options: (A) MDP scan-out pitch
  4352, then blit + scale into the panel; (B) the MDP scans the iOS surface directly (VIG pipe scaler): zero copy.
- **Kernel patch 0112 (2026-10-31): read-only MDP inventory** at AppleCLCD start (fajita): all 8 SSPPs
  (size/xy/addr/ystride/format/unpack), CTL0-4 (layers/top/flush), PINGPONG0-3 (tear check, auto-refresh).
- **0112 on the phone (2026-10-31):** booted fine (710 MHz), but no `SWEET MDP` (nor the AppleCLCD start) line
  reached the USB log: the early-boot burst drops lines. **0113:** the inventory runs once ~20 s after start
  from the vsync timer, via printf.
- **More apps (user, 2026-10-31): root image v5.** The 32 removable Apple apps of 20H392 sit in
  /private/var/staged_system_apps (first-boot data migration installs them on a real device; sweet-root-tweaks
  skips that migration). `tools/sweet-root-apps.py` copies them to /Applications (root:admin like the other
  system apps, modes kept; Apple-signed, in the IPSW static trust cache), removes LaunchServices'
  com.apple.LaunchServices*.csstore so lsd seeds again, and with --metal2 writes SweetMetal2.bundle from
  mtl2install's SWAR blob (+ /var/local/mtl2-clients, mtl2.log) since a full userdata flash drops the copy
  mtl2install put on the phone. `work/update-root-v5.sh` (v4 -> v5, full .simg: userdata is no longer
  identical to v4, so no sparsediff). Black wallpaper is a separate bug (PosterBoard SIGTRAP).
- **0113 on the phone (2026-10-31, first boot of root v5):** only CTL1-4 (all layers 0, top 0x1f00) and
  PINGPONG0-3 arrived; **PINGPONG0 AUTOREFRESH_CONFIG 0x80000001** (enabled, every frame): ABL left the
  command-mode panel on auto-refresh from the splash buffer. The 8 SSPP lines and CTL0 were dropped (burst).
  **0114:** 50 ms between readout lines, printed at ~20 s and ~60 s.
- **Root v5 on the phone (2026-10-31): all apps are there** (user: "everything is there"; logo ~3 s, no long
  reseed visible). SweetMetal2 from the offline bundle works (GPU-composited as before).
- **0114 on the phone (2026-10-31): the MDP as ABL left it.** VIG0 (QSEED3 scaler pipe) scans 0x9d400000,
  src/out 1080x2340, YSTRIDE0 0x10e0 (4320), FORMAT 0x237ff, UNPACK 0x03020001; VIG1-3 and DMA0-3 unused; CTL0
  LAYER(LM0) 0x01000005, TOP 0x20020 (CTL1-4 empty); PINGPONG0 AUTOREFRESH 0x80000001. Plan (user agreed):
  GPU 2D blit + scale into our own aligned scan-out buffers, MDP flips VIG0 between them.
- **Kernel patch 0115 (2026-10-31):** read-only SMMU report for the display (all valid SMRs/S2CRs, MDSS streams
  0x880/0xc80 flagged, CBs) after the MDP readout; **opt-in flip test `sweetfb_flip=1`**: two buffers inside
  ABL's 36 MiB cont_splash (0x9d400000 + 12 / 24 MiB, stride 4352, WC), the CPU copy goes there, then VIG0
  SRC0_ADDR/YSTRIDE0 + CTL0 FLUSH bit 0, wait for the bit to clear (50 ms, then CTL_START, 50 ms); not latched
  = back to the splash buffer, test off. Logs `SWEET FLIP: ...` for the first 6 flips.
- **0115 flip test on the phone (2026-10-31): works.** Display SMMU: MDSS streams 0x880/0xc80 (SMR0/1) ->
  CB0, CBAR 0x10003, **SCTLR 0xe0 (M = 0: no translation)**, so the MDP scans physical addresses. Flips to
  0x9e000000 / 0x9ec00000 (stride 4352) **latched on the CTL0 flush in 1-13 ms**; screen normal (user); 15
  presents/s at 709 MHz. Next: 0116, the A6xx 2D blit (scaled) into the back buffer instead of the CPU copy.
- **Kernel patch 0116 (written 2026-10-31): the GPU present blit (opt-in `sweetfb_gpublit=1`, needs
  `sweetfb_flip=1`).** `sweet_gpu_present_blit()` (sweet_a630.c): one IB in an arena chunk at GPU VA 0x1a0000000
  - CCU colour/depth flush-TS + invalidate, WFI, RB_CCU_CNTL 0x10000000 (a630 sysmem), GRAS_A2D_SRC_* (24.8)
  + DEST_TL/BR, RB/GRAS_A2D_BLT_CNTL (MASK 0xf, FMT6_8_8_8_8_UNORM 0x30, R2D_UNORM8), RB_A2D_PIXEL_CNTL 0,
  SP_A2D_OUTPUT_INFO, TPL1_A2D_SRC_TEXTURE_INFO (linear, WZYX, FILTER, unk20/22)/SIZE/BASE/PITCH,
  RB_A2D_DEST_BUFFER_INFO/BASE/PITCH, WFI + RB_DBG_ECO_CNTL 0x05100000, CP_BLIT(SCALE), WFI + 0x04100000, CCU
  colour flush-TS, WFI; submitted + fenced (100 ms). SweetFramebuffer: scan-out buffers mapped once at GPU VA
  0x180000000 (24 MiB), source IOSurfaces in 4 cached 16 MiB slots from 0x190000000 (page list via
  getPhysicalSegment), single full-screen layer only; 1125x2436 scaled onto 1080x2340 (Phase 5 fit). Touch
  follows (`sweet_fb_scaled`: x/1080, y/2340 instead of the centred crop). Any failure: path off for the boot
  (CPU copy, centred touch). The stats' "copy N us" becomes the blit's submit-to-fence time.
- **0116 on the phone (2026-10-31): THE GPU PRESENT PATH WORKS.** `fastboot boot` started failing with "Load
  Error" (even known-good images, even after reboot bootloader), so the user had it flashed to boot_a (Sailfish
  boot kept at dtb/fajita/boot_a.img). No USB log on a normal power-on, but the screen shows it: edges visible
  (scaled 1125x2436 -> 1080x2340, the Phase 5 fit), taps right at the edges, "looks great" (user). So the 2D
  blit and the flips run and the CPU copy is gone.
- **Kernel patch 0117 (2026-10-31): flip + GPU blit are the default** with GPU surfaces (sweetmtl=2);
  `sweetfb_flip=0` / `sweetfb_gpublit=0` turn them off.
- **Kernel patch 0118 (2026-10-31): GPU interrupts.** sleh_irq (SWEET) gains `sweet_spi_hook` for INTID >= 32.
  sweet_a630.c: SPI 300 (INTID 332, level, sdm845.dtsi) in the GICD: ICFGR level, priority 0xa0, IROUTER CPU0,
  ISENABLER, read back (secure SPIs read disabled: then polling as before); RBBM_INT_0_MASK = CP_CACHE_FLUSH_TS;
  every fence CACHE_FLUSH_TS carries the IRQ bit; the handler clears RBBM_INT_0 before the EOI and wakes
  waiters; waits sleep on the event with a 200 us timeout (so no IRQ = the old polling). Stats show `irqs N`
  per 5 s and no longer clear RBBM_INT_0 when the IRQ is on. Boot-arg `sweetgpuirq=0`.
- **0118 on the phone (2026-10-31, fastboot boot worked again after a cold start): interrupts work.**
  `interrupt INTID 332: group bit 0, enabled`; `irqs` = submits in every 5 s line; GPU blit 1-3 ms (up to 13 ms
  when queued behind client work), flips latch. User: "sorta smoother but still not like smooth at all": 10-13.6
  presents/s with the GPU busy 1-19 %, so the limit is CPU-side.
- **Kernel patch 0119 (2026-10-31): CPU clock report, read-only** (new bsd/dev/sweet_cpufreq.c, started after
  sweet_a630_init): at ~25 s and ~60 s, each OSM frequency domain's table and current level (mainline
  qcom-cpufreq-hw.c qcom_soc_data: 0x17d43000 / 0x17d45800, LUT 0x110 + 32 i, PERF_STATE 0x920) and each CPU's
  measured clock (bound to it, a dependent add chain timed against mach_absolute_time; no PMU, the hypervisor may
  trap it). Boot-arg sweetcpufreq=0.
- **0119 on the phone (2026-11-01): the big cores ran at their minimum.** Domain 0 (cpus 0-3) level 15 = 1612 MHz
  (table 300..1766), domain 1 (cpus 4-7) **level 0 = 825 MHz** (table 825..2649, 2707 turbo); measured clocks
  1612 / 825 MHz exactly.
- **Kernel patch 0120 (2026-11-01): CPUs to their top non-turbo level at start** (PERF_STATE 0x920 = the
  index, as mainline's fast switch; OSM does the voltage, LMh throttles): domain 0 -> 1766 MHz, domain 1 -> 2649
  MHz. The 25/60 s report shows the measured result. `sweetcpufreq=1`: report only; `=0`: nothing.
- **0120 on the phone (2026-11-01): CPUs at full clock** (domain 0 1766 MHz, domain 1 2649 MHz measured), but user
  "a little better but not much": still 10-15.6 presents/s, idle animation exactly 10.0/s, GPU busy 1-23 %.
- **Why (IOMobileFramebuffer reverse-engineered from the 20H392 shared cache, work/tmp/scripts):** backboardd
  never calls EnableNotifications (72, our vsync); it calls SupportedFrameInfo (74) and got item count 0. iOS 16
  CoreAnimation's frame clock is IOMFB **frame info**: selector 74 [index] -> [count] + 64-byte item name (the
  library builds a name table, bit i = item i); 73 ChangeFrameInfo [3, mask]; notification type 3
  (FrameInfoNotifyFunc) = [mask, value for the client, one 64-bit value per bit] -> {name: value}; QuartzCore reads
  "Presentation_time"; IOMFB knows "Presentation_time", "Issued_time", "totalVBLs". Sampler: backboardd's
  CA::WindowServer::IOMFBServer thread waits on its port for these. Selector map: SwapBegin 4, SwapEnd 5,
  SwapWait(+Timeout) 6, SetParameter 68, SetBlock 78, GetBlock 79, SupportedFrameInfo 74, ChangeFrameInfo 73.
- **Kernel patch 0121 (2026-11-01): frame info.** 74 answers 3 items (Presentation_time, Issued_time, totalVBLs);
  73 stores the client's mask; each 60 Hz vsync tick sends type 3 [mask, last swap id, now, last swap time,
  vblank count (per mask)].
- **0121 on the phone (2026-11-01): backboardd takes the frame clock.** It now reads 74 three times (count 3 +
  names), enables notification type 3 (72) and calls ChangeFrameInfo (73) twice. User: "sorta smoother"; still
  ~14-15 presents/s. Per frame (~70 ms): ~15 ms GPU (the blit waits for it: copy 14-17 ms), up to ~16 ms flip
  latch, ~40-45 ms CPU (CoreAnimation + metal2vulkan + Turnip), all serial in backboardd's render thread.
- **Kernel patch 0122 (2026-11-01):** sampler boot-args `sweetsample_delay` (s), `sweetsample_ms`, `sweetsample_n`
  (<= 2000) to profile the frame loop; **non-blocking present (opt-in `sweetfb_async=1`)**: the swap queues the
  blit (submit-only, 8 rotating IB slots) into the buffer not on screen and returns; a worker waits for its fence
  (GPU interrupt), flips and waits for the latch; at most one flip pending (a swap that finds one waits).
- **0122 on the phone (2026-11-01): non-blocking present works.** Blits `queued`, flips `fence ok, latched after
  7-8 ms`, no timeouts. **21-24 presents/s in use (was 14-15)**; user: "honestly decent smooth now, still not the
  best but way better". Profile (193 samples of backboardd every 250 ms): its threads wait (mach_msg, semaphores,
  GPU calls) in nearly every sample, so backboardd's CPU is no longer the limit. ios-apollo (Adreno 650 fork,
  CoreAnimation -> OpenGL ES bridge) reaches 43-54 fps; its limits were background CPU hogs found with a
  per-process "top" (vmd, coreduetd, crash loops). Ours: bluetoothd and fairplayd.H2 restart every ~10 s.
- **Kernel patch 0123 (2026-11-01):** non-blocking present is the default (`sweetfb_async=0` off); **"TOP 10s"**
  line every `sweettop` s (default 10): CPU per process over the window, % of one core (fill_taskprocinfo).
- **0123 on the phone (2026-11-01, flashed to boot_a + continue; `fastboot boot` gave Load Error again).**
  TOP per 10 s: **vmd ~95-100 % always** (= com.apple.voicemail.vmd, no modem; ios-apollo found the same),
  **backboardd 86-108 % during use** (17-37 % idle), SpringBoard 30-64 % in use, **launchd ~26-30 % always**
  (bluetoothd + fairplayd.H2 crash loops every ~10 s), ReportCrash ~18 % early.
- **phone/quiet (2026-11-01, user chose the on-device fix):** run once through sweetrun, removes
  com.apple.voicemail.vmd, com.apple.fairplayd.H2, com.apple.bluetoothd, com.apple.ReportCrash (labels checked in
  20H392's launchd.plist) from /System/Library/xpc/launchd.plist, same format/owner/mode, original kept as
  launchd.plist.pre-quiet; effective after the next boot. Built by the "phone" workflow.
- **To re-enable later (user, 2026-11-01):** quiet's removals must come back with their features: com.apple.bluetoothd
  (Bluetooth), com.apple.voicemail.vmd (+ the CommCenter jobs sweet-root-tweaks removes: cellular),
  com.apple.fairplayd.H2 (FairPlay), com.apple.ReportCrash (crash reports). The original job list is on the phone
  as /System/Library/xpc/launchd.plist.pre-quiet.
- **0123 after quiet (2026-11-01):** vmd and the crash loops gone, but **launchd ~72 %** (was ~28 %) and
  **mediaserverd ~47 %** (new), ~150 % total idle (was ~160 %). Samples (launchd, mediaserverd every 0.5 s):
  mediaserverd's threads sit in system calls, among them `_xpc_pipe_mach_msg` (requests to launchd), launchd's
  main thread busy: most likely mediaserverd asking launchd for the removed Bluetooth service again and again.
  **phone/unquiet:** puts com.apple.bluetoothd back from launchd.plist.pre-quiet (vmd, fairplayd.H2, ReportCrash
  stay off, as ios-apollo).
- **PosterBoard (2026-11-01):** traps in -[PBFPosterExtensionDataStore _stateLock_setupSwitcherConfigurationForFirstTime]
  (assert `!_stateLock_switcherConfiguration`). Same as ios-apollo/Bergamota on 20H392: lsd's
  __mac_syscall("AMFI", 0x60) got ENOPOLICY (our policy is "SWEET AMFI"), so the extension catalog is
  ignored, pkd has 0 plugins, PosterBoard 0 wallpaper extensions (widgets "Waiting..." likely the same).
  **Kernel patch 0124 (written, building):** a MAC policy named "AMFI": op 0x60 -> state 0x86, others ENOPOLICY.
  **Still to do:** phone program `extfix` = write /private/var/preferences/FeatureFlags/Domain/ExtensionKit.plist
  ({enforce_xpc_cache_code_signing: {Enabled: false}}, XML, 0:0 644) + delete LaunchServices'
  com.apple.LaunchServices-*.csstore* (containers/Data/InternalDaemon/*/Library/Caches and System/*) so lsd reseeds;
  boot 1 0124 + extfix, boot 2 0124: wallpaper + widgets expected.
- **0124 + phone/extfix on the phone (2026-11-01):** `lsd asks __mac_syscall("AMFI", 0x60): boot-arg state 0x86`;
  old store (com.apple.LaunchServices-4035-v2.csstore, 4 MB, in InternalDaemon/EBAC5B3C-...) deleted, ExtensionKit
  flag written. The reseed registered the extensions: CollectionsPoster, AegirPoster, PhotosPosterProvider,
  WeatherPoster, the widget extensions all exec for the first time.
- **Wallpaper works (2026-11-01, user: everything fine).** PosterBoard 0 crashes on a second boot of plain 0124;
  CalendarWidgetExtension's ~100 % was one ~60 s burst at start-up, not a spin (cause unmeasured).
- **Siri voice extensions' crash loop:** SiriTTSSynthesizerAU, MacinTalkAUSP, KonaSynthesizer abort (uncaught
  NSException) from PlugInKit +0x18d5c: "plugin has invalid sandbox or container: error %d path %s", because our
  sandbox op 4 (container path) answers ENOENT. **Kernel patch 0125:** op 4 args { pid, pidversion, buffer, size,
  flag }; a process asking about itself with its executable inside an .appex gets /private/var/mobile; everyone
  else keeps ENOENT (`sweetsbappex=0`: nobody). Not booted yet.
- **0125 on the phone (2026-11-01): too wide.** The three voice extensions exec once and no longer crash (10 each
  before), but every .appex got the path: WeatherPoster and CollectionsPoster trap in
  +[PRSPosterPath _temporaryDirectoryURLWithContainerPath:...] ("unexpected mkdtemp result"; the path is no real
  container), AegirPoster 24 crashes (0 on 0124; link to 0125 not proven).
  **Kernel patch 0126:** only those three extensions get the path; everyone else ENOENT as on 0124.
- **0126 on the phone (2026-11-01):** wallpaper shows on every boot since 0124 (user). Results in work/tmp/usb-0126.log.
- **0126 results:** voices exec once and do not crash (a 2-3 min log), WeatherPoster/CollectionsPoster traps gone,
  **AegirPoster still 22 crashes, so 0125 was not its cause** (uncaught exception on an FBSSerialQueue block, not
  looked into further; ios-apollo saw the same and moved its appex aside). Idle total ~33 %, backboardd ~21 %.
- **Kernel patch 0127 (2026-11-01, user chose "find the hot code"):** the sampler prints `SMPIMG name address path` for images
  outside the shared cache, so backboardd's samples in SweetMetal2 (6635 symbols) can be named. Turnip's dylib is
  stripped (6 exports): only its share is visible until it is rebuilt with symbols.
- **Hot-code profile during scrolling (0127, 2026-11-01; 450 samples, 15-19 presents/s, backboardd 80-89 %):** user-mode
  running samples cover ~34 % of a core, so about half of backboardd's CPU is not user code. Of those: SweetMetal2 51 %,
  Turnip 14 %, unfair lock waits 12 %, QuartzCore 9 %. Hottest spot ~11 %: stores at SweetMetal2 +0x460e90/+0x460e98
  (unnamed routine after error::internal), 112-byte entries into a growing list. Hypothesis, unproven: per-frame
  allocation -> page faults, zero fill, TLB shootdowns. Idle: ~9.5 presents/s with nothing moving (backboardd ~21 %), cause
  unknown.
- **Kernel patch 0128:** `TOPX name[pid]: cpu kernel%, per s: faults cow syscalls switches` for the 3 busiest each window.
- **0128 results (2026-11-01):** scrolling, backboardd 86-96 %, kernel 16-38 % (mostly ~17 %), ~1000 faults/s, ~7000 syscalls/s,
  ~2100 switches/s, 13-18 presents/s: mostly user-mode work, so the fault hypothesis was mostly wrong.
- **[UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) (MIT): same stack (metal2vulkan + Turnip), with measured
  performance work we lack.** Our frontend is 1758 lines with 8 bridge patches; theirs is ~318 KB with 24 + Droid-VM's
  draw-path patches 0025-0037. Candidates: 01bfb259 hold surface imports for a second (CoreAnimation makes a texture of each
  surface per frame: allocate, map, free every frame); b59dbd0d + a579f1f5 one encoding thread fed in batches (a dispatch
  queue woke a pool thread per call; 16 % fewer frames on 60 % more CPU in GFXBench); 34abea7a draw-path patches; bridge patch
  0006 batched barriers + key-hashed maps. Their numbers are from GFXBench/other OSes (iPadOS 27), not this phone.
  mimalloc was tried there and backed out on iPadOS 27.
