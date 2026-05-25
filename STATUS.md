# Subsystem Status

> **Alpha.** Honest snapshot, updated 2026-05-25. "Working" means verified
> on hardware, not "compiles".

## TL;DR

- ✅ ARM userland, networking, IPC, audio, thermals, eMMC, USB, Wi-Fi, BT: **working**
- ✅ ARM↔MIPS IPC + callback delivery + signal detection: **working**
- ⚠️ HDMI input picture: **broken** (4×1 XRGB grayscale tiling on the projector)
- ⚠️ GPU + video decoding: **disabled** in current build
- ⚠️ Wayland desktop: **off** — current build routes the display through
  the HDMI-RX scanout path (which is what we're debugging), not through a
  compositor
- **No HDMI output port exists** on the HY310. Only HDMI input. The DLP
  projector itself is the only display output.

## Subsystem table

| Subsystem | Status | Driver | Notes |
|---|---|---|---|
| Boot | ✅ works | stock U-Boot + patched env | Android boot v3, cmdline in DTS |
| Serial console | ✅ works | `8250_dw` | `ttyS0 @ 115200` |
| eMMC | ✅ works | `sunxi-mmc` patched | **HS200 @ 200 MHz SDR**, ~50 MB/s. Was HS400; downgraded after second board misbehaved |
| USB | ✅ works | ehci/ohci + `phy-sun4i-usb` patched | 3× EHCI + 3× OHCI, needs PMU bit-0 quirk |
| Wi-Fi | ✅ works | `aic8800_bsp` + `aic8800_fdrv` OOT | AIC8800D80 SDIO, stable |
| Bluetooth | ✅ works | `hci_uart` H4 + `aic8800_btlpm` | UART1, 1.5 Mbaud, flow control |
| IR remote | ⚠️ partial | `sunxi-cir` + rc-core | NEC decoder flaky (R_CCU prescaler mismatch). lircd raw works |
| RTC | ✅ works | `sun6i-rtc` | @ 0x07090000 |
| Thermal | ✅ works | `sun8i-thermal` | 2 zones (CPU ~65 °C, GPU ~66 °C) |
| I2C | ✅ works | `mv64xxx` | TWI1, STK8BA58 detected |
| PWM | ✅ works | `pwm-sun8i` (new driver) | 8 channels |
| Fan | ✅ works | `hy310-board-mgr` | PWM + tachometer (hrtimer polling) + NTC |
| Watchdog | ✅ works | `sunxi-wdt` | @ 0x02051000 |
| Reboot / poweroff | ✅ works | — | verified |
| Keystone motor | ⚠️ untested | `hy310-keystone-motor` | sysfs works, limit switch defective on test unit |
| Audio | ✅ works | codec + cpudai + machine OOT | Speaker output + digital volume, no HDMI audio |
| **ARM↔MIPS IPC** | ✅ works | `cpu_comm` OOT | Full bidirectional, callback-delivery via `.read`/`.poll` patch |
| **MIPS coprocessor** | ✅ works | `sunxi-mipsloader` (built-in) | APP_READY=0x5, all 81 routines registered, state machine 1→5 (Running) |
| **HDMI-RX signal detection** | ✅ works | MIPS firmware + `hy310-hdmird` | 1080p60 detected, signal callbacks fire on ARM |
| **HDMI-RX picture quality** | ❌ broken | `h713_drm` + `mips_scanout_addr` override | Shows on projector as **4×1 XRGB grayscale tiling**. Channel format mismatch. Next: swap to `sunxi_ge2d`. |
| **DLP projector output (LVDS)** | ⚠️ broken picture | `h713_drm` ch1 XRGB hack | The only output the device has. Currently shows the broken HDMI-RX content. |
| GPU | ⚠️ disabled | `panfrost` (built but not loaded) | Worked previously; off while debugging display pipeline |
| Wayland desktop | ⚠️ off | Labwc + Panfrost | Output is currently routed to the HDMI-RX scanout path, not a compositor |
| Video decode (Cedrus) | ⚠️ disabled | `sunxi-cedrus` (built but not loaded) | Same as GPU. H.264/H.265/MPEG-2/VP8 |
| Video decode (AV1) | ⚠️ untested | `sun50i-h713-av1` (in `drivers/media/av1/`, not in defconfig) | Standalone H713 AV1 hw decoder, V4L2 stateless. Source present, never enabled or tested |
| `sunxi_tvtop` (display bus fabric + clocks) | ✅ works | OOT module | Required for any display sub-block to respond. Used as client by `h713_drm`, `sunxi_ge2d`, `sunxi_decd` |
| IOMMU | ⚠️ provider only | `sun50i-iommu` (built-in) | Runs; no consumer attached |
| GPADC | ✅ works | `sun20i-gpadc` IIO | 2 channels |
| LRADC | ✅ works | `sun50i-h713-lradc` IIO | NTC temp sensing |
| LRADC keyboard | ❓ unclear | — | 6 keys + power, but ADC stays at 63 — wiring unclear |
| Crypto engine | ✅ works | `sun8i-ce` (built-in) | 33 algorithms |
| HW spinlock | ✅ works | `sun6i-hwspinlock` (built-in) | Used by `cpu_comm` |

## Why some things are off right now

### Current output routing — projector via HDMI-RX path

The HY310's only display output is the DLP projector via LVDS. We have
**no HDMI output port** on the device.

Right now the projector shows what comes through the HDMI-RX path: 4×1
grayscale tiling because AFBD channel-1 is in XRGB (32 bpp) mode while
MIPS writes NV12 (12 bpp) into the buffer. Every row's 1920 NV12-Y bytes
get interpreted as 480 XRGB pixels, then tiled 4× to fill the row.
Chroma is ignored → grayscale.

Wayland desktop is intentionally **off** in the current build. Until the
HDMI-RX scanout is producing a clean picture, running a compositor on
top doesn't make sense.

### h713_drm — minimal scanout shim

`h713_drm` is a thin DRM/KMS driver that hacks the AFBD scanout pointer
to point at MIPS's output buffer (via `mips_scanout_addr=0x4c3ef000`).
With it loaded you get the broken 4×1 picture. Without it: no picture
on the projector at all.

The right driver is `sunxi_ge2d` (3000-line port already in the repo at
`drivers/display/ge2d/`), which is blacklisted because of a DT-binding
conflict — we picked the wrong one. Next step: swap the blacklist. See
[docs/subsystems/display.md](docs/subsystems/display.md).

### Wayland + Panfrost + Cedrus

These all worked previously. They're temporarily off while we debug the
HDMI-RX path — the GPU was using the display pipeline aggressively and
made register-state diffs unreadable. The plan is to re-enable them
after `sunxi_ge2d` is active and the HDMI-RX path is clean.

### eMMC HS400 → HS200

The first device handled HS400 fine. A second device showed intermittent
CRC errors and rare hangs at HS400. We downgraded the DTS to HS200 for
portability across boards. See
[docs/subsystems/emmc.md](docs/subsystems/emmc.md).

## Verifying a fresh build

```sh
# ARM userland up
ssh hy310 'uname -a; uptime'

# IPC working
ssh hy310 '/usr/local/bin/test_brightness_call'

# MIPS up and running
ssh hy310 'cat /sys/class/hy300/mips/state_machine'   # expect 5 (Running)

# HDMI-RX signal detection
ssh hy310 'systemctl status hy310-hdmird; journalctl -u hy310-hdmird -n 30'

# Display output: look at the projector. Expect 4×1 grayscale of HDMI source.
```

## Open issues (highlights)

Full list: [docs/known-issues.md](docs/known-issues.md). Highlights:

- HDMI-RX picture quality: 4×1 XRGB grayscale tiling. Path forward:
  `sunxi_ge2d` swap.
- IR remote NEC decoder: unreliable due to R_CCU prescaler. Use lircd raw.
- Msgbox TX reliability: 40-60% (edge-trigger H713). Pulse-doorbell workaround.
- HPD plug/unplug: not auto-detected (IRQ 266 silent). Polling planned.
- EDID DMA readback: zero but functionally works. Mechanically not understood.

## Not started

- TVCAP/INCAP frame-capture pipeline (skeleton exists in `sunxi_tvcap_rx.ko`)
- LRADC physical keyboard
- HDMI audio

## How to contribute

See [docs/contributing-re.md](docs/contributing-re.md) for RE workflow,
tools (IDA, capstone, `hreg`/`hdump`), and which problems are currently
actionable.
