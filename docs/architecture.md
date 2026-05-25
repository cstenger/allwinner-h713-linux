# Architecture

How the three CPU cores on the H713 cooperate, and where Linux fits in.

## The three brains

```
                  +-------------------+
                  |  ARM Cortex-A53   |  <-- Linux runs here
                  |  (×4, 1.5 GHz)    |
                  +---------+---------+
                            |
              +-------------+-------------+
              |                           |
      msgbox / cpu_comm             msgbox / arisc_rpm
              |                           |
              v                           v
      +-------+--------+         +--------+--------+
      |  MIPS32 LE     |         |  OR1K BE ARISC  |
      |  display.bin   |         |  scp (172 KB)   |
      |  1.25 MB       |         |                 |
      +----------------+         +-----------------+
       (display engine,           (PMU, HPD pin,
        HDMI-RX state,             EDID storage)
        picture quality)
```

When Linux boots, the other two CPUs are **already running**. U-Boot loads
`display.bin` into MIPS RAM before the kernel starts; ARISC is initialized
even earlier (by BL31).

Cold-resetting MIPS or ARISC from Linux is fragile. The Linux drivers
assume MIPS is alive and just establish IPC with it.

## ARM↔MIPS — `cpu_comm`

The primary IPC bus. ~80 routines registered by the MIPS firmware on boot.
ARM calls them like RPC functions.

**Transport**:
- 5 MB shared memory at ARM-phys `0x4E300000` (= MIPS-VA `0xAE300000`).
- Hardware Msgbox at `0x03003000` (user region 2 for the MIPS peer).
- HW spinlocks at `0x03004000` for the shared-mem ringbuffer.

**Protocol** (one 168-byte CALL packet):
- `msg[2..3]` u16 `dst_cpu` (1 = MIPS)
- `msg[40..43]` u32 `comp_id` — Allwinner-CRC32 hash of the routine name
- `msg[64]` u32 `param_count` — **caller MUST set this** (silent no-op if missing)
- `msg[68..]` u32 param values
- `msg[120..159]` result / return values

**Routine name → comp_id**: Allwinner CRC32 with seed `0x123456`, name
formatted as `<base>_<cpu_id>_<pid_low12>`. Example:
`THal_Vp_SetBrightness_1_000` → `0x7221d017`.

**State**: full bidirectional. ARM sends a CALL → MIPS sends a RETURN.
Userspace receives MIPS-initiated callbacks via the kernel's `.read`/`.poll`
patch + per-fd ringbuffer (see [docs/subsystems/cpu-comm.md](subsystems/cpu-comm.md)).

## ARM↔ARISC — `msgbox_amp` + `arisc_rpm`

Lower-volume IPC. Used for HPD and EDID negotiation.

**Transport**: same Msgbox HW (`0x03003000`), user region 1 for the ARISC
peer. Polls `sub0 port 3`, not port 1 (initial guess was wrong, corrected
in session O).

**TX pattern**: H713 is edge-triggered. Stock's pure-IRQ pattern has 0%
success. Workaround uses `MSG_DATA` prefill + `TX_IRQ_EN` doorbell pulse
+ `udelay(10)`. Reliability ~40–60% (RX-cb-fire), which is enough for
HPD/EDID flow but not great. Tagged as `WORKAROUND` in
`sunxi_msgbox_amp.c`.

## Display pipeline (LVDS path — currently inactive)

```
HDMI input
   |
   v
DW-HDMI-RX @ 0x050C0000   <-- driven by MIPS state machine
   |
   v
INCAP @ 0x06940000        <-- "input capture", MIPS-side
   |
   v
WCE pipeline (MIPS)       <-- 5 stages, picture quality + scaling
   |
   v
DRAM buffer @ 0x4c3ef000  <-- NV12 frames, deterministic SMM allocation
   |
   v
AFBD decompressor @ 0x05600000   <-- needs pool-1 fill + config
   |                                  CURRENTLY NOT WORKING
   v
VBlender @ 0x05200000     <-- timing, currently set up by MIPS
   |
   v
LVDS PHY @ 0x051C0000     <-- MIPS-initialized
   |
   v
DLPC3435 → DLP imager     <-- currently the picture path is broken
                              before this point reaches a clean image
```

**ARM's role**: clock setup (`tvtop`), MMIO programming for AFBD and
VBlender, scanout pointer management. Two drivers can do this:

- `h713_drm` — thin shim, currently active. Hacks the AFBD scanout
  pointer at `+0x05600178` to `mips_scanout_addr=0x4c3ef000`. Picture
  arrives but is 4×1 grayscale because channel-1 is XRGB-mode while
  MIPS writes NV12.

- `sunxi_ge2d` — full port of stock `ge2d_dev.ko`, 3000+ lines, has
  vsync IRQ, OSD plane init via `LogoRegData.bin`, `/dev/fb0`. Currently
  blacklisted because of DT-binding conflict with `h713_drm` (both use
  `compatible = "trix,ge2d"`). **Next planned step**: swap the blacklist.

## What about HDMI-TX?

The H713 SoC also has a DW-HDMI TX block at `0x05010000`. **But the HY310
board does not wire it to a connector** — there is no HDMI output port on
the device. The DTS keeps the node `status="disabled"` for that reason.

The only display output is the DLP projector via LVDS (above).

## HDMI-RX state machine (MIPS-side)

| State | Name | What happens |
|---|---|---|
| 1 | Initial | set by Port_Init |
| 2 | Sleep | asserts HDCP + DDC reset |
| 3 | Idle | releases HDCP+DDC, asserts TMDS reset, AEC enable |
| 4 | TransitionUp | 8 IRQ-mask writes for PHY_INT_FREQ_DET enable |
| 5 | Running | releases TMDS reset, FreqDetect runs |

State 3 → 4 needs `Vp_Init` to be called with `Para[2] = staging-phys-addr`
(non-zero — `Para[2] = 0` causes MIPS NULL memcpy → fault → kernel-MM
cascade). Once that's right, MIPS reaches state 5 cleanly.

**Important**: the "obvious lock bits" (`CMU_STATUS`, `PHY_STATUS`,
`byte@0x06840001 bit 5`) are **zero in stock too**. They are NOT progress
indicators. Don't waste time chasing them. See
[SESSIONS.md](../SESSIONS.md) sessions Q–W for the cost of misinterpreting
these.

## Kernel modules at a glance

| Module | Type | Status | Function |
|---|---|---|---|
| `sunxi-mipsloader` | built-in | active | Loads `display.bin`, manages elog |
| `sunxi-tvtop` | OOT module | active | TVTOP bus fabric + clock manager |
| `cpu_comm` | OOT module | active | ARM↔MIPS IPC |
| `sunxi-msgbox-amp` | OOT module | active | ARM↔ARISC mailbox |
| `sunxi-arisc-rpm` | OOT module | active | RPMSG-style ARISC channel |
| `h713-hdmi-rx` | OOT module | partial | Synopsys HDMI-RX bringup, EDID |
| `h713_drm` | OOT module | active (limited) | Minimal DRM/KMS, scanout-pointer hack |
| `sunxi-ge2d` | OOT module | **blacklisted** | Real stock display driver port |
| `sunxi-decd` | OOT module | inactive | Stock decd port, not used for HDMI-RX |
| `hy310-board-mgr` | OOT module | active | Fan, NTC, GPIO PB5 management |
| `aic8800_*` | OOT modules | active | Wi-Fi + Bluetooth |
| `audio (codec/cpudai/machine)` | OOT modules | active | Internal speaker audio |
| `panfrost` | built-in | **disabled** | Mali-G31 GPU (off in current build) |
| `sunxi-cedrus` | OOT module | **disabled** | Video decode (off in current build) |

## Userspace components

| Daemon | Source | Function |
|---|---|---|
| `hy310-hdmird` | `/opt/hy310/hy310-hdmird/` | Replicates stock tvserver init + source-switch |
| `hy310-pqd` | `/opt/hy310/hy310-pqd/` | Picture-quality daemon |

Both use the kernel `cpu_comm` driver via `/dev/cpu_comm`. Both run via
systemd.

## Boot sequence (one-line view)

```
BROM → boot0 → U-Boot → fatload /boot/mboot32.{00,01} → bootm 0x45000000
                ↑                                           ↑
                |                                           |
                env_a CRC32-signed                          Linux boots,
                MIPS firmware loaded                        MIPS already alive
                to 0x4B100000
                ARISC running
```

Detail: [boot subsystem doc](subsystems/boot.md).

## Why so much userspace?

Stock Android does almost all HDMI/display work in `tvserver` userspace,
not kernel. The stock vmlinux has **zero HDMI-related symbols** — everything
goes through `cpu_comm` RPC to MIPS. Our daemon `hy310-hdmird` replicates
that architecture rather than fighting it.

This means: things like signal detection, EDID handshake, HPD timing,
source-switch all happen in `hy310-hdmird`, not in a kernel driver. The
kernel just provides the IPC transport.

## Next reads

- [Hardware overview](hardware.md) — what's actually on the board.
- [Boot subsystem](subsystems/boot.md) — full boot flow.
- [cpu_comm subsystem](subsystems/cpu-comm.md) — IPC protocol detail.
- [Display subsystem](subsystems/display.md) — what's broken right now.
- [MIPS subsystem](subsystems/mips.md) — firmware loading + state machine.
