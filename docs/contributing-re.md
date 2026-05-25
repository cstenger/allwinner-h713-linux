# Contributing — RE workflow

If you want to help reverse-engineer the H713, this is your starting page.

## What's actionable right now

In rough priority order:

1. **`sunxi_ge2d` activation** — biggest single fix. The mainline port is in
   `drivers/display/ge2d/`. Currently blacklisted because of a DT-binding
   conflict with `h713_drm`. Swap which one is active, deal with the DRM/KMS
   dependency for Wayland separately. See
   [docs/subsystems/display.md](subsystems/display.md).

2. **HDMI-RX picture quality** — the LVDS path produces 4×1 grayscale
   tiling. Even after `sunxi_ge2d` is active, the channel format / scanout
   chain needs more work. See
   [docs/re/ge2d-port-notes.md](re/ge2d-port-notes.md) for what we know
   about the stock pipeline.

3. **HPD plug/unplug detection** — IRQ 266 never fires. Stock uses ARISC
   writes to `0x07091014`. We need either a polling watcher or to find the
   real interrupt source.

4. **TVCAP / INCAP frame-capture driver** — an out-of-tree skeleton exists
   at `sunxi_tvcap_rx.ko` but hasn't been reviewed in months. Needs a
   review pass against the current MIPS state.

5. **Wakeupgen porting** — stock uses `wakeupgen` as an IRQ-controller
   layer that routes Msgbox SPI 108/109 to ARM. Without it we have the
   pulse-doorbell workaround and 40-60% reliability. Porting it would fix
   the IPC reliability properly.

If you pick something, open an issue first so we don't duplicate work.

## Tools

### Host side

- **Cross-compiler**: `gcc-arm-linux-gnueabi` (Debian package).
- **IDA Pro** — main RE tool. Workspace is at `/opt/hy310/stock-re/` with
  `display.bin.i64`, `cpu_comm_dev.ko.i64`, `ge2d_dev.ko.i64`,
  `libhalhdmi.so.i64`, `libvideo.so.i64`. All renames + comments
  accumulated over 50+ sessions.
- **capstone-elftools** (Python) — fast disassembly without IDA when
  you just need to look at one function. See examples in
  [docs/re/](re/).
- **Ghidra** — works as a free alternative to IDA, especially for MIPS
  (set ABI to o32 LE, base to `0x8B100000` for `display.bin`).

### On-device side

- **`hreg <addr> <count>`** — generic MMIO reader for any 4-byte-aligned
  address. Statically linked ARM EABI5. Built from `tools/hreg.c`. Works
  via `/dev/hidtvreg` on stock Android, or `/dev/mem` on mainline.
- **`hdump <addr> <size>`** — byte-level dumper for DRAM regions. Same
  underlying mmap. Reads display.bin firmware, MIPS shared memory,
  anything.
- **`regtrace`** — periodic register-diff tool. Set a range, polling
  interval, log changes. Best way to see "what is changing" during a
  source-switch event.
- **`mips_elog_dump.py`** — pulls MIPS firmware logs via mmap. Ring-buffer
  mode (mode 1, ~120 KB) and linear mode (mode 2, 2 MB). `display_cfg.xml`
  on `mmcblk0p1` selects the mode.
- **`test_brightness_call`**, **`test_mips_call.py`**, **`test_all_routines`**
  — IPC tests, deployed to `/usr/local/bin/`.

## Workflow patterns

### 1. Boot stock Android and live-compare

Stock is on `mmcblk0p6` (failsafe — never touched). Flash `boot_a` from
stock by:

```sh
ssh hy310 'dd if=/dev/mmcblk0p6 of=/dev/mmcblk0p5 bs=4M conv=fsync'
# reboot
```

ADB + magisk root: `adb connect 192.168.8.228:5555`, then `adb shell su -c ...`.

On stock you have:
- `/dev/hidtvreg` — vendor `hidtvreg_dev.ko`, universal mmap to any phys
  address including DRAM.
- `hreg` + `hdump` deployed to `/data/local/tmp/`.
- `/proc/cpu_comm/RPC_calls` — live view of cpu_comm RPC traffic.
- Full Android tvserver running — the ground-truth IPC flow.

**WARNING**: do not `strace` stock `tvserver`. The ptrace slowdown breaks
the HDMI-RX timing. We learned this in session U.

### 2. Capture register diffs

```sh
# Stock state at moment of interest (e.g. after HDMI source-switch):
ssh stock 'hreg 0x05600000 0x100' > stock-afbd-active.txt

# Mainline state at the same moment:
ssh hy310 'python3 dump_mainline_regs.py' > mainline-afbd.txt

# Diff:
diff stock-afbd-active.txt mainline-afbd.txt
```

This is how we found the EE source fixes (TVTOP, LVDS corrections,
VBlender wrong-writes, OSD_FB_ADDR, NRWinNode). 32 register differences
identified in one capture pair.

### 3. capstone for quick function disassembly

Example: disassemble a function in `display.bin` without loading IDA:

```python
import capstone
from elftools.elf.elffile import ELFFile

# For a .ko module:
elf = ELFFile(open("/opt/hy310/stock-re/modules/decd.ko", "rb"))
text = elf.get_section_by_name(".text")
text_data = text.data()
text_addr = text["sh_addr"]

# ARM mode:
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
# MIPS LE for display.bin (raw binary, not ELF):
# md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)

# disassemble a function:
addr = 0x29b4  # dec_sync_frame_to_hardware
size = 596
code = text_data[addr - text_addr : addr - text_addr + size]
for ins in md.disasm(code, addr):
    print(f"0x{ins.address:x}: {ins.mnemonic}\t{ins.op_str}")
```

For finding callers / xrefs of a constant, scan all binaries with
ELF .text byte-pattern search. See examples in
[docs/re/cpu-comm-protocol.md](re/cpu-comm-protocol.md).

### 4. display.bin patches

MIPS firmware can be patched in-place. The current patch set:

```
File offset 0x002C4 (24 B): counter-cave trampoline + counter @ 0xAE340000
File offset 0x2156C: j 0x8B1002C4 (jump to counter)
File offset 0x1A52C: nop (spam-kill PLF-CPU-ready)
File offset 0x238D8: nop (ShStartAddr spam)
File offset 0x600 (64 B): sub_8B17FD10 entry hook (RETURN-write counter)
File offset 0x7FD10: j 0x8B100600 (jump to entry hook)
```

Stock MD5: `0d2191ca0dad3c17cd7db6ffa47217f5`. Always revert before
distributing builds. Patches survive reboots (they're in
`/1/mips/display.bin` on `mmcblk0p1`).

### 5. Kernel module build with dependent symbols

```sh
KBUILD_EXTRA_SYMBOLS=$PWD/drivers/tvtop/Module.symvers \
make -C $KDIR M=$PWD/drivers/decd \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules
```

**Don't** append the dependency's `Module.symvers` to the kernel's
`Module.symvers`. That pollutes the kernel symbol table.

### 6. Deploy and test cycle

```sh
# Edit source locally on Windows.
# Push to build server (openclaw):
scp drivers/display/drm/h713_drm.c openclaw:/opt/hy310/repo/drivers/display/drm/

# Build:
ssh openclaw 'cd /opt/hy310/kernels/working && \
  KDIR=$PWD bash /opt/hy310/repo/scripts/build_kernel_arm32.sh'

# Repack — MANDATORY (skipping this bricks the device):
ssh openclaw 'python3 /opt/hy310/repo/scripts/repack_boot.py \
  --outdir /opt/hy310/repo/build/output_arm32 \
  --zimage ... --dtb ... --cmdline "..."'

# Module-only deploy (for cpu_comm_*.c, decd_*.c, h713_drm.c changes):
scp openclaw:/opt/hy310/.../h713_drm.ko hy310:/lib/modules/6.16.7/.../

# Full kernel deploy:
scp openclaw:/opt/hy310/repo/build/output_arm32/hy310-mainline-arm32-boot.img hy310:/tmp/
ssh hy310 'dd if=/tmp/...img of=/dev/mmcblk0p5 bs=512k conv=fsync && sync'

# Power-cycle physically. NOT ssh-reboot — that can kill MIPS, then a power-cycle
# is required anyway.
```

### 7. MIPS firmware deploy

Patches need a physical power-cycle (display.bin is only loaded by U-Boot
at boot):

```sh
ssh openclaw 'python3 patch_display_bin.py'  # apply patches
scp openclaw:/tmp/display.bin.current hy310:/tmp/
ssh hy310 'mount /dev/mmcblk0p1 /1 2>/dev/null; \
           cp /tmp/display.bin.current /1/mips/display.bin && sync'
# Now power-cycle.
```

## Rules of thumb

These are scars from 50+ sessions:

- **No blind testing.** Read the source / disasm before patching a
  register. We have a "I'll just try this and see" rule in
  [SESSIONS.md](../SESSIONS.md) discarded hypotheses — it costs 5+
  sessions.
- **Read [SESSIONS.md](../SESSIONS.md) discarded hypotheses before
  proposing a new theory.** If your idea is on that list, it's already
  been falsified.
- **Stock register diffs are the gold standard.** Diff stock-android live
  state against mainline state at the same moment to find what's missing.
- **DMB ISHST + spin-wait** for ARM↔MIPS cache sync on shared-mem flag
  writes. Pattern is in `cpu_comm_handle_CPU2_*` functions (since session W).
- **Never write PB5 = LOW**. Fan and backlight share the line; LOW kills
  the fan and the board overheats.
- **Mari power-cycles physically when MIPS dies.** SSH reboot may not
  recover MIPS from a stuck WAIT-ACK state. If `cat /sys/class/hy300/mips/state_machine`
  returns nothing after a software reboot, that's the indicator.

## Reference: pre-refactor archived source

`drivers/Archived/` holds **read-only reference copies** of `cpu_comm`,
`tvtop`, `decd`, plus older monolithic `sunxi-mipsloader.c` /
`sunxi-nsi.c` from before the 2026-04-10 refactor. They're not compiled,
not linked into any Makefile — they're there for "what did the old
implementation do at this exact register?" lookups. See
`drivers/Archived/README.md` for the mapping to which old patches they
came from.

If you're tracing back an implementation decision in the RE notes, the
archived versions are usually what the older session was working with.

## Setting up the build environment

If you're new, this is the minimum to get a build:

```sh
# Cross-compiler:
sudo apt install gcc-arm-linux-gnueabi flex bison libssl-dev bc

# Mainline kernel (one-time, ~250 MB tarball):
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.16.7.tar.xz
tar xf linux-6.16.7.tar.xz

# Apply patches:
cd linux-6.16.7
for p in $(cat /path/to/hy310-linux/patches/series); do
  patch -p1 < /path/to/hy310-linux/patches/$p
done

# Build:
export KDIR=$PWD
cd /path/to/hy310-linux
./scripts/build_kernel_arm32.sh
```

See [BUILDING.md](../BUILDING.md) for the full build pipeline.

## Where to find help

- **[SESSIONS.md](../SESSIONS.md)** — what we already tried, what failed.
- **[docs/known-issues.md](known-issues.md)** — open bugs.
- **[docs/re/](re/)** — deeper RE notes on specific subsystems.
- **GitHub issues** — open a question, even if it feels dumb. The project
  scope is huge and someone else probably has context you don't.

## What this project needs most

In short:

- **A wakeupgen port** would fix half the IPC reliability issues.
- **`sunxi_ge2d` activation experiment** would unblock the LVDS picture
  path.
- **Help with the `DMA_CONFIG10/11` EDID write puzzle** — it works but we
  don't know why. Someone with Synopsys DW-HDMI-RX experience could
  probably untangle that in an afternoon.
- **An H713 vendor SDK or BSP**, if anyone has one. Vendor docs would
  short-circuit weeks of RE.
