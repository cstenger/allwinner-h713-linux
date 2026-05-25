# Audio

**Status**: ✅ speaker output works via internal codec.

## What works, what doesn't

- ✅ Speaker output via internal codec (this page)
- ✅ Digital volume control (0-63 via ALSA)
- ❌ HDMI audio (not started)
- ❌ Microphone / capture (not tested)
- ❌ TridentALSA DSP effects (would need MIPS IPC — not done)

## Signal chain

The H713 has an internal audio codec at `0x02030000`. Speaker output goes
through the **Audio Hub** path (not the I2S path, not the MIPS DSP):

```
ALSA → APB DMA → DAC → Analog Mixer → PA Amplifier → Speaker
```

Earlier assumption that audio required the MIPS DSP was wrong for basic
playback. MIPS is only involved for DSP effects (TridentALSA), which we
haven't implemented.

## Drivers

`drivers/audio/` (out-of-tree):

| Module | Compatible | Function |
|---|---|---|
| `snd-soc-sunxi-h713-codec.ko` | `allwinner,sunxi-internal-codec` | codec + analog mixer |
| `snd-soc-sunxi-h713-cpudai.ko` | `allwinner,sunxi-dummy-cpudai` | DMA platform driver |
| `snd-soc-sunxi-h713-machine.ko` | `allwinner,sunxi-codec-machine` | ALSA sound card |

Autoloaded at boot via `/etc/modules-load.d/audio.conf`.

## ALSA controls

| Control | Range | Default | Function |
|---|---|---|---|
| `digital volume` | 0-63 (inverted: 63 = max) | 32 (50%) | master volume |
| `Speaker` | on/off | on | PA amplifier enable (GPIO PL2) |

## Critical register init

Set in `sunxi_codec_init`:

| Register | Value | Purpose |
|---|---|---|
| `DAC_DPC` bit 0 | 1 | Audio Hub output ON (**mandatory**!) |
| `DAC_DPC` bit 29 | 0 | DAC source = APB (not I2S) |
| `DAC_VOL_CTRL[15:8]` | 90 | calibrated DAC volume (max without clipping) |
| `HP_REG` | `HP_EN_L`+`HP_EN_R`+`HP_AMP_EN` | HP amp drives internal mixing bus |
| `DAC_REG 0x310` | `0x0B15FC7F` | full analog output path enabled |

## Findings worth knowing

- **Audio Hub Output = ON is mandatory** for speaker sound via APB path.
- **Headphone amp must be ON** even without headphones plugged in — it
  drives the internal mixing bus that feeds the speaker. Without it,
  volume is ~50% lower.
- **DAC volume > 90 has no additional gain** but values < 68 reduce
  output. We cap at 90 to prevent clipping.
- **DAC source must be APB (not I2S)**. The I2S path produces no sound
  on H713. Stock uses I2S differently with a 3-clock setup we don't
  replicate.

## GPIO

- **PL2** (R_PIO): PA amplifier enable, active HIGH. Controlled by the
  `Speaker` ALSA control. 160 ms settling delay after enable.

## Quick test

```sh
ssh hy310 'aplay -l'                          # should show 'card 0: ...'
ssh hy310 'aplay /usr/share/sounds/alsa/Front_Center.wav'
```

## Open work

- HDMI audio (separate subsystem, has DTS nodes but no driver work yet)
- Microphone / capture path testing
- TridentALSA DSP path (needs MIPS IPC integration — low priority)
