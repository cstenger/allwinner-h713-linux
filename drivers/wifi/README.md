# AIC8800D80 Wi-Fi + Bluetooth driver

Out-of-tree driver for the AIC8800D80 SDIO Wi-Fi + Bluetooth chip on the
HY310 board. Based on the aic8800-v5 port from the AIC vendor SDK,
adapted for mainline kernel 6.16.7.

## Modules

| Module | Function |
|---|---|
| `aic8800_bsp` | board support: SDIO init, firmware upload, power control |
| `aic8800_fdrv` | full-MAC driver, creates `wlan0` interface |
| `aic8800_btlpm` | Bluetooth low-power management + rfkill |

## Prerequisites

1. Kernel patched with `sunxi-mmc` H713 v5p3x support (patch 0006)
2. AIC8800D80 firmware in `/lib/firmware/aic8800D80/` (from stock Android)
3. R_PIO pinctrl with `SUNXI_PINCTRL_NEW_REG_LAYOUT` (patch 0003)

## Loading Wi-Fi

```sh
modprobe aic8800_bsp
modprobe aic8800_fdrv
# wlan0 (or wlan1) interface appears

wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
dhclient wlan0
```

## Loading Bluetooth

```sh
modprobe aic8800_btlpm
rfkill unblock bluetooth
hciattach /dev/ttyS1 any 1500000 flow
# hci0 appears

bluetoothctl
> scan on
```

## Systemd autoload

```ini
# /etc/systemd/system/aic8800-wifi.service
[Unit]
Description=AIC8800 Wi-Fi
After=network-pre.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe aic8800_bsp
ExecStart=/sbin/modprobe aic8800_fdrv

[Install]
WantedBy=multi-user.target
```

## Known issues

- **BT cold-boot autostart fails** at 1.5 Mbaud. Chip is at 115200 at
  power-on; autostart's negotiation race-condition makes hciattach fail
  on first boot. Retry loop in the init service works around it.
  Manual `hciattach` always works.
- **Wi-Fi runs at High Speed 25 MHz**, not SDR104 @ 50 MHz, because
  SDR104 needs 1.8 V signal-voltage switching that isn't wired on the
  HY310 board.
- **Firmware version**: stock-Android Dec 2024 or newer works. Older
  versions cause init timeout. Not redistributable — extract from
  `super.fex` vendor partition.

## Port history

This is a port of radxa-pkg/aic8800 main (`445a655fc5fb8deb`) onto Linux
6.16.7 ARM32 for Allwinner H713. The port preserves H713-specific
platform glue in `aicsdio.c` (`4021000.mmc`, PM1 `wlan_regon`,
`mmc_detect_change`).

Substantial V5 API updates were merged (`from_timer` → `timer_container_of`,
`del_timer_sync` → `timer_delete_sync`, `MODULE_IMPORT_NS` syntax,
`set_monitor_channel` 3-arg, `get_tx_power` 4-arg with `link_id`).

See [PORT_REPORT.md](PORT_REPORT.md) for the per-file change log.

## See also

- [docs/subsystems/wifi-bt.md](../../docs/subsystems/wifi-bt.md) — usage
  side
- [docs/subsystems/emmc.md](../../docs/subsystems/emmc.md) — the
  sunxi-mmc patches that affect SDIO
