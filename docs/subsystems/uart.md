# UART / serial console

Console access for U-Boot interruption, kernel debugging, and recovery
when SSH isn't available.

## Settings

- **Baud rate**: 115200
- **Data bits**: 8
- **Parity**: none
- **Stop bits**: 1
- **Flow control**: none

## Hardware connection

UART pins are on the HY310 main board. You need a **3.3 V USB-UART
adapter** — 5 V will fry the SoC. Common choices that work:

- CP2102-based adapters (most common, ~5 €)
- CH340/CH341 adapters (cheap, also fine)
- FT232/FT232H (more expensive but very reliable)

Any 3.3 V adapter from your favorite electronics shop is fine. Avoid
"5 V default" adapters that need a jumper change.

Wiring:

```
HY310 TX  ─── adapter RX
HY310 RX  ─── adapter TX
HY310 GND ─── adapter GND
```

> Cross TX/RX. The HY310's transmit pin connects to the adapter's
> receive pin, not transmit-to-transmit.

> Do not connect 3.3 V power between the boards. Power the HY310 from
> its own DC barrel jack, only share GND with the UART adapter.

## Terminal software

### Linux / macOS

```sh
# Linux:
screen /dev/ttyUSB0 115200
minicom -D /dev/ttyUSB0 -b 115200

# macOS:
screen /dev/tty.usbserial-* 115200
```

### Windows

PuTTY, Tera Term, or [serial](https://serial.com) — select COM port,
115200 8N1, no flow control.

### tio (recommended)

[tio](https://github.com/tio/tio) is a nicer serial console tool than
screen/minicom:

```sh
tio /dev/ttyUSB0 -b 115200
```

## Optional: network-to-UART bridge

If your dev machine isn't close to the projector, you can put a
"middle-man" board between them that exposes the UART over TCP. We use
an Orange Pi PC running `ser2net` as a one-off setup at our bench:

```
Dev PC --(TCP:9999)--> bridge board (/dev/ttyS3) --(wire)--> HY310 UART
```

`ser2net` config example (`/etc/ser2net.yaml` on the bridge board):

```yaml
connection: &serial9999
    accepter: tcp,9999
    enable: on
    options:
        kickolduser: true
    connector: serialdev,/dev/ttyS3,115200n81,local
```

Then from any machine on the LAN:

```python
import socket, time
s = socket.socket()
s.connect(("<bridge-ip>", 9999))
s.sendall(b"uname -a\r")
time.sleep(1)
print(s.recv(4096).decode())
s.close()
```

This is **entirely optional** — for most users a direct USB-UART cable
into the dev machine is simpler. The `tools/uboot_interrupt.py` script
expects a TCP bridge by default (`192.168.8.179:9999`), but you can
fork it to use a local serial port if you don't have a bridge.

## Interrupting U-Boot

> **Important**: stock U-Boot has `bootdelay=0` — **no interrupt window**
> by default. First patch the env to set `bootdelay=5` using
> [sunxi-env-patcher](https://github.com/well0nez/sunxi-env-patcher).
> Without that, U-Boot interrupt methods won't work.

After patching, U-Boot waits 5 seconds — press any key during that
window.

### Automated (via TCP bridge)

```sh
python3 tools/uboot_interrupt.py
# spam keypresses through the bridge, drop to U-Boot prompt

python3 tools/uboot_interrupt.py --boot-usb
# interrupt AND issue USB boot commands
```

If you use a direct USB-UART cable instead of a bridge, you'll need to
adapt the script (or just spam Enter manually in your terminal during
the 5-second window).

### Manual (any terminal)

1. Open serial terminal at 115200 8N1.
2. Power on the HY310.
3. When you see "Hit any key to stop autoboot:", spam any key.
4. You get the `=>` U-Boot prompt.

## Useful U-Boot commands

```
# Boot from USB stick (rescue):
usb start
fatload usb 0:1 0x45000000 mboot32.00
fatload usb 0:1 0x45400000 mboot32.01
bootm 0x45000000

# Boot stock Android from boot_b (failsafe):
sunxi_flash read 45000000 boot_b
bootm 45000000

# Check environment:
printenv bootcmd

# Read eMMC partition:
mmc dev 0
mmc read 0x45000000 0x32400 0x1
md.b 0x45000000 0x100
```

## Boot log reference

A normal successful boot:

```
U-Boot SPL ...
U-Boot 2018.05 (Allwinner H713) ...
Hit any key to stop autoboot: 5 4 3 2 1 0
...
Starting kernel ...
[    0.000000] Booting Linux on physical CPU 0x0
[    0.000000] Linux version 6.16.7 ...
[    1.234567] sun50i-h713-pinctrl 2000000.pinctrl: initialized sunXi PIO driver
...
```

## Diagnosing boot failures

| Pattern | Likely cause |
|---|---|
| "data abort" after "Starting kernel" | boot image format problem (wrong repack?) |
| Kernel panic + stack trace | driver bug or config issue. Auto-reboots in 5 s (`panic=5`) |
| Hangs after "Starting kernel" with no output | wrong DTB or early init crash |

UART output is the primary diagnostic when SSH isn't reachable.
