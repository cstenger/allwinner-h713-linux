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

UART pins are on the HY310 main board. You need a **3.3 V** USB-UART
adapter — 5 V will fry the SoC.

Wiring:
- HY310 TX → adapter RX
- HY310 RX → adapter TX
- HY310 GND → adapter GND

## Linux / macOS

```sh
screen /dev/ttyUSB0 115200
# or
minicom -D /dev/ttyUSB0 -b 115200
```

macOS device names: `/dev/tty.usbserial-*`.

## Windows

PuTTY or Tera Term, select COM port, 115200 8N1.

## OrangePi as UART bridge (our dev setup)

In our development setup an OrangePi acts as a network-to-UART bridge,
allowing UART access from any machine on the LAN:

```
Windows/Linux PC --TCP:9999--> OrangePi (/dev/ttyS3) --wire--> HY310 UART
```

Python access:

```python
import socket, time
s = socket.socket()
s.connect(("192.168.8.179", 9999))
s.sendall(b"uname -a\r")
time.sleep(1)
print(s.recv(4096).decode())
s.close()
```

## Interrupting U-Boot

> **Important**: stock U-Boot has `bootdelay=0` — **no interrupt window**
> by default. First patch the env to set `bootdelay=5` using
> [sunxi-env-patcher](https://github.com/well0nez/sunxi-env-patcher).
> Without that, U-Boot interrupt methods won't work.

After patching, U-Boot waits 5 seconds — press any key during that
window.

### Automated

```sh
python3 tools/uboot_interrupt.py
# Connects to UART bridge, sends rapid keypresses, drops to U-Boot prompt

python3 tools/uboot_interrupt.py --boot-usb
# Interrupts AND issues USB boot commands
```

### Manual

1. Open serial terminal.
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
