# Display bringup — TVTOP root cause analysis

How we figured out that all display sub-blocks read zeros until TVTOP is
programmed. This is a history page; for current state see
[docs/subsystems/display.md](../subsystems/display.md).

## The problem

After bringing up CCU clocks and deasserting `rst_bus_disp`, every
display sub-block read zero:

- VBlender / OSD_B (`0x05200000`) → `0x00000000`
- GE2D core (`0x05240000`) → `0x00000000`
- LVDS controller (`0x051C0000`) → `0x00000000`
- AFBD (`0x05600000`) → `0x00000000`

## Hypotheses tested and ruled out

| Hypothesis | Result | Evidence |
|---|---|---|
| PPU DE power domain off | ❌ ruled out | PPU is at `0x07001000` (not `0x07010000`), domain 4 `pwr_ctrl=0x01`, status `0x00010000` = ON |
| Display clocks not enabled | ❌ ruled out | Test module: `bus-disp` enable_count=1, `afbd` enable_count=1, correct rates |
| `RST_BUS_DISP` still asserted | ❌ ruled out | Test module: `reset_control_deassert()` returned 0 |
| Wrong CCU register offsets | ❌ ruled out | Stock vmlinux IDA confirmed `RST_BUS_DISP @ 0xDD8 BIT(16)`, matches our CCU |
| **TVTOP bus routing not configured** | ✅ ROOT CAUSE | Stock `tvtop_tvdisp_enable` writes 7 routing registers to `0x05700000` |

## The fix

From stock `tvtop_tvdisp_enable` (offset `0x029c` in `sunxi_tvtop.ko`):

```c
// Full tvdisp enable sequence:
1. clk_prepare_enable(svp_dtl_clk)     // 200 MHz from pll-periph0-2x
2. clk_prepare_enable(deint_clk)       // 1032 MHz from pll-video2-4x
3. clk_prepare_enable(panel_clk)       // 1032 MHz from pll-video2-4x
4. clk_prepare_enable(clk_bus_disp)    // 150 MHz from ahb
5. reset_control_deassert(rst_bus_disp)
6. Write TVTOP routing registers:      // 0x05700000 base
   TVTOP+0x04 = 0x00000001
   TVTOP+0x44 = 0x11111111
   TVTOP+0x88 = 0x11111111         // was 0xFFFFFFFF in initial port — EE-fixed
   TVTOP+0x00 = 0xFFF11111         // was 0x00011111 in initial port — EE-fixed
   TVTOP+0x40 = 0x00011111
   TVTOP+0x80 = 0x00001111
   TVTOP+0x84 = 0xFFF000EF         // 1080p-specific
```

After step 6, display sub-blocks respond to MMIO reads.

## Corrected topology

```
              CCU (0x02001000)
                |
                +-- bus-disp (0xDD8 BIT0 gate, BIT16 reset)
                +-- afbd (0xDC0 BIT31 gate, M/MUX/GATE)
                +-- svp-dtl, deint, panel
                |
              TVTOP (0x05700000)  ← BUS FABRIC ROUTER
                |                    Routing regs gate sub-blocks
                |
    +-----------+-----------+-----------+
    |           |           |           |
 VBlender    GE2D core    LVDS PHY    AFBD
 0x05200000  0x05240000   0x051C0000  0x05600000
```

## PPU power domain — corrected base

**Base: `0x07001000`** (NOT `0x07010000` as previously assumed)

| Domain | Index | Base | Status |
|---|---|---|---|
| pd_gpu | 0 | `0x07001000` | ON (`pwr_ctrl=1`, status `0x10000`) |
| pd_tvfe | 1 | `0x07001080` | ON |
| pd_tvcap | 2 | `0x07001100` | ON |
| pd_ve | 3 | `0x07001180` | ON |
| pd_de (?) | 4 | `0x07001200` | ON |
| (unused) | 5 | `0x07001280` | all zeros |

All power domains are ON from U-Boot. No power domain driver is needed
for initial bringup (but should be implemented for proper PM).

## See also

- [Display subsystem](../subsystems/display.md) — current state and path
  forward
- [GE2D port notes](ge2d-port-notes.md) — what we found in the stock
  display driver after this initial bringup
