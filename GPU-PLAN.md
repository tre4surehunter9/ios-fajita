# GPU acceleration on the OnePlus 6T (Adreno 630)

Goal: Metal (and through it CoreAnimation, so the whole UI) on the Adreno 630, the way
[ios-flame](https://github.com/NWylynko/ios-flame) does it on the Pixel 4's Adreno 640 (same a6xx family,
same ios-sweet base, same iOS 16.7.16 20H392 userland): an in-kernel GPU core plus Mesa's Turnip
(Vulkan) on iOS, then metal2vulkan and a Metal plug-in bundle.

Decisions (user, 2026-10-29): the zap shader comes from postmarketOS (open source); all builds,
userland included, run on GitHub Actions (no Mac available).

## Sources and how each may be used

| Source | Licence | Use |
| --- | --- | --- |
| [ios-flame](https://github.com/NWylynko/ios-flame) (Pixel 4, A640) | MIT | Code: kernel GPU core (`sweet_a6xx.c`), `/dev/pmem`, accelerator service + user client (patches 0095-0124), `phone/turnip`, `phone/metal2`, build scripts. With attribution |
| [edi194/ios-fajita](https://github.com/edi194/ios-fajita) (this phone, A630) | none (all rights reserved) | Facts only: A630 register values, power sequence, what broke (patches 0099-0174, NOTES-FAJITA.md). No code |
| Mainline Linux `drivers/gpu/drm/msm/adreno/a6xx_gmu.c`, `a6xx_gpu.c`, `sdm845.dtsi` | GPL-2.0 | Facts and sequences. No code |
| [iOSPlus10](https://github.com/akshaynexus/iOSPlus10) (A730) | see repo | Design notes (`docs/gpu-*.md`, `metal-design.md`), comparison of all ports |
| [metal2vulkan](https://github.com/steelbrain/metal2vulkan) | GPL-2+ or LGPL-3+ | Built as ios-flame does |

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
