# Firmware files

This directory **does not** contain firmware binaries. Firmware files
are not redistributable and must be obtained separately from your
device's stock firmware.

## Required files

### AIC8800D80 Wi-Fi / Bluetooth

**Install to**: `/lib/firmware/aic8800D80/`

**Source**: stock Android `super.fex` vendor partition, or AIC vendor SDK.

Required files (approximate — `modprobe` will tell you what's missing):

- `fmacfw.bin`
- `fw_patch_table_u03.bin`
- `fw_adid_u03.bin`
- `fw_patch_u03.bin`

**Versions**: Dec 2024 / Nov 2024 from stock Android are confirmed
working. Older firmware causes init timeouts.

### HDCP 2.2 key

**File**: `hdcp_v22.bin` (960 bytes)
**Source**: stock Android `vendor/etc/firmware/hdcp_v22.bin`
**Install to**: `/lib/firmware/hdcp_v22.bin`

Used by `hy310-hdmird` to copy into shmem at offset `0x36000` during
init. Without it, HDCP 2.2 negotiation fails (HDCP 1.4 may still work).

### HDMI-RX EDID

**File**: `hy310-edid.bin` (256 bytes)
**Source**: stock `vendor/etc/tvconfig/HDMI_EDID_14.bin`
**Install to**: `/lib/firmware/hy310-edid.bin`

Loaded by the H713 HDMI-RX driver via `delayed_work` retry loop. Without
it, the laptop won't see a valid EDID and may not output HDMI.

### MIPS display firmware (`display.bin`)

**Loaded by U-Boot**, not by Linux. Lives on FAT partition (`mmcblk0p1`)
at `/1/mips/display.bin`.

- Size: 1.25 MB (1 256 216 bytes)
- Stock MD5: `0d2191ca0dad3c17cd7db6ffa47217f5`
- Load address: ARM-phys `0x4B100000`

The `sunxi-mipsloader` kernel driver provides a `/dev/mipsloader`
interface for elog and state queries, but the firmware itself is
already loaded by U-Boot before Linux starts.

See [docs/re/display-bin.md](../docs/re/display-bin.md) for the patches
we apply for debugging.

### ARISC firmware

**Loaded by BL31 / U-Boot**. Lives in `bootloader_a.bin` TOC1 item "scp"
at offset `0xb0c00`. No file extraction needed for normal use — Linux
sees ARISC as an already-running peer over the Msgbox.

See [docs/re/arisc-firmware.md](../docs/re/arisc-firmware.md) if you
want to extract and disassemble it.

## Where to extract from

If you have a stock device backup:

```sh
# Extract super.fex from the boot package:
imgrepacker boot_a.fex             # or use stock fastboot tools

# Mount super to get vendor partition:
mkdir vendor && sudo mount -o ro,loop super_vendor.img vendor

# Wi-Fi firmware:
sudo cp -a vendor/etc/firmware/aic8800D80/* /tmp/aic8800-firmware/

# HDCP key:
sudo cp vendor/etc/firmware/hdcp_v22.bin /tmp/

# EDID:
sudo cp vendor/etc/tvconfig/HDMI_EDID_14.bin /tmp/hy310-edid.bin

# display.bin (MIPS firmware):
sudo cp vendor/firmware/display.bin /tmp/
```

For ADB-rooted stock devices, the files are accessible at the same paths
inside the running system.
