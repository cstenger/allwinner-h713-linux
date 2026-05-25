# LRADC (low-resolution ADC)

**Status**: ✅ working (IIO provider). Used for NTC thermal sensing.

Buttons-via-LRADC are unclear — see below.

## Hardware

- **Address**: `0x02009800`
- **Registers**: `CTRL` (+0x00), `INTC` (+0x04), `INTS` (+0x08), `DATA0`
  (+0x0c), `DATA1` (+0x10)
- **Resolution**: 6-bit (0–63)
- **Reference voltage**: ~1.8 V (AVCC)
- **CCU gate**: `bus-lradc` at offset `0xA9C` (BIT 0 = gate, BIT 16 = reset)

## Driver

- **File**: `drivers/lradc/sun50i-h713-lradc-iio.c` (built-in:
  `CONFIG_SUN50I_H713_LRADC=y` — probes before board-mgr)
- **Compatible**: `allwinner,sun50i-h713-lradc`
- **Type**: IIO voltage provider, 2 channels

## Architecture

The LRADC hardware is owned exclusively by the IIO driver. Consumers
access it through the kernel IIO consumer API:

```
LRADC HW (0x02009800)
    |
    v
sun50i-h713-lradc-iio (IIO provider, 2 voltage channels)
    |
    +---> hy310-board-mgr  (io-channels = <&lradc 0>, NTC temperature)
    +---> adc-keys         (io-channels = <&lradc 0>, keyboard — see note)
```

This replaced an earlier direct-MMIO approach (`of_iomap` + `readl`)
that caused resource conflicts and had a wrong-register-offset bug
(`DATA0` was at `0x08` instead of the correct `0x0c`).

## DTS

```dts
lradc: lradc@2009800 {
    compatible = "allwinner,sun50i-h713-lradc";
    reg = <0x02009800 0x400>;
    clocks = <&ccu CLK_BUS_LRADC>;
    resets = <&ccu RST_BUS_LRADC>;
    #io-channel-cells = <1>;
    status = "okay";
};
```

## NTC temperature sensing

`hy310-board-mgr` reads NTC thermistor values via
`iio_read_channel_raw()`. The DTS reference:

```dts
fan {
    io-channels = <&lradc 0>;
    io-channel-names = "ntc";
};
```

Works reliably. Used by the fan control loop and by stall detection.

## Physical buttons — status unclear

The stock DTS defines a `keyboard@2009800` node with 6 LRADC key
thresholds, and the U-Boot DTS defines `fastboot_key` and
`recovery_key` with ADC value ranges. **But on the HY310 the LRADC
voltage stays at 63 (max) regardless of button presses.**

The physical buttons on the HY310 may be:
- Connected to a different input (GPIO-based, possibly PA4 as
  `power-gpio`)
- On a different ADC channel
- Not connected to the LRADC at all on this board variant

The `adc-keys` DTS node is present but non-functional until the button
input source is clarified.

The IR remote works independently via `sunxi-cir` so this isn't a
blocker.

## Debugging

```sh
ssh hy310 'cat /sys/bus/iio/devices/iio:device*/in_voltage*_raw'
```
