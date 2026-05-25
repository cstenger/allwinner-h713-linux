# Userspace daemons

HY310-specific userspace services that talk to the MIPS co-processor over
the `cpu_comm` IPC driver (`/dev/cpu_comm`). They are not part of the kernel
build — build and install them separately on the target rootfs.

| Daemon | Purpose |
|---|---|
| [`hy310-hdmird`](hy310-hdmird/) | HDMI-RX bring-up daemon: programs the EDID-RAM, loads the HDCP 2.2 key, runs the post-signal cpu_comm call sequence on HPD, drives the scanout path. |
| [`hy310-pqd`](hy310-pqd/) | Picture-quality daemon: gamma/bezier/white-balance computation and the `THal_Vp_*` MIPS calls (factory gamma load from `tvfactorypq.db`). |

Both share a small `cpucomm` helper (`src/cpucomm.cpp`) — the userspace side
of the IPC `IOCTL_CALL` packet format documented in
[`docs/re/cpu-comm-protocol.md`](../docs/re/cpu-comm-protocol.md).

## Build

```sh
cd hy310-hdmird   # or hy310-pqd
make              # cross-compile per the Makefile toolchain vars
```

`hy310-pqd` additionally has host-native unit tests under `tests/`
(`make` targets build them into `build-native/`).

## HDCP key (hy310-hdmird only)

`hy310-hdmird` installs `/lib/firmware/hdcp_v22.bin`. That blob is the
licensed HDCP 2.2 device key extracted from the stock vendor firmware and is
**deliberately not in this repo**. Supply it yourself (the Makefile expects it
at the path in `HDCP22_KEY`) before running `make install`.
