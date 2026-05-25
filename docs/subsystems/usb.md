# USB

**Status**: ✅ works.

## Hardware

- 3× EHCI controllers
- 3× OHCI controllers
- One external USB-A socket on the HY310 case

## PHY

Driver: `phy-sun4i-usb` with H713 quirk.

**H713 quirk** (patch 0005): `pmu_enable_bit0` — `BIT(0)` at the PMU
base register must be set, or the PHY won't initialize. Undocumented.

## Boot requirement

U-Boot must run `usb start` before kernel handoff. The PHY initialization
performed by U-Boot is required — the kernel doesn't re-init from
scratch.

This is encoded in `env_a`:

```
bootcmd=usb start;run setargs_nand boot_normal
```

If you flash a new env_a and forget the `usb start`, USB devices won't
work in kernel. Use [sunxi-env-patcher](https://github.com/well0nez/sunxi-env-patcher)
to add it.

## What you can plug in

- USB-Ethernet adapters (e.g., RTL8153) — used for SSH/flashing
  alongside Wi-Fi
- USB sticks — used for rootfs (`/dev/sda2`)
- USB hubs work
- Standard USB devices (HID, mass storage) work

## What's NOT there

- No USB device/gadget mode (HY310 is host-only on this socket)
- No USB-C
- No power input via USB — power is via DC barrel jack

## Debugging

```sh
ssh hy310 'lsusb -t'
ssh hy310 'dmesg | grep -i usb | tail -30'
```
