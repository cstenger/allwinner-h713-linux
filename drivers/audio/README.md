# HY310 audio drivers

Out-of-tree drivers for the H713 internal audio codec. Speaker output
works via the Audio Hub path (DAC → analog mixer → PA amplifier).

## Modules

| Module | Source | Function |
|---|---|---|
| `snd-soc-sunxi-h713-codec` | `snd-soc-sunxi-h713-codec.c` | internal codec (DAC + ADC + speaker PA) |
| `snd-soc-sunxi-h713-cpudai` | `snd-soc-sunxi-h713-cpudai.c` | CPU-DAI / DMA platform (DMA ch7) |
| `snd-soc-sunxi-h713-machine` | `snd-soc-sunxi-h713-machine.c` | machine driver (creates ALSA card) |

## Loading

```sh
modprobe snd-soc-sunxi-h713-machine
# auto-loads codec + cpudai as dependencies
```

Autoload at boot via `/etc/modules-load.d/audio.conf`.

## Verification

```sh
aplay -l                           # should show: card 0: audiocodec
amixer -c 0 contents               # 14 controls expected
speaker-test -c 2 -t wav
```

## Important notes

- **Speaker audio uses the Audio Hub path**, not the I2S path and not
  the MIPS DSP. Earlier assumption that audio required MIPS for basic
  playback was wrong. MIPS is only needed for DSP effects, which we
  don't implement.
- **Audio Hub Output bit must be ON** (`DAC_DPC bit 0 = 1`). Without it,
  speaker stays silent.
- **HP amplifier must be ON** even with no headphones plugged in — it
  drives the internal mixing bus that feeds the speaker. Without it,
  volume is ~50% lower.

See [docs/subsystems/audio.md](../../docs/subsystems/audio.md) for the
full register init + analysis.

## Audio bridge (kept for reference)

The `bridge/` subdirectory has the TridentALSA audio bridge
(`snd-hy310-trid-audio-bridge`) — a routing block between the internal
codec, I2S, and S/PDIF (OWA). Stock uses it for HDMI audio passthrough,
but the HY310 has no HDMI output port, so this is mostly irrelevant on
this board. Kept here in case the same chip + board is reused with a
wired HDMI-TX in the future.
