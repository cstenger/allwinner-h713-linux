# Flashing the HY310

> **Note:** All flashing is done over the network (SSH/SCP) — either via a
> USB-Ethernet adapter plugged into the single USB-A port, or via WiFi.
> The HY310 has no USB data connection to a PC and no native Ethernet.
> Power is supplied via a dedicated DC barrel jack, not USB.

## Boot Architecture

The HY310 uses a **stock Allwinner U-Boot** that cannot be easily replaced.
This U-Boot only accepts **Android Boot v3** format images (`ANDROID!` magic header).

Key constraints:
- U-Boot ignores the boot image header cmdline — the kernel cmdline is
  **hardcoded in the device tree** (`chosen/bootargs` in `dts/sun50i-h713-hy310.dts`).
  This is intentional: we do not want U-Boot or any other component overriding
  our boot parameters. **If you need to change boot arguments, edit the DTS and rebuild.**
- The boot image must be split into 4MB chunks (`mboot32.00`, `mboot32.01`)
  because U-Boot uses `fatload` which has size limitations on this platform.
- `CONFIG_ARM_APPENDED_DTB=y` is required — the DTB is appended to zImage
  inside the boot image, not passed separately.

## eMMC Partition Map

| Partition | Offset | Size | Content |
|-----------|--------|------|---------|
| p3 | 204800 | 256K | **env_a** (U-Boot environment, CRC32 signed) |
| p4 | 205312 | 256K | env_b |
| p5 | 205824 | 64M | **boot_a** — OUR MAINLINE KERNEL |
| p6 | 336896 | 64M | boot_b (stock Android kernel backup) |

See [reference/emmc_partition_map.txt](reference/emmc_partition_map.txt) for the
complete 26-partition layout.

---

## Creating Boot Image Files

After building the kernel and DTB (see [BUILDING.md](BUILDING.md)), create the
Android Boot v3 image and split it into chunks:

```bash
# This creates: mboot32.00, mboot32.01, and hy310-mainline-arm32-boot.img
python3 scripts/repack_boot.py \
    --zimage /path/to/linux-6.16.7/arch/arm/boot/zImage \
    --dtb output/sun50i-h713-hy310.dtb \
    --outdir output/
```

The script:
1. Appends the DTB to zImage (for `CONFIG_ARM_APPENDED_DTB`)
2. Wraps the result in an Android Boot v3 header (`ANDROID!` magic)
3. Splits into 4MB chunks: `mboot32.00` (first 4MB) and `mboot32.01` (remainder)

These `mboot32.xx` files are what U-Boot loads via `fatload`.

---

## Prerequisite: Patch U-Boot Environment (one-time, on a fresh device)

**Required before any UART-interrupt procedure below** (Method 1 Step 5,
Recovery, U-Boot prompt access in general). A fresh HY310 ships with **stock
U-Boot configured with `bootdelay=0`** — keypresses over UART are not even
sampled. You have **no way to drop into the U-Boot prompt** until the
environment is patched.

The patcher rewrites `env_a` (mmcblk0p3) with a CRC32-correct env that does
two things:

| Env-Var | Value | Why |
|---|---|---|
| `bootdelay` | `5` | **The 5-second UART-interrupt window** — without this, no prompt. |
| `bootcmd` | `usb start; run setargs_nand boot_normal` | Initialise USB PHY before boot, so `usb start` at the prompt actually finds devices. |

[`sunxi-env-patcher`](https://github.com/well0nez/sunxi-env-patcher) is a pure
Python tool that **runs on a host PC** (not the device). It takes either a raw
`env_a` partition dump or an `env.fex` extracted from a stock firmware image,
applies the patch with a correct CRC32 (which covers only the first 128 KiB of
the partition — most manual tools get this wrong), and outputs a patched binary
ready to flash back.

There are two ways to get the patched env back onto a fresh device. Both are
documented in [`magcubic-root`](https://github.com/well0nez/magcubic-root), the
sister toolkit which handles Allwinner `IMAGEWTY` firmware images via `awimg.py`.

**Path 1 — Firmware-image patch (recommended, safe):**

1. Extract the stock firmware image with `awimg.py` → gives you `env.fex`.
2. `python3 sunxi_env_patcher.py patch env.fex -o env_patched.fex --set bootdelay=5` (and the bootcmd edit).
3. Repack the firmware with `awimg.py` — it automatically recomputes partition
   checksums (without this, PhoenixUSBPro fails at the bad partition with
   error `0x164`).
4. Flash the repacked image via USB stick, the device's "Local UI update", or
   PhoenixUSBPro (FEL-mode recovery).

**Path 2 — Direct eMMC raw write via ADB (no root needed, faster, but bootloop risk):**

On stock firmware, `/dev/block/mmcblk0` is `rw-rw-rw-` for the unprivileged
`shell` user — a firmware permissions flaw that lets you `dd` raw blocks
without root or a bootloader unlock. Procedure (verbatim from magcubic-root):

```sh
# find the env_a partition (look up its block offsets from sysfs):
adb shell 'cat /proc/partitions | grep mmcblk0'
adb shell 'cat /sys/block/mmcblk0/mmcblk0p3/start /sys/block/mmcblk0/mmcblk0p3/size'

# dump env_a from raw eMMC (skip+count in 512 B sectors from sysfs above):
adb shell 'dd if=/dev/block/mmcblk0 of=/data/local/tmp/env_a.bin bs=512 skip=$START count=$SIZE'
adb pull /data/local/tmp/env_a.bin

# patch on host:
python3 sunxi_env_patcher.py patch env_a.bin -o env_a_patched.bin --set bootdelay=5

# write back (note: seek, conv=notrunc — DO NOT just `dd of=mmcblk0p3`):
adb push env_a_patched.bin /data/local/tmp/
adb shell 'dd if=/data/local/tmp/env_a_patched.bin of=/dev/block/mmcblk0 bs=512 seek=$START conv=notrunc'
adb shell 'sync'
```

magcubic-root itself notes Path 2 as **"Not recommended"** because of bootloop
risk if anything is off — Path 1 is the safer default. **Do this before attempting
Method 1 or any recovery.**

---

## Method 1: USB Stick Boot (First Boot / Testing)

This is the easiest way to get started. You need a USB stick (any size, 4GB+
recommended) and a way to connect it to the HY310's USB port.

### Step 1: Prepare the USB Stick

```bash
# Partition: 128MB FAT32 (boot) + rest ext4 (rootfs)
sudo fdisk /dev/sdX    # Create 2 partitions
sudo mkfs.vfat -F32 -n HY310BOOT /dev/sdX1
sudo mkfs.ext4 -L HY310ROOT /dev/sdX2
```

### Step 2: Copy Boot Image Chunks

```bash
sudo mount /dev/sdX1 /mnt
sudo cp output/mboot32.00 output/mboot32.01 /mnt/
sudo umount /mnt
```

### Step 3: Install Rootfs

See [ROOTFS.md](ROOTFS.md) for creating a full Debian rootfs, or quick version:

```bash
sudo mount /dev/sdX2 /mnt
sudo debootstrap --arch=armhf trixie /mnt http://deb.debian.org/debian
# Configure root password, SSH, etc. (see ROOTFS.md)
sudo umount /mnt
```

### Step 4: Install Kernel Modules into Rootfs

```bash
sudo mount /dev/sdX2 /mnt
sudo cp -a staging/lib/modules/6.16.7 /mnt/lib/modules/
sudo umount /mnt
```

### Step 5: Boot from USB

Plug the USB stick into the HY310, then at the U-Boot prompt (interrupt
via UART during the 5-second `bootdelay` window — this **requires the env_a
patch from the [Prerequisite](#prerequisite-patch-u-boot-environment-one-time-on-a-fresh-device)
section**, otherwise stock `bootdelay=0` gives you no window at all):

```
usb start
fatload usb 0:1 0x45000000 mboot32.00
fatload usb 0:1 0x45400000 mboot32.01
bootm 0x45000000
```

The kernel boots, finds the rootfs on partition 2 of the USB stick
(`root=/dev/sda2` from DTS bootargs), and you have a running system.

### Step 6: Get SSH Access

Once booted, the system tries to connect to WiFi (if configured in rootfs)
or you can connect via:
- **USB-Ethernet adapter** — Plug in an RTL8153 or similar, gets DHCP
- **Serial console** — ttyS0 at 115200 baud via UART

---

## Method 2: Permanent eMMC Boot (Recommended for Development)

Once you have a working USB stick boot, you can flash the kernel permanently
to eMMC so it auto-boots without UART intervention.

### Flash the Boot Image

> The one-time env_a patch (sets `bootdelay=5` + `bootcmd += usb start`) is
> a hard prerequisite — see [Prerequisite: Patch U-Boot Environment](#prerequisite-patch-u-boot-environment-one-time-on-a-fresh-device)
> above.

Use the manual hash-chain verified flash workflow (only boot_a, `/dev/mmcblk0p5`):

```bash
# 1. Hash on build machine
sha256sum output/hy310-mainline-arm32-boot.img

# 2. Copy to HY310
scp output/hy310-mainline-arm32-boot.img root@192.168.8.141:/root/

# 3. Verify hash on device
ssh root@192.168.8.141 'sha256sum /root/hy310-mainline-arm32-boot.img'
# MUST match build machine hash!

# 4. Write to boot_a
ssh root@192.168.8.141 'dd if=/root/hy310-mainline-arm32-boot.img of=/dev/mmcblk0p5 bs=512 conv=notrunc && sync'

# 5. Verify partition hash
ssh root@192.168.8.141 'dd if=/dev/mmcblk0p5 bs=512 count=<BLOCKS> 2>/dev/null | sha256sum'
# MUST match! If not: DO NOT REBOOT.

# 6. Reboot
ssh root@192.168.8.141 'reboot'
```

**NEVER flash if hashes don't match. NEVER touch boot_b (p6) — that's the
stock Android fallback.**

### Auto-Boot Flow After Flashing

```
Power on -> U-Boot (5s delay) -> usb start -> load kernel from boot_a (p5)
-> kernel boots with DTS cmdline -> rootwait finds USB stick -> Debian boots
-> WiFi connects -> SSH available (~15-20s total)
```

---

## Recovery

### If eMMC boot_a is broken (boot loop)

The USB stick is your failsafe. Use UART to interrupt U-Boot and boot from USB:

```bash
# Automated (from Windows, via OrangePi UART-TCP bridge):
python3 tools/uboot_interrupt.py --boot-usb

# Manual (at U-Boot serial prompt):
usb start
fatload usb 0:1 0x45000000 mboot32.00
fatload usb 0:1 0x45400000 mboot32.01
bootm 0x45000000
```

Once booted from USB, fix boot_a by flashing a known-good image:
```bash
dd if=/root/known-good-boot.img of=/dev/mmcblk0p5 bs=512 conv=notrunc && sync
```

### If you need to restore stock Android

boot_b (p6) still contains the stock Android kernel. From U-Boot:
```
sunxi_flash read 45000000 boot_b
bootm 45000000
```

### UART Access

The HY310 serial console is at 115200 8N1. See [docs/subsystems/uart.md](docs/subsystems/uart.md) for
hardware connection details, U-Boot interrupt instructions, and recovery commands.

> The UART interrupt window only exists *after* the env_a patch — see the
> [Prerequisite](#prerequisite-patch-u-boot-environment-one-time-on-a-fresh-device)
> section at the top.
