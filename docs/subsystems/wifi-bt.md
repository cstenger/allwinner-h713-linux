# Wi-Fi + Bluetooth

**Status**:
- Wi-Fi ✅ stable
- Bluetooth ✅ works (manual attach is rock-solid, cold-boot autostart
  has a baud-rate race)

## Hardware

- **Chip**: AIC8800D80
  - Wi-Fi: SDIO, VID `0xC8A1` / DID `0x0082`
  - BT: UART1 (`ttyS1`), H4 protocol, 1.5 Mbaud, flow control
- **MMC controller for Wi-Fi**: `0x04021000` (MMC1)
- **Pin mux**: PG0–PG5 for SDIO
- **Power**: PM1 (`wlan_regon`) via R_PIO

## Drivers

`drivers/wifi/` (out-of-tree, based on AIC vendor SDK aic8800-v5 port):

| Module | Function |
|---|---|
| `aic8800_bsp` | board support: SDIO init, firmware upload, power control |
| `aic8800_fdrv` | full-MAC driver, creates `wlan` interface |
| `aic8800_btlpm` | Bluetooth low-power management + rfkill |

## Critical patches that make this work

### R_PIO new register layout (patch 0003)

R_PIO bank spacing on H713 is `0x30`, not `0x24` like H6. With wrong
spacing, `gpio_direction_output()` silently writes to wrong registers
and `wlan_regon` never asserts → Wi-Fi power never comes up. The
`SUNXI_PINCTRL_NEW_REG_LAYOUT` flag is mandatory.

### sunxi-mmc v5p3x support (patch 0006)

Multiple fixes — see [emmc.md](emmc.md). Required for stable SDIO at
high speeds.

## Firmware

Files in `/lib/firmware/aic8800D80/`. Not redistributable — extract from
stock Android `super.fex` vendor partition or AIC vendor SDK. Confirmed
working versions: Dec 2024 / Nov 2024 from stock.

Required files (approximate):
- `fmacfw.bin`
- `fw_patch_table_u03.bin`
- `fw_adid_u03.bin`
- `fw_patch_u03.bin`
- plus a few more — `modprobe` will tell you what's missing

## Loading

Autoloaded at boot via `/etc/modules-load.d/wifi.conf`:

```
aic8800_bsp
aic8800_fdrv
aic8800_btlpm
```

## Bluetooth attach

UART1, 1.5 Mbaud:

```sh
hciattach /dev/ttyS1 any 1500000 flow
```

**Manual attach is reliable.** Autostart via systemd has a race on cold
boot: the BT chip is at default 115200 baud at startup, hciattach
negotiates up to 1500000, but the cold-boot timing is off and the
negotiation occasionally fails.

Workaround: retry loop in the init service. Real fix: add baud-rate
negotiation retry to the autostart sequence.

## Performance

- Wi-Fi: ~1 MB/s sustained on 10+ MB transfers, no errors observed.
- BT: standard 5.4 features, A2DP works.

## Debugging

```sh
# Wi-Fi:
ssh hy310 'iw dev wlan0 link'
ssh hy310 'dmesg | grep -i aic8800'

# Bluetooth:
ssh hy310 'hciconfig hci0 up; bluetoothctl'
```

## See also

- [eMMC subsystem](emmc.md) — sunxi-mmc patches that affect SDIO
