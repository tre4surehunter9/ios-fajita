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
