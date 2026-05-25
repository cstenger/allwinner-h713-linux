# Reverse-Engineering Timeline

How the HY310 mainline-Linux port came together, in chronological order. This is
the long form of [STATUS.md](STATUS.md) — read STATUS first if you just want to
know what currently works.

Each entry is a milestone, not a blow-by-blow. Dead ends and false hypotheses
are listed in [Discarded hypotheses](#discarded-hypotheses) at the bottom — they
matter because re-trying them is expensive.

---

## Pre-port baseline (early 2026)

- Hardware identified: Allwinner H713 SoC, three CPUs (ARM Cortex-A53 + MIPS32
  LE coprocessor for display/PQ + OR1K BE ARISC SCP for PMU/HPD), DLP projector
  output via LVDS, HDMI-RX input via Synopsys DW-HDMI-RX block.
- Stock firmware is Android with vendor `wakeupgen` IRQ-controller, vendor
  `cpu_comm_dev.ko` for ARM↔MIPS IPC, `hidtvreg_dev.ko` for arbitrary MMIO
  access, and a tvserver that drives the entire display pipeline via cpu_comm.
- Mainline kernel 6.16.7 boots ARM cleanly. Most peripherals work (USB, eMMC,
  Wi-Fi, BT). Display + HDMI-RX + audio do not.

---

## IPC bringup — sessions A–X (2026-04-16 → 2026-05-04)

### What was figured out

**Hardware**

- Three msgbox user regions at `0x03003000`, each 0x400 bytes, with sub-blocks
  per remote peer (sessions A/B).
- Write-protection map was reverse-engineered empirically — ARM can only write
  certain registers in certain user regions, others are HW-blocked.
- H713 msgbox is **edge-triggered**, not level-triggered like H6. Stock's
  pure-IRQ TX pattern fails on H713; a pulse-doorbell workaround was found
  (session O) with 40-60% reliability — flagged as workaround in
  `sunxi_msgbox_amp.c`.
- DTS amp-mapping: cpus=1 (ARISC), mips=2. Initially guessed inverted in
  session N, corrected in O.
- TX addresses fully mapped: ARM→MIPS CALL at `0x03003874`, MIPS→ARM at
  `0x03003174`, MIPS→ARISC HPD at `0x03003574`, ARM→ARISC at `0x0300347c`.

**MIPS firmware (`display.bin`, 1.25 MB FreeRTOS-style)**

- Loaded by U-Boot before kernel.
- Code/data lives at MIPS-VA `0x8B100000` = ARM-phys `0x4B100000`.
- Shared memory at MIPS-VA `0xAE300000` = ARM-phys `0x4E300000` (5 MB).
- Critical functions located and named in IDA: msgbox IRQ handler at
  `0x8B12156C`, cpu_comm dispatcher at `0x8B11EF78`, HISR worker at
  `0x8B1219A4`, FIFO management at `0x8B11D418`/`0x8B118B84`, name-hash
  generator at `0x8B1247EC`.
- The MIPS firmware uses ThreadX queues + HISR-worker scheduling, with HW
  spinlocks at `0x03004000`.
- 29 MIPS functions reverse-engineered + named (session H), enabling all the
  later patches.

**cpu_comm protocol**

- 168-byte CALL packet. Critical field offsets: `dst_cpu` at +2, `comp_id`
  at +40, `params[0]=count` at +64 (caller MUST set count, otherwise the
  packet is silently dropped — session Q bug).
- Name hash is Allwinner's CRC32 variant with seed `0x123456`. Names are
  formatted as `<base>_<cpu_id>_<pid_low12>`.
- 81-85 routines registered by the MIPS firmware on boot, full table of
  comp_ids documented in `docs/re/cpu-comm-protocol.md`.

### Major bug fixes during bringup

| Session | Fix |
|---|---|
| H | `INTR_TYPE_SEND=2` was wrong, MIPS interpreted it as CALL_ACK. Fix: use `MSG_TYPE_CALL=0` for forward CALL. |
| I | HISR-worker waiter-tracking offset was `queue+36`, not what the initial guess had. |
| J | Lock-2 init bug — `comm_SpinLocksetType(2,1)` was wrong; removing it let all 81 routines register cleanly in 34 ms. |
| K | First full IPC roundtrip: `THal_Vp_SetBrightness` ENTER/LEAVE on MIPS + RETURN_ACK on ARM. 8 sub-fixes (RX port +0x100, bit-layout parsing, ack/command_action bit-2 check inverted, MIPS-flag handshake). |
| K-night | FreeCall FIFO overflow solved by removing a cache-hack and using fresh `Comm_GetFreeCall` per send. Multi-call now stable. |
| Q | cpu_comm packet-format bug — `params[0]=count` at offset 64 was being silently no-op'd because the write happened at the wrong offset. |

### Address translation

Stock `cpu_comm_dev.ko` has translation helpers (`Vir2Mid`, `Mid2Vir`,
`Mid2Phy`, `Phy2Mid`, `Trid_SMM_transAddr`) for converting between three
address spaces:

- ARM kernel virtual (vmap of shmem)
- ARM physical (for DMA)
- MIPS virtual (`0xAE3xxxxx` KSEG1 uncached for shmem, `0x8B1xxxxx` for code)

Mainline `cpu_comm_dev.c` has those translations now (session W). Without them,
`returnPipeLine` couldn't find entries because seq_idx pointed to the wrong
slot.

### Stock-Android live comparison (session U)

ADB+magisk root on stock-Android (booted from mmcblk0p6, our failsafe).
`/proc/cpu_comm/RPC_calls` exposed the full init + source-switch sequence —
about 33 cpu_comm calls. This became the template for `hy310-hdmird`.

Stock kernel has **zero HDMI-related symbols** — all HDMI logic lives in MIPS
firmware + tvserver userspace daemon. That settled the architecture: we need
to port tvserver's logic into a Linux userspace daemon, not into a kernel
driver.

### hy310-hdmird daemon

Sessions V-W. C++ ARM32 cross-compiled daemon, source in the repo at
`userspace/hy310-hdmird/`. Replicates the stock tvserver init + source-switch:

1. Open `/dev/cpu_comm`, mmap 5 MB shmem at phys `0x4E300000`.
2. Load `hdcp_v22.bin` (960 B from stock `vendor/etc/firmware/`) into shmem at
   offset `0x36000`.
3. Register 9× `MipsHalCallback_*` routines for ARM-side callbacks (signal
   change, HPD, audio/video info packets).
4. Run init sequence (12 calls: `Vp_Init`, `RegisterSignalChangeCallback`,
   `SetHotPlug`, picture defaults, port-map, HDCP22, HPD timing).
5. Run source-switch (12 calls: `SetSource` + reapply picture defaults).
6. Listen on `/run/hy310-hdmird.sock` for CLI commands.

Initial workaround: 500 ms inter-call throttle because the FreeCall pool
drained after ~20 calls. Later replaced by a kernel-side callback receiver
(session DD).

### EDID + HPD (session P)

EDID write goes through the Synopsys block at `0x050C0000` via
`DMA_CONFIG10/11` registers. Pattern: enable WRITE_EN + slave address 0x50,
byte-stream to DMA_CONFIG10. Readback shows zeros (mechanically not
understood — see [open issues](#open-issues)) but laptops read
`SGD SX8 1920x1080p@60` correctly.

HPD pin is at `0x07091014`, 3 bits (port 1=bit 1). Undocumented register,
reverse-engineered from ARISC firmware. Sequence: write `0x00` early, write
`0x07` after EDID is loaded. The LOW→HIGH edge forces the source to re-read
EDID.

ARISC firmware at 172 KB OR1K BE was extracted from `bootloader_a.bin` TOC1
item "scp" at offset `0xb0c00` (word-byte-reversed). Two dispatchers found:
`0xbc9c` handles PMU types {25, 34, 36–38, 96–100}, and `0x12490` is the
SCPI/HDMI dispatcher with EDID/HPD handlers. All `0x12490` responses are
hollow (status -3 "imt error") — that path is no-op by design.

### State machines

**HDMI-RX (MIPS-side)**: 5 states. 1=Initial, 2=Sleep (asserts HDCP+DDC
reset), 3=Idle (releases HDCP+DDC, asserts TMDS reset, AEC enable),
4=TransitionUp, 5=Running (releases TMDS reset, FreqDetect runs).

The state 3→4 transition needs `CheckSignalReady() == true`, which checks
`byte@0x06840001 bit 5 == 1 AND byte@0x068401AB bit 2 == 1`. These bits are
**zero in both stock and mainline** (session W proved this via
`/dev/hidtvreg`) — so they are NOT the real gate. Stock somehow reaches
state 5 without setting them. The real gate remained unknown until
session Z.

### Session-X: the silicon-level dead end

Counter-cave hooked into `sub_8B17FD10` (MIPS MMIO write helper). Counts:
total 495 611 writes during a full hdmird run, but **zero writes had
value=1** — meaning MIPS's `BG_Thread` never sent `type=1` (RETURN) to ARM,
despite elog showing `DoIntr2CPU2 End`.

Conclusion: MIPS `BG_Thread` blocked in `WAIT ACK completion` forever, the
FreeCall pool drained after 19-21 calls, and the state machine stayed at
state 3.

---

## Vp_Init breakthrough — session Z (2026-05-05)

The Y session (2026-05-04) spent the day trying to fix `Vp_Init` crashes that
caused kernel-MM corruption (`*pte=0x8baa0000` in unrelated processes).

Z found the root cause: `Vp_Init` was being called with `{0, 0, 0}` parameters,
but MIPS-handler `sub_8B109F04` uses `Para[2]` as a phys-addr pointer for a
55 KB factory-PQ memcpy. `Para[2]=0` → NULL memcpy → MMU fault → BG_Thread
breaks → kernel-MM corruption cascades.

**Fix**: ParaCount=3 with `Para[2] = SHMEM_PHYS_BASE + 4 MB = 0x4E700000` as
staging area.

**Result**:

- 33/33 hdmird calls clean.
- TMDS lock: `tmds=0x8d9f485`.
- MIPS state machine progresses 1→2→3→4→**5 (Running)**.
- 1080p60 signal detection: `signal_id = kHalSignalID_XGA19201080`,
  pixel frequency 148.5 MHz.
- Signal-change callbacks reach ARM (session 40000002/3).
- HPD detection + cmd 144/160/176 chain.

The IPC + signal-detect pipeline was complete after Z.

What still didn't work: **the picture wasn't reaching the projector**. The
display stack (`h713_drm` + `sunxi_decd` + `sunxi_tvtop`) held the scanout
pointer at u-boot's bootlogo buffer `0x78541000`, not at MIPS's output buffer.

---

## LVDS scanout — sessions AA-BB (2026-05-05/06)

### AA: register-dump comparison

Captured stock pre vs post HDMI-source-switch register dumps. About 30
registers in the `0x05600000` (AFBD) region change. Mainline writes only some
of them. Critical finding: scanout pointer `+0x320/+0x324` in stock cycles
through MIPS output buffers; in mainline it stays at u-boot bootlogo.

### BB: the actual breakthrough

Added module parameter `mips_scanout_addr` to `h713_drm`. When set to
`0x4c3ef000` (MIPS's first SMM allocation, deterministic), the AFBD scanout
pointer redirects from bootlogo to MIPS-output.

Combined with `blacklist sunxi_ge2d` + `labwc + hdmird --src 3`
(HDMI1, not 5), the laptop content **reaches the projector for the first
time**.

It looks wrong though: 4×3 horizontal tiling, black-and-white. The reason:
channel-1 is in XRGB mode (32 bpp), reading from MIPS's NV12 buffer (12 bpp)
with stride 7680 instead of 1920. The display interprets each row's bytes
incorrectly and tiles them 4× horizontally; the chroma plane is ignored, so
the picture is grayscale.

Three dead ends explored in BB evening:
- Forcing channel-1 NV12 mode: writes accepted but no effect.
- Channel-0 register access: hardware-protected.
- Replaying the stock register-write stream verbatim: no effect.

---

## Display pipeline investigation — sessions CC-EE (2026-05-06/07)

### CC: stock-android live trace + hdmird modifications

Booted stock-android, captured register traces via `regtrace` tool. Source-
switch event was captured deterministically. Plus deeper IDA RE of
`display.bin`: WCE pipeline has 5 stages, `PanelWinNode__WriteReg` confirms
that MIPS writes only DE2 (`0x05000000-0x051FFFFF`) and INCAP (`0x06940000`),
**not** TCON, LVDS-PHY, or panel-output. That means LVDS routing is 100%
ARM-side, not MIPS-side.

### DD: callback-loop infrastructure + AFBD stock-conform

Kernel patch in `cpu_comm_dev.c`: added `.read`/`.poll` file ops + per-fd
ringbuffer + `cpu_comm_userspace_deliver()`. Plus hdmird receiver-thread that
processes `SignalChange(state=3)` events and runs the post-signal sequence
(12 cpu_comm calls).

**Channel-id mismatch fix**: `IOCTL_INSTALL_RT` registered routines with
`(channel & 0xF) | (target_cpu << 4) = 0` when target_cpu=0, but MIPS sends
type=0 (CALL) with `channel_id = 1 | (PID << 4) = 0x26f1`. Added a second
channel registration with `comp_id=1` (= `MIPS_CPU_ID`) and
`cpu=current->pid` for ARM-impl routines.

After the patch: `SignalChange(state=3)` callbacks reach hdmird userspace
correctly. Stock-conform post-signal sequence triggers via the real callback
instead of the 3 s timeout fallback.

Also tested AFBD register state — every register matches stock byte-for-byte
after `+0x10/+0x14` trigger writes. Visual: **white**. The AFBD decompressor
accepts the configuration but doesn't produce content.

### DD-NIGHT: pool-1 hypothesis

Identified that `+0x70-0xA4` in the AFBD area is decd's "pool-1" — Y addresses
(0x70-0x7C), C addresses (0x84-0x90), info-page addresses (0x98-0xA4). Stock
fills these per-vsync with rotating MIPS-buffer addresses. Mainline leaves
them at zero.

Hypothesis: pool-1 fill is the missing piece. Decd module is loaded but no
userspace consumer opens `/dev/decd`, so pool-1 stays zero. Confirmed via
`/proc/interrupts`: decd IRQ hits=0 (later this changed once we engaged it).

### EE: five h713_drm source fixes

Reverse-engineered stock register diffs between `stock_extended_dump_HDMI.txt`
and `posttest2-final.txt`. Found 32 register differences. Wrote 5 source
fixes:

1. TVTOP routing: `+0x00 = 0xFFF11111` (was 0x00011111, upper 12 bits were
   missing), `+0x88 = 0x11111111` (was 0xFFFFFFFF, conservative).
2. LVDS PHY corrections: `+0x14 = 0x1A000005`, `+0x24 = 0x00350000`,
   `+0x28 = 0x08100035` — overrides u-boot defaults to stock-conform values.
3. VBlender wrong-writes removed: `+0x05200040/+0x050/+0x054` were being
   written by `h713_drm` with values that actually belonged to LVDS offsets
   — the original RE engineer had patched stock-RE values to the wrong base.
4. `OSD_FB_ADDR` write removed: stock has it at zero in HDMI-active state.
5. `NRWinNode bit 4` added in mode=4 path: `+0x05600060 |= 0x10`.

Visual after EE: black. Better than white (= white was wrong-writes
producing garbage; black is structurally closer to "correctly configured,
waiting for valid input") but still no picture.

---

## Pool-1 saga — autonomous capstone session (2026-05-07/08)

After EE the running theory was still "pool-1 fill is the missing piece" —
across DD, DD-NIGHT, EE, three different attempts.

### The decd port-bug

Built `decd_submit_test`, an 80-line userspace tool that opens `/dev/decd`,
issues `PM_HINT(1)` to bring up power, then `FRAME_SUBMIT` with the linear=1
path. The ioctl succeeds, fence-fd is returned, dmesg is clean.

**Pool-1 is fully populated** byte-for-byte stock-conform:

- Y addresses × 4 = `0x4c3ef000`
- C addresses × 4 = `0x4c5e9400` (= Y + 0x1FA400, NV12 chroma offset)
- info-page addresses × 4 = `0x4c3f0000`

But the info-page address `0x4c3f0000` is **inside the Y plane**. MIPS writes
1920×1080×1.5 = `0x1FA400` bytes starting at `0x4c3ef000`. The info-page is
at +4 KB inside that range. MIPS overwrites the info-page metadata with Y
pixel content.

Found the bug:

```c
u32 video_info_buffer_init(struct dec_frame_submit_desc *desc)
{
    return lower_32_bits(desc->y_phys) + 4096;
}
```

Hardcoded `y_phys + 4096` in the linear=1 path. Stock decd uses the dma-buf
path (`video_info_buffer_init_dmabuf`) which allocates from a reserved-mem
pool (`decd_reserved` at `0x4d941000 + 128 KB`).

**Fix**: change linear=1 path to use `alloc_video_info_page()` like the
dma-buf path does. Module rebuilt clean with `KBUILD_EXTRA_SYMBOLS` (Mari
pointed out the correct build pattern; earlier append-hack on
`Module.symvers` was sloppy).

After fix: info-page is at `0x4d941000` (from `decd_reserved`), no collision
with MIPS Y plane. Verified.

Visual: still black.

### Capstone-RE: stock decd magic constant

Disassembled stock `decd.ko` with capstone-elftools. `video_info_buffer_init_dmabuf`
checks the user-supplied buffer for magic constant before memcpy'ing it.
Mainline port has `0x61766b40` ("@kva"). Stock has `0x61770000` ("..wa") — the
mainline port had the wrong constant in its comment, which the porter then
hardcoded into the validation.

Wrote correct magic into our info-page via /dev/mem. Visual: still black.

### Pool-1 is not the missing piece

Five sessions had carried the "pool-1 fill is the missing piece" hypothesis.
With pool-1 byte-for-byte stock-conform, info-page in `decd_reserved`, magic
constant corrected, AFBD mode bits in `+0x100` toggled across multiple
permutations, and `+0x010` set stock-conform — visual was **constantly
black**.

The hypothesis is definitively falsified. Pool-1 fill changes the output
from "white (no input)" to "black (input present, decoder not rendering)",
which is a structural change but not the right one.

### ge2d_dev.ko is the real stock display driver

Searched all stock binaries for `DECD_IOC_FRAME_SUBMIT` (`0x40706400`)
immediate. Found only inside `decd.ko` itself. **No userspace consumer.**
Only `libmips.so` and `svp-suspend` open `/dev/decd`, and they use
`MAP_LINEAR_BUFFER` for memory mapping — never `FRAME_SUBMIT`.

Stock `decd.ko` is likely dead code for the HDMI-RX path. It was built for
video playback (decoded H.264 / MPEG2 frames from ffmpeg / OMX) with
AFBC-compressed metadata in the info-page, not for HDMI-RX scanout.

Found `vendor_modules/ge2d_dev.ko` instead. 36+ functions including
`tgd_flip_plane` (232 B page-flip routine), `tgd_vblender_irq` (1532 B vsync
IRQ handler), `init_osd_plane` (3472 B!), `tgd_put_plane_info` (11 064 B!),
`tgd_set_vinterpolation` (744 B vertical scaling/conversion), `lvds_reset_fifo`,
`svp_set_cmap`. Full tracepoint instrumentation
(frame/mux/hwreg/vsync/ge2d tracing). Depends on `vs_io_helper`,
`sunxi_tvtop`, `backlight`.

**ge2d_dev.ko is the real display driver for the HDMI-RX scanout path.**

### Mainline already has the port

`/opt/hy310/repo/drivers/display/ge2d/sunxi-ge2d.ko` — about 3000 lines
across 9 source files — is already a substantial port of `ge2d_dev.ko`:

- 16-step probe directly ported from `ge2d_drv_probe`
- `ge2d_vblender_hardirq` (port of `tgd_vblender_irq`)
- `ge2d_afbd_hardirq` (port of `osd_afbd_irq`)
- OSD plane init via delayed work + `LogoRegData.bin` firmware loader
  (1010-line parser for stock plane-init register streams)
- `/dev/fb0` (1920×1080 ARGB8888) + `/dev/ge2d` chardev
- LVDS watchdog thread
- Backlight + DLPC3435 helpers

Only blocker: DT-binding conflict. Both `h713_drm` and `sunxi_ge2d` claim
`compatible = "trix,ge2d"` for node `5240000.ge2d`. `sunxi_ge2d` was
blacklisted, leaving the wrong driver active.

That ends the autonomous capstone session. **Next session decision: swap
the blacklist — `h713_drm` out, `sunxi_ge2d` in — and test.**

---

## Current state (2026-05-25)

- IPC pipeline works end-to-end: ARM↔MIPS via cpu_comm, ARM↔ARISC via
  msgbox_amp/arisc_rpm, callback delivery to userspace via the `.read`/`.poll`
  cpu_comm patch.
- HDMI-RX TMDS lock works. MIPS reaches state 5 (Running). 1080p60 signal
  detected reliably.
- Picture **reaches the projector** via the BB-baseline path
  (`mips_scanout_addr` override + ch1 XRGB), but **tiled 4×1 grayscale**
  because of the format mismatch.
- HDMI ausgabe (output, separate from input): works.
- eMMC: downgraded from HS400 to HS200 because a second device had HS400
  issues. See [emmc.md](docs/subsystems/emmc.md).
- Picture-Quality daemon (`hy310-pqd`) runs, DE2 gamma is live (visible
  black/white toggle confirmed).
- Picture from HDMI input: still not a clean picture. The path forward is
  `sunxi_ge2d`, not `h713_drm`.

See [STATUS.md](STATUS.md) for the subsystem-by-subsystem table.

---

## Discarded hypotheses

These were tried, instrumented, and confirmed wrong. Listed here so nobody
re-tries them.

| # | Hypothesis | Disproved by |
|---|---|---|
| 1 | MIPS HW-IRQ routing can be activated via NMI node | Session G: NMI node enabled, counter-patch still proves MIPS CP0 handler never runs. Polling fallback is mandatory. |
| 2 | `display_cfg.xml mode=2` breaks MIPS cpu_comm init | Session J ran fine with mode=2. The earlier failure was the SELECTIVE WIPE combination, not mode=2 alone. |
| 3 | MIPS `hal_adapter_init` blocks after 1 routine | Stale module on board was masking it. With current kernel, 35/35 routines register in 34 ms. |
| 4 | `INTR_TYPE_SEND=2` is the correct CALL type | MIPS interpreted it as CALL_ACK. Fix: `MSG_TYPE_CALL=0`. |
| 5 | `queueAction` triggers MIPS hang in HISR worker | Bypass test only revealed the wake-up blocker, not the actual cause. Real bug was waiter-tracking-offset (`queue+36 = 0`). |
| 6 | FIFO-slot leak via cache-hack `comm_intrsem[cache_idx]` | Removing the cache-hack and using fresh `Comm_GetFreeCall` per send fixed multi-call (K-night). |
| 7 | ARM→user1 writes are HW-blocked | Session O proved direct `MSG_DATA`-only is blocked, but `MSG_DATA + TX_IRQ_EN doorbell pulse` works. |
| 8 | DTS amp_remote `cpus=2, mips=1` | Session O: stock has `cpus=1 (ARISC), mips=2`. |
| 9 | Stock pure-IRQ TX pattern works on H713 | 0% success rate. H713 is edge-triggered. Pulse-pattern is the pragmatic workaround. |
| 10 | Endpoint `0x8B271C54` is dead | Live dump showed `{type=2, remote=1, port=1, cb=0x8b13afcc}` correctly initialized. Real blocker was `MEMORY[0x8B271C2C]` gate-flag. |
| 11 | MIPS firmware lacks EDID handler / ARISC handles EDID | AUTONOMOUS-25 proved: stock libhalhdmi mcu_comm EDID path is no-op (status -3), ARISC dispatcher 0xbc9c only handles PMU. |
| 12 | ARISC EDID flow is relevant for TMDS lock | EDID loading was solved by direct Synopsys MMIO since session P. ARISC EDID flow is irrelevant. |
| 13 | MIPS state needs reset | Stock doesn't reset MIPS; same firmware works there. |
| 14 | PHY init via `h713_phy_phase1..5[]` arrays | Session S: those 123 entries were COEF_FLT video-scaler coefficients from `display.bin@0x130030`, not PHY init. |
| 15 | snps mainline PHY-init pattern works on H713 | All 15 phy_register_writes timeout. H713 PHY is not snps-PHYCREG-compatible. |
| 16 | Port-3 vs port-1 mismatch | `setsource(5)` → port-mirror=`0x03` correctly, TMDS state identical. |
| 17 | Phantom H6 clocks (hdmi @0xb00, hdmi-slow @0xb04, hdmi-cec, etc.) are H713 | All H6 carry-overs. H713 only has the TV/Display-region clocks. |
| 18 | `tvfe-1296m` clock runs at wrong rate | HW register `0xd20 = 0x80000000` → actual rate is 1296 MHz. Linux-CCU had `parent="ahb"` hardcoded → 150 MHz cosmetic display. CCU-patch is a Linux model fix, not a HW change. |
| 19 | `pll_tvfe` is the TVFE PHY-PLL | Strings come from audio-codec context. Red herring. |
| 20 | MIPS firmware is internally broken | Session V: stock-android shows the picture on the same HW with the same firmware. |
| 21 | MIPS clock rate (8 MHz default) is the blocker | Live test at 300 MHz: state unchanged. |
| 22 | Kernel driver `h713-hdmi-rx` interferes with MIPS | Driver unbind: state unchanged. |
| 23 | `CMU_STATUS LOCK / PHY_STATUS / byte@0x06840001 bit 5 / byte@0x068401AB bit 2` are state-machine progress indicators | Session W via `/dev/hidtvreg`: all of these are zero in stock too, yet stock shows picture. They are NOT the real gate. Cost: 7+ sessions misinterpreted. |
| 24 | Stock 3-IRQ msgbox config must be in DTS to route SPI 108/109 | Mainline GIC doesn't route them (stock uses vendor `wakeupgen` as a routing layer). DTS edits are useless without porting wakeupgen. |
| 25 | Pool-1 fill is the missing piece for HDMI-RX scanout | Sessions DD/DD-NIGHT/EE/autonomous. Pool-1 fully stock-conform via `decd` engagement (kernel module fix + `decd_submit_test` userspace tool) + info-page allocator fix + correct magic constant. Visual stays black. Decd is the wrong driver for this path — stock uses `ge2d_dev.ko`, not `decd.ko`, for HDMI-RX scanout. |

---

## Open issues

| Issue | Status | Notes |
|---|---|---|
| HDMI-RX picture quality (currently tiled 4×1 grayscale) | Open | Next step: swap blacklist to use `sunxi_ge2d` instead of `h713_drm`. The mainline port already exists at `drivers/display/ge2d/`. |
| TVCAP/INCAP frame-capture pipeline | Not started | Out-of-tree `sunxi_tvcap_rx.ko` does TVTOP clock-gates + INCAP magic-sequence. Needs review for current state. |
| HPD plug/unplug auto-detect | Not implemented | IRQ 266 never fires. Polling-watcher planned. |
| DMA-block EDID write puzzle | Open | `DMA_CONFIG10/11` readback is zero but laptops read EDID correctly. Mechanically not understood. |
| ARM↔MIPS msgbox TX reliability (40-60%) | Workaround | Edge-triggered TX with pulse-doorbell. Stock uses level-triggered (different HW expectation). |
| Address-translation race conditions | Monitoring | `cpu_comm_sync_mips_cache` deployed for CALL/RETURN; ACK paths still use the older bit-check pattern. |

---

## Cross-references

| Topic | Detailed in |
|---|---|
| Hardware overview | [docs/hardware.md](docs/hardware.md) |
| System architecture (diagram) | [docs/architecture.md](docs/architecture.md) |
| cpu_comm protocol | [docs/re/cpu-comm-protocol.md](docs/re/cpu-comm-protocol.md) |
| `display.bin` internals | [docs/re/display-bin.md](docs/re/display-bin.md) |
| ARISC firmware | [docs/re/arisc-firmware.md](docs/re/arisc-firmware.md) |
| EDID protocol | [docs/re/edid-protocol.md](docs/re/edid-protocol.md) |
| ge2d_dev.ko vs mainline port | [docs/re/ge2d-port-notes.md](docs/re/ge2d-port-notes.md) |
| HDMI subsystem | [docs/subsystems/hdmi.md](docs/subsystems/hdmi.md) |
| MIPS coprocessor | [docs/subsystems/mips.md](docs/subsystems/mips.md) |
| Display pipeline | [docs/subsystems/display.md](docs/subsystems/display.md) |
| eMMC (HS400→HS200) | [docs/subsystems/emmc.md](docs/subsystems/emmc.md) |
