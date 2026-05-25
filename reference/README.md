# Reference material

Extracted stock-firmware analysis data. These files are not part of the
build — they're hardware references used during reverse engineering.

## Files

| File | Source | Purpose |
|---|---|---|
| `stock_dts/hy310-board.dts` | decompiled `boot.fex` via `dtc -I dtb -O dts` | primary HW reference (which compatibles, IRQs, clocks) |
| `kallsyms.txt` | extracted from `vmlinux.fex` via `/proc/kallsyms` parser | stock kernel symbol table (~130 k symbols) |
| `emmc_partition_map.txt` | live device read | full 26-partition layout |
| `stock_gpio_map.txt` | extracted via ADB + `/proc` + `/sys` | GPIO pin assignments, I²C devices, reserved memory |
| `sys_config.fex` | from stock boot package | Allwinner FEX hardware config (pin assignments, DRAM params) |
| `uboot_dtb.dts` | decompiled U-Boot DTB | U-Boot HW config + boot sequence |
| `env.fex` | from stock | original (unpatched) U-Boot environment |

## How these were obtained

- **Stock DTS**: from `boot.fex` via `dtc -I dtb -O dts`
- **kallsyms**: from `vmlinux.fex` via custom symbol-table parser
- **GPIO / partition data**: from a running stock Android via ADB +
  `/proc` + `/sys`

## Stock firmware version

The HY310 runs a modified Android TV based on **Allwinner H713 SDK
V1.3**. Stock kernel: **Linux 5.4.99 ARM32**.

## Notable RE artifacts (not stored here)

For larger artifacts that don't fit a git repo, see the dev-server paths
referenced in [docs/contributing-re.md](../docs/contributing-re.md):

- `/opt/hy310/stock-re/display.bin.i64` — IDA workspace for MIPS firmware
  with renames + comments from 50+ RE sessions
- `/opt/hy310/stock-re/modules/decd.ko.i64` — IDA workspace for stock decd
- `/opt/hy310/stock-re/modules/ge2d_dev.ko.i64` — IDA workspace for stock
  display driver
- `/opt/hy310/stock-re/libhalhdmi.so.i64` — HDMI library
- `/opt/hy310/stock-re/libvideo.so.i64` — userspace tvserver helpers

Plus larger binaries in `/opt/hy310/stock-re/super_extracted/` for direct
reference.

## See also

- [docs/contributing-re.md](../docs/contributing-re.md) — RE workflow
- [docs/re/](../docs/re/) — reverse-engineering deep dives
