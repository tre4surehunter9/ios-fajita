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
