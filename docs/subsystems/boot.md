# Boot

**Status**: ✅ works.

## Chain

```
BROM → boot0 → U-Boot 2018.05 (stock Allwinner) → kernel
                ↑                                    ↑
                env_a CRC32-signed                   ARM32 mode (spsr=0x1d3)
                Loads display.bin to 0x4B100000      MIPS firmware already running
                Loads boot image from FAT
```

## Stock U-Boot constraints

We can't easily replace stock U-Boot. The constraints we work with:

- Only accepts **Android Boot v3** format images (`ANDROID!` magic).
- Crashes if DTB is passed separately — `CONFIG_ARM_APPENDED_DTB=y` is
  mandatory, DTB appended to zImage via `repack_boot.py`.
- Ignores the boot image header cmdline — the kernel cmdline is
  **hardcoded in DTS** (`chosen/bootargs`). This is intentional — we
  don't want anything overriding our boot args.
- Uses `fatload` for boot images with a size limit → image split into
  4 MB chunks (`mboot32.00`, `mboot32.01`).

## eMMC layout (boot-relevant)

| Partition | Use |
|---|---|
| `mmcblk0p1` | FAT — holds `mboot32.00`/`.01` + `mips/display.bin` + `mips/display_cfg.xml` |
| `mmcblk0p3` | `env_a` — U-Boot environment, CRC32-signed |
| `mmcblk0p4` | `env_b` — env backup |
| `mmcblk0p5` | `boot_a` — **active mainline boot image** |
| `mmcblk0p6` | `boot_b` — stock Android boot image (**failsafe, never touch**) |

## env_a

CRC32 covers `data[4:0x20000]`. Key variable:

```
bootcmd=usb start;run setargs_nand boot_normal
```

The `usb start` is required — initializes the USB PHY before kernel
handoff. Without it, USB devices don't work in kernel.

To patch env, use [sunxi-env-patcher](https://github.com/well0nez/sunxi-env-patcher).

## Kernel cmdline (in DTS)

```
console=tty0 console=ttyS0,115200 earlycon loglevel=8
root=/dev/sda2 rootwait rootfstype=ext4
net.ifnames=0
panic=5
clk_ignore_unused pd_ignore_unused
hy310_board_mgr.no_rpm_shutdown=1
cma=128M
```

Notes:
- `panic=5` — auto-reboot 5s after kernel panic
- `rootwait` — wait for USB stick rootfs to appear
- `clk_ignore_unused` — required until clock tree is fully described (see
  [docs/known-issues.md](../known-issues.md))
- `cma=128M` — CMA pool size, blocker for Weston buffer allocation

## MIPS pre-load

U-Boot loads `display.bin` (MIPS firmware, 1.25 MB) into ARM-phys
`0x4B100000` **before** the kernel starts. By the time Linux runs, MIPS
is already executing.

This matters because cold-resetting MIPS from Linux is fragile. The MIPS
loader kernel driver (`sunxi-mipsloader`) does not load firmware — it just
manages the running MIPS via the loader register block at `0x03061000+`.

See [MIPS subsystem](mips.md) for details.

## Watchdog

| Watchdog | Address | Compatible |
|---|---|---|
| Main WDT | `0x02051000` | `sun6i-a31-wdt` |
| R_WDOG | `0x07020400` | (backup) |

Main WDT is **not** at H6 address `0x030090a0`. Do not copy H6 DTS
verbatim.

## Repack — mandatory

`repack_boot.py` is what produces a valid boot image:

1. Appends the DTB to zImage (for `CONFIG_ARM_APPENDED_DTB`).
2. Wraps in Android Boot v3 header (`ANDROID!` magic).
3. Splits into 4 MB chunks: `mboot32.00` (first 4 MB) and `mboot32.01`
   (remainder).

**Don't try to inline-make a boot image** with a Python one-liner or `cat
zImage dtb`. It will brick the device because the DTB won't be appended
correctly. See [BUILDING.md](../../BUILDING.md) for the proper invocation.

## See also

- [FLASHING.md](../../FLASHING.md) — the flashing procedure
- [BUILDING.md](../../BUILDING.md) — building boot.img from sources
- [UART](uart.md) — how to interrupt U-Boot
