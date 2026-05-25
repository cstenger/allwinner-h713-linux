# HDMI-RX (HDMI input)

> The HY310 has **only HDMI input**, no HDMI output port. The DLP
> projector via LVDS is the only display output. This page is about the
> HDMI input path.

**Current state**: signal detection works end-to-end; picture quality is
broken (4×1 XRGB grayscale tiling on the projector).

## What works

- HDMI-RX TMDS lock at 1080p60 (`tmds=0x8d9f485`)
- Signal detection: `signal_id = kHalSignalID_XGA19201080`, pixel
  frequency 148.5 MHz
- `MipsHalCallback_SignalChange(state=3)` reaches userspace via cpu_comm
  callback delivery
- HPD detection (the EDID round-trip works, laptop sees the HY310 as
  `SGD SX8 1920x1080p@60`)
- EDID writes go through Synopsys `DMA_CONFIG10/11` (readback returns
  zero, but laptops read the data correctly — mechanically not
  understood)
- `hy310-hdmird` daemon orchestrates the full stock-equivalent init +
  source-switch sequence (28/28 cpu_comm calls verified)

## What's broken

The picture **reaches the LVDS pipeline** but is **tiled 4×1 grayscale**:

- AFBD channel-1 is in XRGB (32 bpp) mode
- MIPS writes NV12 (12 bpp) into the buffer at `0x4c3ef000`
- The display reads 1920 NV12-Y bytes per row as 480 XRGB pixels
- Then tiles them 4× horizontally to fill the row
- Chroma plane is ignored → grayscale

This is the active development front. See
[docs/subsystems/display.md](display.md) for the path forward.

## Hardware

| Block | Address | Notes |
|---|---|---|
| Synopsys DW-HDMI-RX | `0x050C0000–0x050CFFFF` | CMU, PHY, SCDC, DMA, EDID-RAM |
| H713 wrapper (controller) | `0x06800800–0x068008FF` | port-select, PHY-reset (MIPS-side) |
| H713 wrapper (port-state) | `0x06840000–0x068408FF` | per-port, byte-multiplexed |
| TVCAP / INCAP | `0x06800000` / `0x06940000` | frame-capture pipeline (not implemented) |
| HPD pin | `0x07091014` | 3-bit register, undocumented (port N = bit N) |

The H713 SoC also has a DW-HDMI **TX** block at `0x05010000`. **The
HY310 board does not wire it to a connector** — DTS keeps it
`status = "disabled"`.

## EDID

EDID write is via Synopsys DMA — enable `WRITE_EN` + slave 0x50,
byte-stream to `DMA_CONFIG10`. Readback always returns zero, but
laptops read the EDID correctly. Mechanically not understood.

EDID source: `/lib/firmware/hy310-edid.bin` (256 B, copied from stock
`vendor/etc/tvconfig/HDMI_EDID_14.bin`).

Loading: `delayed_work` retry loop at 2 s intervals. Uses
`firmware_request_nowarn` because `request_firmware_nowait` doesn't
auto-retry. EDID loaded around T+9s after probe (after attempt 5).

## HPD

Register `0x07091014`, 3 bits, undocumented. Port 1 = bit 1. This
register was discovered by reversing the ARISC firmware HPD handler at
`0x121e4`.

Sequence (session P v7):
1. `controller_enable` writes `0x00` at T+0.9s
2. `delayed_work` callback writes `0x07` at T+9s after EDID is loaded
3. The 8-second sustained-LOW + LOW→HIGH edge forces the laptop to
   re-read EDID

Plug/unplug auto-detection doesn't work — IRQ 266 never fires.
Polling watcher planned but not deployed.

## MIPS state machine

The HDMI-RX state machine runs on MIPS. See
[docs/subsystems/mips.md](mips.md) for state details. State 3 → 4
requires `Vp_Init Para[2] != 0` (`hy310-hdmird` does this).

## libhalhdmi sub-cmd table

`libhalhdmi.so` (stock library) sends sub-commands to ARISC via mcu_comm.
Format: `{u32 length, u16 sub_cmd, byte payload[]}`. sub_cmd encoding:
`[action_id:8][group:4][type:4]`.

| sub_cmd | Name | Notes |
|---|---|---|
| `0x0011` | `PortRemap` | byte + word |
| `0x0111` | `UpdateEDID` (4× chunks) | 64 B EDID per chunk |
| `0x0211` | `PullHotPlug` | |
| `0x0411` | `SET5VFlag` | |
| `0x0511` | `SetEDIDAudioMode` | |
| `0x2011` | `ResetEDIDModule` | |

**These all return `status=-3`** ("imt error") from ARISC. The stock
ARISC dispatcher 0x12490 has handlers for these but they're hollow by
design. ARISC dispatcher 0xbc9c only handles PMU commands. Bottom line:
the mcu_comm EDID path is no-op. The Synopsys DMA path is the working
EDID mechanism.

## hy310-hdmird daemon

C++ ARM32 cross-compiled daemon at `/opt/hy310/hy310-hdmird/`.
Source: `main.cpp`, `cpucomm.cpp`, `hdmi_ctl.cpp`, `name2id.cpp`,
`receiver.cpp`.

What it does:

1. Opens `/dev/cpu_comm`, mmaps 5 MB shmem.
2. Loads `/lib/firmware/hdcp_v22.bin` (960 B, from stock
   `vendor/etc/firmware/`) into shmem at offset `0x36000`.
3. Registers 9× `MipsHalCallback_*` routines for ARM-side delivery.
4. Runs init sequence (12 calls: Vp_Init with Para[2] fix, callback
   registrations, port-map, HDCP22, HPD timing).
5. Runs source-switch sequence (12 calls: SetSource + reapply picture
   defaults).
6. Listens on `/run/hy310-hdmird.sock` for runtime commands.

Plus the **callback-loop receiver thread** (session DD): waits on
`/dev/cpu_comm.read()`, dispatches `SignalChange(state=3)` events to
trigger the post-signal sequence directly instead of timeout fallback.

CLI:
```sh
hy310-hdmi src 3       # switch to HDMI input port 3 (= HDMI1)
hy310-hdmi status      # current state
hy310-hdmi init        # re-run init sequence
hy310-hdmi quit
```

Service:
```sh
systemctl status hy310-hdmird
journalctl -u hy310-hdmird -n 50
```

## Debugging

```sh
# Daemon state:
systemctl status hy310-hdmird

# MIPS state machine (should be 5 after hdmird init):
cat /sys/class/hy300/mips/state_machine

# MIPS log:
python3 /root/mips_elog2.py | grep -E 'TMDS|signal_id|Wce|tmds=0x'

# HPD pin (should be 0x07 when EDID loaded):
python3 -c "import mmap, os
fd = os.open('/dev/mem', os.O_RDONLY)
m = mmap.mmap(fd, 4, prot=mmap.PROT_READ, offset=0x07091000)
print(hex(int.from_bytes(m[0x14:0x18], 'little')))"
```

## Open work

- **Picture quality**: swap blacklist to `sunxi_ge2d`. See
  [display.md](display.md).
- **HPD plug/unplug**: polling watcher.
- **HDMI audio**: not started.
- **EDID readback mystery**: would be nice to understand
  `DMA_CONFIG10/11` properly. Possibly a write-only port.
- **TVCAP/INCAP frame capture**: existing `sunxi_tvcap_rx.ko` skeleton
  needs review.

## See also

- [Display subsystem](display.md) — where the picture goes after MIPS
- [MIPS subsystem](mips.md) — firmware that drives HDMI-RX
- [cpu_comm subsystem](cpu-comm.md) — IPC that hdmird uses
- [docs/re/edid-protocol.md](../re/edid-protocol.md) — full EDID
  protocol RE
