# Thermal + fan

**Status**: ✅ works.

## Thermal sensors

- **THS** at `0x02009400`, driver `sun8i-thermal`.
- 2 zones — CPU and GPU.
- Typical idle temps: CPU ~65 °C, GPU ~66 °C.
- SID calibration: offset `0x14`, 8 bytes.

```sh
cat /sys/class/thermal/thermal_zone0/temp
cat /sys/class/thermal/thermal_zone1/temp
```

## Fan control

Driver: `hy310-board-mgr` (in `drivers/misc/`).

Hardware:
- **Fan PWM**: PWM channel on the H713 8-channel controller
- **Fan tachometer**: on PH17 — read via **hrtimer GPIO polling at
  100 µs intervals**, NOT GPIO IRQ
- **NTC thermistor**: read via LRADC (see [lradc.md](lradc.md))
- **Fan power GPIO**: PB5 — shared with backlight, never write LOW

### Why hrtimer polling for tachometer

GPIO-IRQ was extensively tested on H713 PH11–PH19 extended pins and
**does not work**:
- IRQ registers correctly via `request_irq`
- Pin value toggles visibly (fan pulses confirmed via GPIO reads)
- But the EINT hardware controller never fires the interrupt (counter
  stays 0)
- Confirmed by multiple independent investigations
- Stock kernel uses IRQ but may have vendor-specific EINT init we lack

The hrtimer polling reads the PIO `DAT` register directly via
`__raw_readl()`, bypassing the GPIO subsystem (which on H713 can alter
the pin function and break the input). RPM is calculated from edge
transitions counted over 1-second intervals.

Typical readings: 2500-2600 RPM at normal operation.

### Fan stall detection

If RPM drops below threshold for multiple cycles, emergency shutdown
triggers (matching stock behavior).

## PB5 warning — read this

**`PB5` controls both panel backlight AND fan power** (shared hardware
line). Writing PB5 = LOW kills both. Never do that.

`hy310-board-mgr` manages PB5. If you're poking GPIOs from userspace,
leave PB5 alone.

## Board manager — the bigger picture

`hy310-board-mgr` is a multi-purpose driver that handles:

- Fan PWM control
- Fan tachometer (hrtimer polling)
- USB power GPIO
- NTC thermistor (via LRADC IIO consumer API)
- Power LED / status LED
- Fan power GPIO (PB5)
- Power hold
- DC-in detect
- Charge-OK signal
- Battery GPADC

The stock `pwm_fan` driver was the reference. Our implementation
consolidates all the board-level peripherals into one driver because
they're all interrelated (fan vs backlight vs power hold, etc.).

## Debugging

```sh
ssh hy310 'cat /sys/class/hwmon/hwmon*/temp*_input'
ssh hy310 'cat /sys/class/hwmon/hwmon*/fan*_input'
ssh hy310 'cat /sys/devices/platform/hy310-board-mgr/*'
```
