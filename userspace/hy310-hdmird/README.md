# hy310-hdmird — HDMI-RX bringup daemon

Replicates stock-android tvserver init sequence to bring HDMI-RX state
machine into Running state on H713 mainline.

## Background

Mainline kernel-driver `sun50i-h713-hdmi-rx.c` writes directly to Synopsys
DW-HDMI-RX registers (`0x050C0000`) and the H713 wrapper (`0x06840000`).
**This is wrong.** Stock-android kernel has no HDMI symbols at all — all
HDMI bringup is driven from userspace via `cpu_comm` IPC to MIPS.

Stock-android RPC trace (captured 2026-05-01) shows tvserver makes 12 calls
during boot and 12 more during HDMI source-switch. The MIPS-side state
machine reaches Running state only after this sequence completes.

## Architecture

- **`hy310-hdmird`** — daemon, runs at boot. Maps `/dev/cpu_comm` shared
  memory, loads `hdcp_v22.bin`, executes init sequence, listens on
  `/run/hy310-hdmird.sock` for source-switch commands.
- **`hy310-hdmi`** — CLI control tool. Sends commands over the socket.
- **`libhy310cpucomm`** (in-tree, built statically) — shared with hy310-pqd.

## Init sequence (replicates stock sessionID 14-33)

1. Picture pipeline defaults: BacklightLevel(100), TNR(2), SNR(1), DCI(2),
   BlackExtension(1), PictureMode(1), VideoRange(0)
2. CvbsSetPedestalMode(1)
3. **3× HDMI_SetPortMap(3, 4, 5)** — port-init (mechanism unclear; stock
   replicates with 3 sequential calls)
4. DisableBlackScreen, TurnOnARCAudioPath, SwitchARCTXPath
5. **SetHDCP22Key(phys-addr-of-hdcp_v22.bin)** — uses cpu_comm shared
   memory mechanism
6. **SetHPDTimeInterval(0xC8)** — 200ms

## Source-switch sequence (sessionID 34-45)

`SetSource(N)` + reapply picture-quality defaults. MIPS responds with
`MipsHalCallback_SignalChange(Para=3)` when TMDS lock achieves.

## Known unknowns

- **SetPortMap with 0x3, 0x4, 0x5** — stock RPC log shows only first arg.
  Memory says routine takes (old, new). We try single-arg first; if MIPS
  rejects, expand to old/new pairs.
- **WhiteBalance + Wce_SetWindow pointer args** — stock passes shared-mem
  pointers to factory-data tables. We don't have the data; calls are
  skipped. May not be load-bearing for TMDS-lock but could affect picture
  quality.
- **MipsHalCallback registration** — defensively registered in case MIPS
  state-advance depends on registered listener. Handlers are no-op
  (cpu_comm fires-and-forgets per stock RPC stats: 4 MIPS→ARM CALLs,
  0 ARM→MIPS RETURNs).

## Build

```
cd userspace/hy310-hdmird
make                     # cross-compile arm32
make deploy              # scp to hy310 (mainline boot)
```

Cross-toolchain: `arm-linux-gnueabihf-g++` (Debian/Ubuntu pkg
`g++-arm-linux-gnueabihf`).

## Test plan

1. Flash mainline boot.img back (after stock RE done)
2. `make deploy` from openclaw
3. On hy310:
   ```
   install -m 0755 /tmp/hy310-hdmird /usr/local/sbin/
   install -m 0755 /tmp/hy310-hdmi   /usr/local/bin/
   install -m 0644 /tmp/hdcp_v22.bin /lib/firmware/
   install -m 0644 /tmp/hy310-hdmird.service /etc/systemd/system/
   systemctl daemon-reload
   systemctl start hy310-hdmird
   journalctl -fu hy310-hdmird
   ```
4. Connect HDMI laptop
5. `hy310-hdmi src 3` (HDMI source select)
6. Check `byte@0x06840001 bit 5` and `CMU_STATUS LOCK` via `/dev/mem`

Success criteria: `MipsHalCallback_SignalChange Para=3` fires, byte@1 bit 5
goes 1, CMU_STATUS LOCK asserts, MIPS state-machine reaches Running.

## Iteration plan if first try doesn't lock

A. **Add SetPortMap (old, new) variants** — try (3,3), (4,4), (5,5).
B. **Add WhiteBalance/Wce_SetWindow with all-zero buffers** — maybe MIPS
   needs *any* pointer.
C. **Reorder init sequence** — strict-match stock sessionID order.
D. **Strace tvserver** — NO, killed timing in test. Use ftrace instead.
E. **Compare** `cat /proc/cpu_comm/share_memory` mainline vs stock.

## License

GPL-2.0-or-later (matches Linux kernel + cpu_comm driver).
