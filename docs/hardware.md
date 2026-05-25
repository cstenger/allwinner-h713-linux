# Hardware overview

## The SoC

**Allwinner H713** (sun50iw12p1) — quad Cortex-A53 @ 1.5 GHz, 1 GB DDR3.
Sister parts: H6, H616, D1. Closer to D1 in pinctrl, closer to H6 in clock
tree, has its own quirks for both.

The H713 has **three CPU cores** people don't usually mention when they say
"quad-core":

1. **ARM Cortex-A53 (×4)** — runs Linux. This is what you boot.
2. **MIPS32 LE coprocessor** — runs `display.bin` (1.25 MB FreeRTOS-style
   firmware). Owns the display engine (DE2/AFBD/VBlender/LVDS), HDMI-RX
   state machine, and picture-quality pipeline. Loaded by U-Boot before
   Linux starts.
3. **OR1K BE ARISC SCP** — 172 KB firmware (extracted from BL31 TOC1).
   Owns PMU power sequencing, HPD pin, and (theoretically) EDID storage.

All three are running by the time you see the first Linux kernel print.

## The board

The HY310 is a portable DLP projector. Cheap, plastic case, fan-cooled. Same
SoC + similar layout appears in HY300, Magcubic, and other no-name
projectors.

Three communications buses connect the CPUs:

- **Msgbox** (`0x03003000`) — 3 user regions for ARM↔MIPS↔ARISC mailbox.
  Edge-triggered on H713 (not level-triggered like H6).
- **Shared memory** (`0x4E300000`, 5 MB) — primary ARM↔MIPS data channel.
- **HW spinlocks** (`0x03004000`, 16× 4-byte) — sun6i-style, used by
  `cpu_comm`.

## Memory map (top hits)

| Region | ARM phys | Purpose |
|---|---|---|
| Linux RAM | 0x40000000–0x4A700000 | usable DDR |
| MIPS firmware `display.bin` | 0x4B100000 | code/data |
| MIPS reserved | 0x4C300000–0x4D700000 | working memory + frame buffers |
| `decd_reserved` | 0x4D941000–0x4D961000 | info-page pool |
| `cpu_comm_reserved` (SharedMem) | 0x4E300000–0x4E7FFFFF | 5 MB ARM↔MIPS IPC |
| u-boot bootlogo | 0x78541000 | leftover from boot screen |
| msgbox HW | 0x03003000 | 3× 0x400 user regions |
| HW spinlocks | 0x03004000 | sun6i-style |
| MIPS-INTC | 0x03061300+ | MIPS interrupt controller |
| MIPS-loader regs | 0x03061000+ | SHARE_ADDR, SHARE_SIZE, BOOT_ADDR |
| HDMI-RX Synopsys | 0x050C0000–0x050CFFFF | DW-HDMI-RX (CMU/PHY/SCDC/DMA, EDID-RAM) |
| HDMI-RX H713 wrapper | 0x06800800–0x068008FF | port-select, PHY-reset (MIPS-side) |
| HDMI-RX H713 port-state | 0x06840000–0x068408FF | per-port, byte-multiplexed |
| Display engine (DE2) | 0x05000000–0x051FFFFF | scanout pipeline |
| TVTOP bus fabric | 0x05700000 | gates display sub-blocks |
| AFBD | 0x05600000 | frame buffer controller |
| LVDS PHY | 0x051C0000 | (MIPS-initialized) |
| TV303 PMU | 0x07001000 | 5 power domains |
| HPD pin | 0x07091014 | 3-bit register, undocumented |
| ARISC NSI MBUS | 0x02020000 | port[4]=TVFE, prio=2 |

## Address translation between ARM and MIPS

This catches everyone the first time. MIPS sees physical RAM differently:

- **Code/data**: `phys = MIPS-VA − 0x40000000`
  Example: `0x8B100000` → `0x4B100000` (KSEG0 cached)
- **Shared memory**: `phys = MIPS-VA − 0x60000000`
  Example: `0xAE300000` → `0x4E300000` (KSEG1 uncached)
- **HW MMIO**: MIPS uses a readl-helper at VA `0x8B17FC70` that does
  `phys = (addr + 0xB5000000) | 0x20000000` internally.

The mainline `cpu_comm` driver has the translation helpers
(`Vir2Mid`/`Mid2Vir`/`Mid2Phy`/`Phy2Mid`) since session W.

## Components on the board

| Component | Where | Notes |
|---|---|---|
| eMMC | 7.3 GB Samsung KLM8G1GETF | HS200 @ 200 MHz SDR |
| Wi-Fi | AIC8800D80 (onboard, SDIO) | 802.11ac dual-band |
| Bluetooth | AIC8800 BT 5.4 | UART1 @ 1.5 Mbaud |
| IR remote | PL9 pin (mux 3) | NEC protocol, see [IR notes](subsystems/uart.md) |
| Accelerometer | STK8BA58 on TWI1 | also DA228EC, LSM6DSR detected on some boards |
| Audio codec | Internal at 0x02030000 | speaker output |
| LVDS panel | DLPC3435 → DLP imager | 1920×1080, the only display output. Picture currently broken (4×1 tiling) |
| HDMI-IN | HDMI 1.4 socket | Synopsys DW-HDMI-RX (only HDMI port on the device) |
| Keystone motor | 4-phase stepper | GPIO-driven, limit switch PH14 |
| Fan | PWM with tachometer on PH17 | NTC thermistor on LRADC |
| Power | DC barrel jack | dedicated input, no USB power |

## GPIO and pinctrl quirks

- **H713 pinctrl bank spacing is 0x30** (D1-style), not 0x24 (H6). Using
  H6 spacing causes silent writes to wrong registers.
- **R_PIO PM bank** needs `SUNXI_PINCTRL_NEW_REG_LAYOUT`. Without it
  `gpio_direction_output()` fails silently.
- **PB5 controls both panel backlight AND fan power**. Never write
  PB5 = LOW — fans stop, board overheats.
- **Buttons**: ADC keys on LRADC don't seem to be wired the way stock DTS
  claims. The stock DTS defines 6 LRADC key thresholds but live reads stay
  at 63 (max). IR remote works independently.

## IRQs (GIC SPI / Linux IRQ numbers)

| SPI | Linux IRQ | Source | Notes |
|---|---|---|---|
| 24 | — | IOMMU 0x030f0000 | Panfrost IOMMU |
| 46 | 262 | msgbox User0 | ARM RX (from MIPS+ARISC) |
| 65 | 266 | hdmi-rx@5000000 | registered but **never fires** |
| 108 | 347 | msgbox User2 | MIPS peer; mainline GIC doesn't route |
| 109 | 348 | msgbox User1 | ARISC peer; same |
| 142 | 355 | decd | vsync handler (was 0 hits, now lives once decd engaged) |
| 172 | — | NMI controller | sun9i-a80-nmi |

The IRQs at SPI 108/109 are a known dead-end — stock Android uses a vendor
`wakeupgen` IRQ-controller layer that mainline GIC doesn't replicate. DTS
edits without porting wakeupgen are useless.

## Next reads

- [Architecture](architecture.md) — how the three CPUs coordinate.
- [Boot sequence](subsystems/boot.md) — how U-Boot brings everything up.
- [Hacking guide](contributing-re.md) — tools and workflow for RE work.
