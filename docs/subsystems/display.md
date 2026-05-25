# Display

> **Status**: ⚠️ **broken picture**. The DLP projector (via LVDS) is the
> only display output on the HY310 — there is no HDMI output port. Right
> now it shows 4×1 XRGB grayscale tiling of whatever comes through the
> HDMI input. We're actively debugging this.

This page covers the LVDS/DLP projector output path, which is where the
reverse-engineering work is happening. For the HDMI input side (signal
detection, EDID, HPD), see [hdmi.md](hdmi.md).

## What we want, what we have

**What we want**: HDMI input → MIPS WCE pipeline → AFBD → VBlender →
LVDS → DLPC3435 → DLP imager. Stock Android does this; the laptop
content shows on the projector cleanly.

**What we have**: same path mostly works through the MIPS side. ARM side
gets the picture into the LVDS pipeline but with channel-1 in XRGB mode
while MIPS writes NV12. Result: 4×1 horizontal tiles of grayscale on the
projector.

Right now the projector also doesn't show a Linux desktop — Wayland +
GPU + Cedrus are intentionally disabled while we debug this scanout
path. So whatever the projector shows is the broken HDMI-RX picture,
not a compositor.

## Architecture

```
HDMI-IN -> DW-HDMI-RX -> MIPS WCE pipeline -> NV12 in DRAM
                                                  |
                                                  v
                          AFBD @ 0x05600000  (decompressor + scanout)
                                                  |
                                                  v
                          VBlender @ 0x05200000  (timing)
                                                  |
                                                  v
                          LVDS PHY @ 0x051C0000  (MIPS-initialized)
                                                  |
                                                  v
                          DLPC3435 (I2C) -> DLP imager
```

## Hardware

| Block | Address | Notes |
|---|---|---|
| TVTOP (bus fabric) | `0x05700000` | MUST program first — gates all sub-blocks |
| VBlender | `0x05200000` | timing controller, MIPS-initialized |
| OSD plane | `0x05248000` | plane control + commit bit |
| GE2D core | `0x05240000` | minimal init |
| AFBD | `0x05600000` | frame buffer controller, scanout addresses |
| LVDS PHY | `0x051C0000` | MIPS-initialized |
| DLPC3435 | I2C @ 0x1B | TI DLP controller |

Scanout pointers:
- Pool-1 (decd's territory): AFBD `+0x70-0xA4` — 4 channels, Y/C/info
- Pool-2 (page-flip target): AFBD `+0x320/+0x324` — HW-autonomous cycling

GPIO warning:
- **PB5** controls both panel backlight AND fan power. **Never write
  PB5 = LOW** — kills the fan.

IRQs:
- VBlender: GIC SPI 101
- AFBD: GIC SPI 112

## The TVTOP bus fabric

This was the original blocker that took a long time to find.

**Problem**: VBlender / OSD_B / GE2D / LVDS / AFBD all read zero.

**Cause**: TVTOP at `0x05700000` is a bus fabric router. Without
programming its 7-register routing table, every display sub-block is
bus-gated. The stock `tvtop_tvdisp_enable` sequence:

```c
// Full sequence:
1. clk_prepare_enable(svp_dtl_clk)     // 200 MHz from pll-periph0-2x
2. clk_prepare_enable(deint_clk)       // 1032 MHz from pll-video2-4x
3. clk_prepare_enable(panel_clk)       // 1032 MHz from pll-video2-4x
4. clk_prepare_enable(clk_bus_disp)    // 150 MHz from ahb
5. reset_control_deassert(rst_bus_disp)
6. Write TVTOP routing:
   TVTOP+0x04 = 0x00000001
   TVTOP+0x44 = 0x11111111
   TVTOP+0x88 = 0x11111111      // was 0xFFFFFFFF, EE-fixed
   TVTOP+0x00 = 0xFFF11111      // was 0x00011111, EE-fixed
   TVTOP+0x40 = 0x00011111
   TVTOP+0x80 = 0x00001111
   TVTOP+0x84 = 0xFFF000EF      // 1080p-specific
```

Now in mainline `sunxi-tvtop.ko` (`drivers/tvtop/`). Both `sunxi_tvtop`
and `sunxi_decd` register as clients via `sunxi_tvtop_client_register()`.

For the historical analysis with all the dead-end hypotheses we ruled out,
see [docs/re/display-bringup.md](../re/display-bringup.md).

## The drivers

This is where it gets messy. We have three modules with overlapping
responsibility:

### 1. `h713_drm` — currently active

`drivers/display/drm/h713_drm.c` (out-of-tree).

- DRM/KMS shim using `drm_simple_display_pipe`
- Fixed mode: 1920×1080@60 LVDS
- Binds to `compatible = "trix,ge2d"` in DTS
- GEM DMA scanout via AFBD controller
- PRIME buffer sharing with Panfrost
- Module params:
  - `mips_scanout_addr=0x4c3ef000` — overrides AFBD scanout to MIPS output
  - `mips_scanout_c_offset=0x5fd000` — reserved (unused)
  - `ch1_mode` (0/1/2/3) — channel-1 mode-bit experiments
  - `ch0_mode` (0/1/2/3/4) — channel-0 mode-bit experiments
  - `plane_init_steps` (bitmask) — selective plane init (default 0)
  - `use_safe_scanout`, `fill_test_pattern`, `enable_dlpc3435` — debug

EE-session source fixes deployed:
- TVTOP routing fix (`+0x00 = 0xFFF11111`, `+0x88 = 0x11111111`)
- LVDS corrections (`+0x14 = 0x1A000005`, `+0x24 = 0x00350000`,
  `+0x28 = 0x08100035`)
- VBlender wrong-writes removed
- OSD_FB_ADDR write removed
- NRWinNode bit 4 added in mode=4 path

With `h713_drm` active you get the 4×1 grayscale picture on LVDS via
mode=1 (ch1 XRGB).

### 2. `sunxi_ge2d` — blacklisted, but is the real solution

`drivers/display/ge2d/` (out-of-tree, ~3000 lines, 9 source files).

This is a full port of the stock `ge2d_dev.ko` — the **real stock display
driver** for the HDMI-RX scanout path. Probe has 16 steps directly
matching `ge2d_drv_probe`:

1. Parse DTS panel properties
2. Map MMIO (4 regions: OSD, LVDS, OSD_B, AFBD)
3. Enable clocks + reset
4. `/dev/ge2d` chardev
5. OSD interrupt init (writes `+0x168 = 0xFFFFFFFF`, `+0x16C = 0x10`)
6. Vsync timestamp init
7. OSD frame init (fence context)
8. Request IRQs (`ge2d_vblender_hardirq`, `ge2d_afbd_hardirq`)
9. Panel GPIO request
10. Backlight init
11. `/dev/fb0` framebuffer init (1920×1080 ARGB8888)
12. PM runtime
13. `sunxi_tvtop_client_register`
14. OSD resume init → delayed work loads `LogoRegData.bin` firmware
    (1010-line parser for stock plane-init register streams)
15. LVDS watchdog thread
16. DLPC3435 companion init

IRQ handlers: `ge2d_vblender_hardirq` (port of stock `tgd_vblender_irq`),
`ge2d_afbd_hardirq` (port of `osd_afbd_irq`).

**Blocker**: binds to the same `compatible = "trix,ge2d"` as `h713_drm`.
One must be blacklisted. Currently we blacklist `sunxi_ge2d` — wrong
choice in retrospect.

Module params:
- `enable_fbdev` (default `true`)
- `enable_irqs` (default `true`)
- `enable_dlpc3435` (default `true`)
- `backlight_boot_on` (default `false`)
- `enable_lvds_watchdog` (default `false`)

### 3. `sunxi_decd` — inactive, probably dead for this path

`drivers/decd/` (out-of-tree).

Port of stock `decd.ko`. Has `dec_frame_submit` ioctl, pool-1 fill, vsync
handler, info-page allocator.

**The conclusion after sessions DD/DD-NIGHT/EE/autonomous**: stock
`decd.ko` is **dead code for the HDMI-RX path**. No userspace consumer
calls `DECD_IOC_FRAME_SUBMIT` in stock — confirmed by searching all
stock binaries for the immediate constant. Stock `decd` was probably
built for video playback (decoded H.264/MPEG2 frames from ffmpeg/OMX
with AFBC-compressed metadata in the info-page), not for HDMI-RX
scanout.

Sessions DD-NIGHT/EE/autonomous tried very hard to make `decd` work as
the HDMI-RX scanout driver: filled pool-1 byte-for-byte stock-conform,
fixed the info-page allocator bug, even corrected the magic constant in
the dma-buf path. Picture stayed black throughout. **Conclusion**: wrong
driver.

The mainline `decd` port DID gain a real bug fix from this:
`video_info_buffer_init` used to hardcode `y_phys + 4096` for the linear
path, which collided with the MIPS Y-plane. Now it uses
`alloc_video_info_page()` from `decd_reserved`. That's permanent and
correct.

`decd_submit_test` userspace tool is at `/usr/local/bin/decd_submit_test`
for whoever wants to exercise the ioctl.

See [docs/re/ge2d-port-notes.md](../re/ge2d-port-notes.md) for the
detailed comparison.

## Path forward

The plan, in order:

1. **Swap the blacklist**. Currently `blacklist sunxi_ge2d`. Make it
   `blacklist h713_drm` instead. `sunxi_ge2d` then probes for `5240000.ge2d`
   and runs its full 16-step init.
2. **Test**: should produce `/dev/fb0` (1920×1080 ARGB) + working LVDS
   output. If the picture is still wrong, iterate on the
   `ge2d_vblender_hardirq` register-writes (they only do 3 reads + 3
   writes per vsync — small surface to debug).
3. **Re-enable GPU + Wayland**: once LVDS works, bring `panfrost` back
   online + restart Wayland compositor on top of `sunxi_ge2d`'s fbdev.
4. **Re-enable Cedrus**: video decode acceleration back on.

If `sunxi_ge2d` doesn't give a clean picture either, the next-deepest
investigation is `tgd_vblender_irq` in stock — only 232 bytes, 3 register
writes, but the register-base comes from a global pointer that's set up
in `ge2d_drv_probe`. We'd need to trace that init flow to know what's
written where.

See [docs/re/ge2d-port-notes.md](../re/ge2d-port-notes.md) for the
RE notes on what the stock driver does that we might be missing.

## DRM device layout (when both are loaded — broken state)

| Device | Card | Function |
|---|---|---|
| Panfrost | card0 | GPU render, `renderD128` |
| h713_drm | card1 | Display scanout (KMS) |

PRIME buffer sharing works (verified). Weston used to launch with GL
renderer on this.

CMA pool blocker: `EGL_BAD_ALLOC` on `CREATE_DUMB` because the pool is
too small. Increase `cma=` cmdline or clean up reserved-mem.

## Quirks worth remembering

- **TVTOP must be programmed first**. Without it everything reads zero.
- **MIPS doesn't write the AFBD region directly**. Only DE2
  (`0x05000000-0x051FFFFF`) and INCAP (`0x06940000`). The AFBD pool-1
  fill is purely ARM-side responsibility.
- **`+0x310 bit 21` is hardware-protected**. We write `0x00800210`, HW
  rewrites it back to `0x00a00210`. Bit 21 is "ch0 not_ready" status.
- **AFBD register `+0x010` bits 0+1** are not state bits. They take
  writes (no HW protection) but don't seem to enable anything we can
  observe.

## See also

- [HDMI subsystem](hdmi.md) — HDMI input side (RX)
- [docs/re/display-bringup.md](../re/display-bringup.md) — the TVTOP
  fabric story
- [docs/re/ge2d-port-notes.md](../re/ge2d-port-notes.md) — stock vs
  mainline driver comparison
- [SESSIONS.md](../../SESSIONS.md) sessions BB-EE for the LVDS work
