# phone/metal2: Metal on the Adreno 630 (GPU-PLAN.md Phase 3)

- `bridge/` - UniXNU's NativeMetal bridge over the metal2vulkan runtime (`NativeMetalBridge.rs`,
  `NativeMetalObjects.rs`, headers; MIT, see `bridge/LICENSE`), as carried by ios-flame, and
  `bridge/patches/`: patches to steelbrain/metal2vulkan 720ef1ce (0001-0006 UniXNU's, 0100/0101 ios-flame's).
  The patches fall under metal2vulkan's licence (GPL-2.0-or-later OR LGPL-3.0-or-later); anything that links
  the built bridge links metal2vulkan statically.
- `m2vtest.c` - the bridge's self-test (from ios-flame, MIT), run on the phone through sweetrun.

Built by the `metal2` workflow.
