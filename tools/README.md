# Debug and analysis tools

Most of these scripts target the development workflow (host + device).
Many can be run on the HY310 directly.

## U-Boot recovery

### `uboot_interrupt.py`

Spams keypresses through the HY310 UART to interrupt U-Boot's
`bootdelay=5` window. The default config in the script connects to a
**TCP-to-serial bridge at `192.168.8.179:9999`** — that's our specific
bench setup (an Orange Pi PC running `ser2net`).

If you use a **direct USB-UART cable** instead (the simpler path), edit
the script's connection setup to read/write `/dev/ttyUSB0` (or your
adapter's device path) at 115200 8N1 instead of opening a TCP socket.

See [docs/subsystems/uart.md](../docs/subsystems/uart.md) for both
approaches.

```sh
python3 tools/uboot_interrupt.py                # drop to U-Boot prompt
python3 tools/uboot_interrupt.py --boot-usb     # USB-stick rescue boot
python3 tools/uboot_interrupt.py --restore      # restore boot_a from boot_b
python3 tools/uboot_interrupt.py --cmd "..."    # custom U-Boot command
python3 tools/uboot_interrupt.py --monitor      # watch UART output only
```

**Note**: stock U-Boot has `bootdelay=0` by default — patch the env
first with [sunxi-env-patcher](https://github.com/well0nez/sunxi-env-patcher)
to add `bootdelay=5`. Otherwise no interrupt window exists.

## Boot image validation

### `verify_bootimg.py`

Validates Android Boot v3 image: checks `ANDROID!` magic, header fields,
kernel/ramdisk offsets, page alignment.

```sh
python3 tools/verify_bootimg.py output/hy310-mainline-arm32-boot.img
```

### `compare_dtb.py`

Compares two DTB files by decompiling both and diffing the DTS. Useful
for verifying that a rebuilt DTB matches the production DTB.

```sh
python3 tools/compare_dtb.py old.dtb new.dtb
```

## U-Boot environment

### `analyze_env.py`

Dumps U-Boot `env_a` content. Parses CRC32 header, shows all environment
variables, validates checksum.

```sh
python3 tools/analyze_env.py /path/to/env_a.bin
```

### `patch_env_usb.py`

Adds `usb start` to bootcmd. Handles CRC32 correctly. See
[FLASHING.md](../FLASHING.md) for context.

```sh
python3 tools/patch_env_usb.py
```

A cleaner version with more features is at
[sunxi-env-patcher](https://github.com/well0nez/sunxi-env-patcher).

## MIPS firmware

### `dump_mips_elog.py`

Reads the MIPS coprocessor error log from shared memory. The elog is a
ring buffer at ARM-phys `0x4B272D9C` (~120 KB), Mode 1.

Run on the device:

```sh
python3 dump_mips_elog.py
```

Mode 2 (2 MB linear) tools are on the device at
`/root/mips_elog2.py` + `/root/unscramble_elog.py`.

## Hardware probing

### `probe_wdt.py`

Probes watchdog registers via `/dev/mem`. Originally used to discover
that H713 WDT is at `0x02051000` (not the H6 address `0x030090a0`).

```sh
# Run on device:
python3 probe_wdt.py
```

### `read_rpio.py`

Reads R_PIO (PL / PM bank) registers via `/dev/mem`. For debugging
GPIO pin function and pull-up on the H713 R_PIO controller (which uses
0x30 byte bank spacing, NOT 0x24 like H6).

```sh
# Run on device:
python3 read_rpio.py
```

## RE workflow (extra)

For deeper RE work see [docs/contributing-re.md](../docs/contributing-re.md)
which covers:

- IDA Pro / Ghidra / capstone-elftools setup
- `hreg` / `hdump` universal MMIO/DRAM accessors via `/dev/hidtvreg`
- `regtrace` periodic register-diff
- display.bin patching workflow
- Stock-Android live-comparison via ADB + magisk

## See also

- [BUILDING.md](../BUILDING.md) — build pipeline
- [FLASHING.md](../FLASHING.md) — flashing procedures
- [docs/contributing-re.md](../docs/contributing-re.md) — RE workflow
