# eMMC

**Status**: ✅ works at HS200 @ 200 MHz SDR.

## Hardware

- **Controller**: `sunxi-mmc`, compatible `"allwinner,sun50i-h713-mmc"`
- **Controller version**: v5.4.0 (`sunxi-mmc-v5p3x`)
- **Device**: 7.3 GB Samsung KLM8G1GETF-B041

## Modes — HS200 vs HS400

**Currently used: HS200 @ 200 MHz SDR**, ~50 MB/s sustained.

**Historical**: HS400 @ 200 MHz DDR (~103 MB/s) worked on the first test
unit. A second device showed intermittent CRC errors and rare hangs at
HS400. We downgraded the DTS to HS200 for portability across boards.

To re-enable HS400 per device (if you've confirmed yours is stable):

```dts
mmc0: mmc@4020000 {
    /* Remove these to enable HS400: */
    /* mmc-hs400-1_8v;          */
    /* mmc-hs400-enhanced-strobe; */
    /* (or add them back to enable) */
};
```

The kernel patches required for HS400 (DMA reset sequence, NTSR delays)
are still in `patches/0006-mmc-sunxi-add-h713-v5p3x-support.patch` — only
the DTS opts in.

## Partition table

26 partitions. Highlights:

| Partition | Label | Use |
|---|---|---|
| `mmcblk0p1` | (FAT) | `/1/` mount point, `mips/display.bin`, `mips/display_cfg.xml`, `mboot32.00`, `mboot32.01` |
| `mmcblk0p3` | env_a | U-Boot environment (CRC32-signed) |
| `mmcblk0p4` | env_b | U-Boot env backup |
| `mmcblk0p5` | boot_a | **Active mainline boot image** |
| `mmcblk0p6` | boot_b | Stock Android boot image (failsafe — never touch) |

Full map: [reference/emmc_partition_map.txt](../../reference/emmc_partition_map.txt).

## Patches applied

The patches live in `patches/0006-mmc-sunxi-add-h713-v5p3x-support.patch`:

| Patch | Notes |
|---|---|
| `no_wait_pre_over` flag | Required for v5p3x stability |
| IDMA chunking | Prevents transfer errors on large blocks |
| DMA / FIFO / IDMA 3-step reset | Correct reset sequence for v5p3x |
| CMD53 retry with phase rotation | Fixes intermittent CMD53 failures |
| Clock doubling | Required to reach 200 MHz |
| NTSR stock delays | Preserves stock timing margins |

## Debugging note

The historical debug `printk` in `mmc/core/core.c` is NOT part of the
upstream patch set. It was used during bringup and must not be in
production builds.

## See also

- [Boot subsystem](boot.md) — how U-Boot uses partition 5
- [FLASHING.md](../../FLASHING.md) — flashing procedures
