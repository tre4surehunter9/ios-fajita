# phone/wifi: wland (Wi-Fi from userland)

From [NWylynko/ios-flame](https://github.com/NWylynko/ios-flame) `phone/wifi` (MIT; Pixel 4, the same WCN3990
Wi-Fi), ported to the OnePlus 6T (SDM845). Every SDM845 change is marked `SDM845:` in the source, with its
mainline source (`sdm845.dtsi`, `sdm845-oneplus-common.dtsi`):

| | ios-flame (SM8150) | here (SDM845) |
| --- | --- | --- |
| APCS IPC doorbell | `0x17c0000c` | `0x1799000c` (same bits: 14 SMP2P, 12 GLINK, 0 AOSS) |
| WLAN rails | ldoa1, ldoa7, ldoc2, ldoc11 | ldoa5 800, ldoa7 1800, ldoa17 1304, ldoa25 + ldoa23 3312 mV |
| QDSS clock over QMP | required | sent, a refusal is not fatal (not in SDM845's wifi node) |
| modem region | `0x8dc00000` + `0xb400000` | `0x8e000000` + `0x7800000` |
| MSA | `0x8bc00000` | `0x8df00000` |
| rmtfs buffer | allocated | fixed `0xf5b01000` (OnePlus: where the modem expects it) |
| WLAN SMMU stream | `0x640` | `0x0040` |
| board data | `bdwlan-flame.bin` | picked from `board-2.bin` by the reported board id (wlfw.c, `tools/ath10k-bdf.py`) |
| modem boot | TrustZone PAS | PAS tried first; mainline boots SDM845's modem through MBA (`qcom_q6v5_mss.c`) |

Needs kernel 0133 (`/dev/pmem`). Files on the phone: `/var/local/fw/modem/` = mba.mbn, modem.mbn,
wlanmdsp.mbn, modemr.jsn, modemuw.jsn, board-2.bin (public: gitlab.com/sdm845-mainline/firmware-oneplus-sdm845
@176ca71); `/var/local/fw/rmtfs/` = this phone's modemst1/modemst2/fsg/fsc (per phone, never in git).
