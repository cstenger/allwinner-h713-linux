# BACKGROUND.md — the why behind hy310-pqd

This document explains how the HY310 projector's Picture Quality (PQ)
stack actually works on Stock Android, what we learned reverse-engineering
it, why the daemon is shaped the way it is, and where the current rough
edges come from.  It exists so that a future session can pick up the
work without re-doing the reverse engineering.

## 1. The hardware

HY310 is a DLP beamer built around an **Allwinner H713** SoC.
Internally it hosts:

```
                 ┌──────────────────────────── Allwinner H713 ─┐
   ARM Cortex    │  ARM cluster (Linux main OS)                │
                 │    │                                        │
                 │    │ msgbox @ 0x03003000                    │
                 │    │   User0 Port1 : MIPS → ARM  RX data    │
                 │    │     +0x164 count, +0x174 data          │
                 │    │   User1 Port1 : ARM → MIPS  TX data    │
                 │    │     +0x464 count, +0x474 data          │
                 │    ▼                                        │
                 │  MIPS32-LE Trident SX6 co-processor         │
                 │    (display.bin runs here)                  │
                 │                                             │
                 │  DE2 display engine @ 0x051C0000 (CTRL)     │
                 │                    @ 0x05208000 (LUT bulk)  │
                 │                                             │
                 │  TCON + HDMI + DLPC3435 output chain        │
                 └─────────────────────────────────────────────┘
```

The MIPS chip ("Trident SX6") is a dedicated TV-processor: it does
video decode, scaling, HDMI-input capture, and runs the Stock PQ
pipeline internally.  ARM is the main Linux OS.  Communication between
them uses an IPC protocol called **FusionDale RPC** over the msgbox
mailboxes.

## 2. Stock Android PQ flow

Stock runs this exact chain on Android 11 to get the Vivid/Cinema/HDR
modes and the slider controls:

```
 Android UI (Settings app)
        │  HIDL call
        ▼
 tvserver  (/vendor/bin/hw/tvserver, service class=early_hal)
        │  links libtvpq.so
        ▼
 libtvpq.so ─── PQControl:: setGammaFactor / setBrightness / setXyz …
        │  virtual calls to ThalDevice vtable
        ▼
 libhaldisplay.so ─── THal_Vp_SetGamma → CalculateGamma → NEON LUT gen
        │                                      │
        │                                      └── direct MMIO to DE2
        │                                          (0x05208000 …)
        ▼
 libhalcpucomm.so ─── Trid_Util_CPUComm_InstallRoutine, CPUComm_CallEx
        │  ioctl on /dev/cpu_comm
        ▼
 cpu_comm_dev.ko ─── msgbox write → MIPS
        │
        ▼
 display.bin on MIPS ─── command_action → Comm_Add2NewCallFifo
                             → dispatch to per-routine handler
```

Stock's config lives in `/vendor/etc/tvconfig/`:

- `tvpq.db`                           — SQLite: Picture_Mode (25 rows),
                                         White_Balance_Mode (20), Gamma_Point (33)
- `pqcontrol_custom_setting.xml`      — current slider values
- `pqcontrol_config_setting.xml`      — per-mode defaults
- `pq_picturemode.ini`                — picture-mode presets
- `pq_colortemp.ini`                  — colour-temp gains/offsets
- `pq_factory_extern.ini`             — 592 KB of Luma/NR/CTI/SSR/CM curves
- `pq_overscan_config.ini`
- `panel_config/*`                    — panel timing

These are read by libtvpq and libhaldisplay at startup and at runtime
when settings change.

## 3. The wire protocol — FusionDale RPC

Reverse-engineered entirely from `libhalcpucomm.so` (42 exported
functions) and `libUtility.so` (Name2ID + CRC32).

### 3.1 Device handle

```
fd = open("/dev/cpu_comm", O_RDWR);
ioctl(fd, 0xC0087F13, &region_a);   // query+mmap first shared region
ioctl(fd, 0xC0087F13, &region_b);   // same, second region
ioctl(fd, 0x80087F34, &cpu_id);     // our own CPU id (0 = ARM)
```

### 3.2 Name → comp_id hash

Each routine has a string name like `THal_Vp_SetGamma`.  Stock prefixes
it with caller-cpu + caller-pid:

```
name_formatted = sprintf("%s_%1x_%3.3x", base, cpu_id, pid & 0xFFF)
                  e.g.  "THal_Vp_SetGamma_1_000"
```

The `comp_id` used for routing is a **CRC32 of the formatted name**:

```
crc = 0x00123456                       // seed
for each byte b in formatted_name:
    crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
return crc
```

The polynomial is standard Ethernet (`0xEDB88320`), confirmed by
byte-for-byte match of the extracted `TriHidtv_crc32_table` in
`libUtility.so:0xC804`.  Test vectors passed live:

```
name2id("THal_Vp_Wce_SetMirrorMode_1_000") = 0x09FFC6EB
name2id("ResetNoticeCPU")                  = 0x892D5F22
name2id("ShowMM")                          = 0x287FB988
```

### 3.3 Install a routine

```
ioctl(fd, 0xC0087F30, &desc_96_bytes);     // IOCTL_INSTALL_RT
```

Descriptor is 96 bytes:

```
+0   u16   channel (0..4)
+4   u32   pid
+8   u32   comp_id  (= CRC32 of formatted name)
+12  char  name[64]
+92  i32   next_index (-1 = end of hash chain)
```

### 3.4 Synchronous CALL

Stock uses `ioctl(fd, 0xC0087F01, &msg)` with an 8-byte ioctl header
that holds only a pointer to the real 168-byte message buffer.  **Our
mainline kernel exposes the same behaviour via `ioctl(fd, 0xC0087F26, buf)`
— identical payload format, different ioctl encoding.**  Our
`CpuComm::call()` uses the mainline number.

The msg buffer layout (relevant fields):

```
+0    u16   routine_name_lo   (filled by FindRoutine)
+2    u16   dst_cpu           (1 = MIPS)
+4    u32   type              (0 = CALL)
+6    u16   flags             (MSG_FLAG_NOTIFY / RETURN_ACK)
+8    u16   msg_type          (kernel writes 1/2)
+12   u32   session_id        (kernel fills)
+16   u32   dst_cpu expanded  (kernel fills from FindRoutine)
+28   u32   pid
+32   u64   wait_obj
+40   u32   comp_id           ← MUST be set by caller
+64   u32[10] params          ← caller fills, count at [0]
+120  u32[10] result           ← kernel writes after ACK
```

### 3.5 Round-trip confirmed

With a matching ARM-side stub installed, `CPUComm_CallEx` for MIPS's
one registered routine (`THal_Vp_Wce_SetMirrorMode_1_000`, channel 0x30)
completes in **34 ms** — proof that transport, naming, and kernel
dispatch all work end-to-end.

## 4. Name/channel semantics on MIPS

MIPS's `Comm_AddNewChannel` (`sub_8B11CD08` in display.bin) stores
channels with the key

```
channel_id = (thread_id << 4) | (name & 0xF)
```

where `thread_id` and `name` are the parameters it passed in.  For the
one registered routine:

```
THal_Vp_Wce_SetMirrorMode_1_000
  chan_slot = 0
  channel_id = 0x30  (= 3 << 4 | 0)
  funcRoutine = 0x8B192…
```

This is **independent** of the comp_id (CRC32) that ARM uses for its
SharedMem hash table.  They live on different sides:

```
 ARM side                     MIPS side
 ────────                     ─────────
 SharedMem hash table  ─┐
  key = comp_id         │ read by both sides
  keyed by CRC32(name)  │
                        │
                        ▼ MIPS's own local pool
                          key = (thread_id<<4) | (name & 0xF)
                          populated only by MIPS's hal_adapter_init
```

## 5. DE2 gamma — the direct MMIO path

Reverse-engineered from `libhaldisplay.so::WriteGammaLUTByColor`
(`sub_0xB881`, 604 bytes).

### 5.1 Registers

All physical addresses, accessed via `mmap(/dev/mem)`:

| Addr        | Name            | Notes                                     |
| ----------- | --------------- | ----------------------------------------- |
| 0x051C00E8  | DISPLAY_CTRL    | LUT write path + double-buffer control    |
| 0x051C0174  | DISPLAY_STAT    | frame status (high 16 bits = state)       |
| 0x05208000  | LUT bank R      | 512 × u32, each holds two 12-bit samples  |
| 0x05208800  | LUT bank G      | same                                      |
| 0x05209000  | LUT bank B      | same                                      |

### 5.2 Control-register bits

```
bit 23  0x00800000   LUT_WRITE_EN   arms the LUT write port (self-clears)
bit 22  0x00400000   CHAN_LATCH     channel-write latch
bit 30  0x40000000   COMMIT         begin write transaction
bit 28  0x10000000   DBUF_FLIP      double-buffer front/back toggle
bit 21  0x00200000   LATCH_A        apply latch A
bit 20  0x00100000   LATCH_B        apply latch B
bit 26  0x04000000   SCANOUT_KICK   request scanout resample
```

### 5.3 Write sequence (exactly what stock does)

```
1. read DISPLAY_STAT hi16; spin up to 40 ms until the value changes.
   The stock cache is in dword_11CDC; we replicate with a local static.
2. save orig_ctrl = read(DISPLAY_CTRL).
3. set LUT_WRITE_EN, CHAN_LATCH, COMMIT.
4. bulk-write 512 u32 into LUT bank R.
5. bulk-write 512 u32 into LUT bank G.
6. bulk-write 512 u32 into LUT bank B.
7. clear COMMIT.
8. write bit 28 = orig_ctrl ^ 0x10000000     (double-buffer flip)
9. set bit 21.
10. set bit 20.
11. set bit 26.
```

Steps 7–11 are the **activation sequence** — without them, the LUT data
is staged but never applied to the scanout path.  Live-tested on
labwc: writing all-zero samples produces a fully black screen; writing
identity restores normal output.

### 5.4 Sample packing

Each u32 packs two adjacent 12-bit samples:

```
u32[i] = (samples[2*i + 1] << 12) | samples[2*i]
```

Stock's NEON code does this via `vld2_s16` + `vshll_n_u16` + `vaddw_u16`;
we do the scalar equivalent.  Total 1024 samples per colour × 3 colours.

### 5.5 What DOESN'T work

- `/dev/mem` writes are only effective when DE2 is actively clocked.
  On our mainline kernel, `h713_drm.ko` (in
  `/opt/hy310/repo/drivers/display/drm/`) brings up DE2 when a DRM
  client is running.  If the only thing on the screen is the static
  U-Boot logo (no Linux compositor), DE2 is idle and gamma writes have
  no visible effect — the stock path writes correct values but they
  never reach the panel.
- Stock uses a separate kernel driver `/dev/hidtvreg` (from
  `hidtvreg_dev.ko`) to map the display registers.  Our path via
  `/dev/mem` works but requires root.

## 6. The MIPS blocker

MIPS boots its display.bin firmware, which runs `hal_adapter_init`
(`sub_0xAD78`).  `hal_adapter_init` is supposed to register **64** VP
routines (`THal_Vp_Set/Get…`) in MIPS's local channel pool.

Currently it registers **only one** — `THal_Vp_Wce_SetMirrorMode_1_000`
at channel 0x30 — then aborts.  The MIPS elog shows:

```
E/DisplayAPI  PQDriver/Gamma/Gamma.cpp 340  Can not get gamma LUT data,
                                            dwGammaType: 0x30030003
```

Because the rest of the PQ routines are never registered, every ARM →
MIPS call to any of them is silently dropped by MIPS's
`Comm_Add2NewCallFifo` ("no channel for id 0xNN").  Our transport
layer still fires them and the kernel's msg goes through — MIPS ACKs
the transport but returns nothing in the result FIFO, producing the
"null return" log on ARM.

### 6.1 Why it aborts

`display.bin` at `0x8B152A60` registers 6 gamma LUT types with pointers
into a reserved DRAM region (`0x8B48xxxx` = phys `0x4B48xxxx`).
Registration loops through types 0x30030000..0x30030005; type
`0x30030003` is the one failing.

What we've confirmed:

- The 6 address slots exist.  On a system that recently ran Stock
  Android, the slots even contain plausible gamma curve data (residual
  from the prior boot — DDR is not cleared between boots).
- No stock file ships the LUT data as a binary.  Scanning every file
  in `/vendor/` for the gamma-LUT byte pattern returned zero hits.
- Therefore the LUT data must be **computed at runtime** by libtvpq's
  `CalculateGamma` (4.2 KB NEON SIMD function in libhaldisplay.so,
  `sub_0xBC49`).

### 6.2 What unblocks it ("Strategy A1")

To get MIPS to register the remaining 63 routines we must supply the 6
gamma LUTs **before MIPS boots**.  Three sub-options:

- Extend `sunxi-mipsloader.c` to generate and write the 6 × 6 KB of
  gamma data to `0x4B48xxxx` before releasing MIPS from reset.
- Extend the U-Boot boot sequence to preload a `gamma_lut.bin` at the
  same address (requires authoring that file once).
- Decompile `CalculateGamma` fully and reimplement it in userspace,
  then have the daemon pre-populate the LUTs from the 33-point curves
  in `tvpq.db::Gamma_Point` before loading MIPS.

Option (3) is the cleanest but requires porting a non-trivial NEON
algorithm.  Option (1) is the quickest path to unblocking MIPS.

Once MIPS has all 64 channels registered, every `THal_Vp_*` call from
our daemon will succeed without further code changes.

## 7. The kernel fixes we made along the way

Our `cpu_comm` driver (under
`/opt/hy310/kernels/working/drivers/soc/sunxi/cpu_comm/`) required
three small patches to make Stock's IPC semantics work:

1. `proto.c`: fall back to `msg[2]` for dst_cpu when
   `routine_find_buf[2]` is actually a MIPS thread_id (>1).  Needed
   because MIPS uses the same field position for two different
   purposes (routing vs. channel-key high nibble).
2. `rpc.c::CPUComm_CallEx`: hard-set `msg[2] = 1` so the HY310-fix in
   (1) has something meaningful to read.
3. `rpc.c::CPUComm_CallEx`: remove the redundant
   `FindWaitBySessionId` + `sem_down_interruptible` block after
   `SendComm2CPUEx`.  `SendComm2CPUEx` already blocks on the slot
   semaphore until MIPS ACKs; doing a second wait created a race
   against the dispatcher's own wait-object cleanup, which
   consistently lost and printed "wait obj not found".

All three are marked "HY310-fix" in the source comments.

## 8. Known rough edges

- **FIFO-slot leak** when calling MIPS routines that don't exist.
  MIPS ACKs but sends no return data; our kernel's slot-release path
  currently only fires when a return is produced.  After ~20 calls
  the share-seq FIFO fills and the kernel returns `-EBUSY`.  Two
  possible fixes: have the dispatcher release on CALL_ACK alone, or
  have the daemon skip apply_all until MIPS is known-ready.
- **Gamma invisible without DE2 pipeline**.  If the screen content is
  not being driven by a Linux compositor over `h713_drm` (i.e. just
  the U-Boot logo), our gamma writes succeed silently without visible
  effect.
- **Gamma calibration port in progress**.  `include/pq_calculate_gamma.h`
  + `src/pq_calculate_gamma.cpp` now provide a pure, hardware-free
  `calculate_gamma(...)` path that applies decomp-backed invariants
  (33→1024 interpolation, 12-bit clamp, `pow(..., gamma/2.2)` shaping)
  and emits all six required output slots.  Remaining gap: extract and
  validate stock-specific per-type/per-mode divergence constants
  (`dword_4A50`, per-type 0x30030000..05 differences) from richer IDA
  artifacts so outputs are byte-identical to stock for all modes.

## 9. Why this daemon and not just "port libtvpq"

Stock libraries are Android HAL consumers: they depend on `libhidlbase`,
`libbase`, `libcutils`, `libutils`, and a bionic userspace.  Running
them on mainline Linux is possible but brittle — they expect Android
property services, HIDL infrastructure, and a specific libc++ ABI that
Debian's ARM toolchain doesn't match.

The surface we actually need is small:

- Read a few config files (SQLite + INI + XML).
- Talk to `/dev/cpu_comm` (documented protocol above).
- Write a LUT to DE2 (documented register sequence above).

Re-implementing that as a clean daemon in standard C++20 gives us a
single static binary (~185 KB) with no Android-dependency surface,
first-class systemd integration, and a simple CLI for runtime control.
The trade-off is that we own more code — but the code we own is all
small and focused.

## 10. Cross-session context

- Main source tree: `/opt/hy310/hy310-pqd/` on **openclaw**.
- Windows mirror: `C:\Users\User\AppData\Local\Temp\hy310\hy310-pqd\`.
- Test builds deploy to `hy310:/tmp/`; production install goes to
  `/usr/sbin/hy310-pqd` + `/usr/bin/hy310-pqctl` + `/etc/systemd/system/hy310-pqd.service`.
- Reference IDA databases live in `/tmp/pq_re/` on openclaw:
  `libhalcpucomm.so.*`, `libtvpq.so.*`, `libhaldisplay.so.*`,
  `libUtility.so.*`.  Decompilation output in
  `/tmp/pq_re/halcpucomm_decomp.txt`, `/tmp/pq_re/haldisp_decomp.txt`,
  `/tmp/pq_re/util_decomp.txt`.
- MIPS firmware IDA DB: `/tmp/display_b0.i64` on openclaw
  (base=0, MIPS VA = offset + 0x8B100000).
- Stock partition extracts: `/opt/hy310/stock-re/super_extracted/`
  (vendor/ system/ product/).
- Handoffs from prior sessions:
  `C:\Users\User\Desktop\weltneuheit\HANDOFF-IPC-SESSION-*.md`
- H713 DRM driver (used to light up DE2): in
  `/opt/hy310/repo/drivers/display/drm/h713_drm.c`; built-in-tree
  integration is still pending.
