# hy310-pqd — Picture Quality Daemon

Native Linux daemon that manages Picture Quality (PQ) settings on the
HY310 projector (Allwinner H713, ARM32).  Replaces the stock Android
`/vendor/bin/hw/tvserver` + `libtvpq.so` stack with a clean,
systemd-integrated service that works on mainline Linux.

## What it does

- Reads stock PQ configuration files (`tvpq.db`, `pqcontrol_custom_setting.xml`,
  `pq_colortemp.ini`) from `/etc/hy310/tvconfig/`
- Opens `/dev/cpu_comm` for IPC with the MIPS co-processor (FusionDale RPC)
- Opens `/dev/mem` to write gamma LUTs directly into the DE2 display
  controller registers
- Installs 34 ARM-side routine stubs so calls to MIPS-side
  `THal_Vp_*` functions resolve via FindRoutine
- Applies initial PQ state from config on startup
- Exposes runtime control via a UNIX socket (`/run/hy310-pqd.sock`)
  consumed by `hy310-pqctl`

See [BACKGROUND.md](BACKGROUND.md) for the reverse-engineering story and
architectural rationale.

## Building

The project cross-compiles from openclaw (Debian x86_64) to hy310 (armv7l
armhf).  Dependencies: `g++-arm-linux-gnueabihf`, `libsqlite3-dev:armhf`.

```bash
# Cross-compile (default)
cd userspace/hy310-pqd
make

# Native build on hy310 (requires g++, libsqlite3-dev)
make NATIVE=1

# Run the interpolation unit tests natively on build host
make NATIVE=1 test
```

Build output:

```
build/libhy310pqd.a     static library (transport + config + gamma + service)
build/hy310-pqd         daemon binary  (~185 KB)
build/hy310-pqctl       CLI tool        (~71 KB)
build/test_name2id      CRC32 Name2ID unit test (matches stock hashes)
build/test_gamma_interp gamma curve interpolation unit tests
build/test_gamma_mmio   live gamma MMIO write (needs /dev/mem + active DE2)
build/test_mips_call    end-to-end MIPS roundtrip test
build/test_mmio_smoke   /dev/mem writability probe
build/test_pqconfig     config-parser test against stock files
```

## Deployment

```bash
# From openclaw
cd userspace/hy310-pqd
make deploy    # scp's binaries to hy310:/tmp/

# On hy310 as root
install -m 0755 /tmp/hy310-pqd   /usr/sbin/
install -m 0755 /tmp/hy310-pqctl /usr/bin/
install -m 0644 /tmp/hy310-pqd.service /etc/systemd/system/
systemctl daemon-reload
# (optional) systemctl enable hy310-pqd   # autostart at boot

# Stock config files are read from:
mkdir -p /etc/hy310/tvconfig
# copy pq_factory_extern.ini, pq_colortemp.ini, pqcontrol_custom_setting.xml,
# tvpq.db, pq_picturemode.ini from stock /vendor/etc/tvconfig/
```

## Usage

One-shot apply (load config, write hardware, exit):

```bash
hy310-pqd --oneshot -v
```

Run as service (socket listens on `/run/hy310-pqd.sock`):

```bash
hy310-pqd --daemon -v
# in another terminal / from a UI
hy310-pqctl dump
hy310-pqctl set gamma 3
hy310-pqctl set brightness 55
hy310-pqctl apply
```

Available CLI keys: `brightness`, `contrast`, `saturation`, `hue`,
`sharpness`, `backlight`, `colortemperature`, `gamma`, `tnr`, `snr`,
`dci`, `blackextension`, `picture_mode`.

## Daemon flags

```
--config DIR       config dir (default /etc/hy310/tvconfig)
--socket PATH      UNIX socket path (default /run/hy310-pqd.sock)
--no-apply         skip initial apply-to-HW step
--no-stubs         skip routine-stub installation
--daemon           run forever (socket loop)
--oneshot          apply and exit (default)
-v / -q            more / less verbose
```

## Current feature matrix

| Feature                      | Path                       | Status       |
| ---------------------------- | -------------------------- | ------------ |
| Gamma curve                  | direct MMIO to DE2         | ✅ working   |
| Brightness / contrast / …    | MIPS IPC via /dev/cpu_comm | ⏳ blocked   |
| Colour temperature           | MIPS IPC                   | ⏳ blocked   |
| Black Extension / DCI / NR   | MIPS IPC                   | ⏳ blocked   |
| Picture modes (Standard…HDR) | config + above             | ⏳ blocked   |

⏳ MIPS IPC paths compile, wire correctly, and produce round-trip
CALL_ACKs in 34 ms — but MIPS rejects them silently because its
`hal_adapter_init` aborts before registering the PQ routines.  Unblocking
this requires providing gamma LUT data at the KSEG0 addresses MIPS
reads before it boots.  See BACKGROUND.md §"MIPS blocker".

## Repo layout

```
include/
├── cpucomm.h       # /dev/cpu_comm transport (FusionDale RPC)
├── pqgamma.h       # gamma curve gen + DE2 MMIO writer
├── pqconfig.h      # stock config loader (tvpq.db, XML, INI)
└── pqservice.h     # orchestrator: combines everything

src/
├── name2id.cpp     # CRC32 seed=0x123456 routine name hash
├── cpucomm.cpp     # ioctl wrapper + install + call
├── pqgamma.cpp     # interpolate 33 points → 1024 LUT → MMIO
├── pqconfig.cpp    # SQLite + regex XML + INI parser
├── pqservice.cpp   # ties everything together + socket server
├── main.cpp        # hy310-pqd daemon entrypoint
└── pqctl.cpp       # hy310-pqctl CLI client

tests/
├── test_name2id.cpp       # unit: CRC matches stock hashes
├── test_gamma_interp.cpp  # unit: 33→1024 interpolation correctness
├── test_gamma_mmio.cpp    # hw:   live gamma write to display
├── test_mmio_smoke.cpp    # hw:   which regs are writable
├── test_mips_call.cpp     # hw:   end-to-end MIPS call roundtrip
└── test_pqconfig.cpp      # data: parse real stock config files

etc/
└── hy310-pqd.service      # systemd unit

Makefile
README.md                  # this file
BACKGROUND.md              # why / how / RE story
```

## Caveats

- **`/dev/mem` required** for gamma.  Daemon runs as root for now.
  A future kernel driver could expose a safer `/dev/hy310-pqmmio`.
- **DE2 must be active** (a DRM client must be rendering) for gamma
  writes to produce visible effects.  On our mainline stack this means
  `h713_drm.ko` is loaded **and** a compositor (e.g. labwc) is driving a
  scanout buffer.  The daemon has no way to detect this — if the
  screen doesn't react, check `ls /dev/dri/` and that something is
  consuming it.
- **MIPS IPC FIFO leak** when calling routines that don't exist on
  MIPS.  10 leaked slots per apply_all; after ~2 applies the driver
  returns `-EBUSY`.  Current mitigation is to avoid calling apply_all
  repeatedly; root fix is on our todo list (see BACKGROUND.md).

## License

The project sources are under the same MIT-style licence implied by
their clean-room nature.  The `include/` headers are derived solely
from observed behaviour and Stock binary analysis — no Stock source
code is vendored.  Stock configuration *files* (`tvpq.db`, INIs, XML)
are vendor data installed separately by the user under `/etc/hy310/`.
