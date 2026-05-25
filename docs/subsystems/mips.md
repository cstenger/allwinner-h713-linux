# MIPS coprocessor

**Status**: ✅ working. Firmware loads, all 81 routines register, state
machine reaches state 5 (Running), 1080p60 signal detection works.

The H713 has a MIPS32 LE coprocessor that owns the display engine, HDMI-RX
state machine, and picture-quality pipeline. It runs `display.bin`, a
1.25 MB FreeRTOS-style firmware loaded by U-Boot before the kernel boots.

## Quick check

```sh
ssh hy310 'cat /sys/class/hy300/mips/state_machine'
# Expect: 5 (Running)

ssh hy310 'cat /sys/class/hy300/mips/elog_full | tail -20'
# Expect: recent log lines from MIPS firmware
```

## Firmware loading

`display.bin` lives on the FAT partition (`/dev/mmcblk0p1` mounted at
`/1/`):

```
/1/mips/display.bin     # 1.25 MB MIPS firmware
/1/mips/display_cfg.xml # config: elog mode, etc.
/1/mips/TSE.bin         # picture-quality data (factory tuning)
```

**U-Boot loads** `display.bin` into ARM-phys `0x4B100000` (= MIPS-VA
`0x8B100000`) and starts MIPS execution. By the time Linux runs, MIPS is
already alive.

`sunxi-mipsloader.c` (in `drivers/misc/`, built-in via
`CONFIG_SUNXI_MIPSLOADER=y`) does the post-boot setup: writes the
SharedMem base address to the CCU share registers, manages the elog
ringbuffer, exposes `/sys/class/hy300/mips/*` controls.

## Memory map

| ARM phys | MIPS-VA | Purpose |
|---|---|---|
| `0x4B100000` | `0x8B100000` (KSEG0 cached) | code + data |
| `0x4C300000–0x4D700000` | various | working memory, frame buffers |
| `0x4D941000` | — | `decd_reserved` info-page pool (128 KB) |
| `0x4E300000` | `0xAE300000` (KSEG1 uncached) | 5 MB cpu_comm SharedMem |
| `0x4E340000` | `0xAE340000` | counter-cave instrumentation (debug patches) |

Translation: `phys = MIPS-VA - 0x40000000` for code, `phys = MIPS-VA -
0x60000000` for SharedMem.

MMIO from MIPS goes through the `readl_checked` helper at `0x8B17FC70`,
which does `phys = (addr + 0xB5000000) | 0x20000000` internally.

## State machine (HDMI-RX)

The MIPS firmware drives a 5-state machine for each HDMI-RX port:

| State | Name | What happens |
|---|---|---|
| 1 | Initial | set by Port_Init |
| 2 | Sleep | asserts HDCP + DDC reset |
| 3 | Idle | releases HDCP+DDC, asserts TMDS reset, AEC enable |
| 4 | TransitionUp | 8 IRQ-mask writes for PHY_INT_FREQ_DET enable |
| 5 | Running | releases TMDS reset, FreqDetect runs |

**State 3 → 4** needs `Vp_Init` to be called with `Para[2] != 0`. Para[2]
is used by MIPS as a phys-addr pointer for a 55 KB factory-PQ memcpy.
With `Para[2] = 0` (mistake we made for many sessions), MIPS faults with
a NULL memcpy, BG_Thread breaks, kernel-MM corruption cascades into
unrelated processes (`*pte=0x8baa0000` pattern).

**Fix**: `Para[2] = SHMEM_PHYS_BASE + 4 MB = 0x4E700000` (staging area in
the shared-mem region). `hy310-hdmird` does this in its init sequence.

The other 4 transitions happen autonomously once Vp_Init is right.

### What the "lock bits" don't tell you

The "obvious" indicators we spent 7+ sessions on are **zero in stock too**:

- `CMU_STATUS LOCK bit 4`
- `PHY_STATUS`
- `byte@0x06840001 bit 5`
- `byte@0x068401AB bit 2`

These are NOT progress indicators. Stock reaches state 5 with all of them
zero. The real gate is `Vp_Init Para[2]`. Don't chase the lock bits.

## Routines registered at boot

About 80-85 routines (depending on firmware variant). They are register
via `hal_adapter_init` (`0x8B10AD78`) during MIPS init. Calling
`hal_adapter_init` triggers `InstallRoutine` (`0x8B1247EC`) which formats
the name + hashes it.

See [docs/re/cpu-comm-protocol.md](../re/cpu-comm-protocol.md) for the
full routine list with comp_ids.

## elog (MIPS firmware log)

Three modes selected via `display_cfg.xml`:

- **Mode 0**: no log (default in production)
- **Mode 1**: ~120 KB ring buffer at `0x4B272D9C`
- **Mode 2**: 2 MB linear buffer (we use this for debugging)

Tools:
```sh
# Ring-buffer dump (linear scan, fast):
ssh hy310 'cat /sys/class/hy300/mips/elog_full | tail -100'

# Mode-2 2 MB linear (bypasses STRICT_DEVMEM via mmap):
ssh hy310 'python3 /root/mips_elog2.py | tail -50'

# Unscramble (newer firmware encodes the log; needs the unscramble tool):
ssh hy310 'python3 /root/unscramble_elog.py'
```

Optional Phase-1 patch: prepend `[SSSSSSSS]` 8-digit seq numbers to log
lines for tracing. See `RE/display.bin-patches.md`.

## Critical IDA addresses

From `display.bin.i64` (renames accumulated across sessions H–W):

| VA | Name | Purpose |
|---|---|---|
| `0x8B100000` | start of firmware | |
| `0x8B100600` | counter-cave entry hook | Session-X RETURN-write tracker |
| `0x8B10AD78` | `hal_adapter_init` | registers 81 routines |
| `0x8B11D418` | `Comm_Add2NewCallFifo` | check FIFO + signal `_GotCallSem` |
| `0x8B11E8AC` | `DoIntr2CPU2` | sends type-byte to ARM endpoint |
| `0x8B11EF78` | `cpu_comm_handle_CPU2_call` | CALL dispatcher |
| `0x8B12156C` | `msgbox_IRQ_handler` | (counter @ `0xAE340000`) |
| `0x8B12254C` | `cpu_comm_msg_cb` | type-switch (0=CALL, 1=RETURN, 2=CALL_ACK, 3=RETURN_ACK) |
| `0x8B1247EC` | `InstallRoutine` | name-format + hash |
| `0x8B131958` | `THDMIRx_Ctor` | 3 ports + 3 threads |
| `0x8B138738` | `THDMIRx_StateMachine_Dispatcher` | 5-state dispatcher |
| `0x8B17FC70` | `readl_checked` | MMIO read helper (KSEG1 transform) |
| `0x8B17FD10` | (MMIO write helper) | counter target (session X) |
| `0x8B271C2C` | HPD gate-flag | set by `THDMIRx_SetPortMap` |

Full reference: [docs/re/display-bin.md](../re/display-bin.md).

## display.bin patches

Stock MD5: `0d2191ca0dad3c17cd7db6ffa47217f5`.
Current MD5: `12d9635ed3a73065b260dd7227d133db` (with counter-cave + nop patches).

| File offset | Address | Patch | Purpose |
|---|---|---|---|
| `0x002C4` (24 B) | `0x8B1002C4` | counter-cave | Trampoline at `msgbox_IRQ` entry, counter @ `0xAE340000` |
| `0x2156C` | `0x8B12156C` | `j 0x8B1002C4` | jump to cave |
| `0x1A52C` | `0x8B11A52C` | `nop` | spam-kill PLF-CPU-ready |
| `0x238D8` | `0x8B1238D8` | `nop` | ShStartAddr-spam |
| `0x600` (64 B) | `0x8B100600` | counter-cave (session X) | RETURN-write tracker |
| `0x7FD10` | `0x8B17FD10` | `j 0x8B100600` + nop | jump to entry hook |

These all preserve revert-bytes. The counter-caves are 24 B (6 instructions),
safe for MIPS stack. Larger caves with atomic ops (Session J tried ll/sc
hex-formatting) caused MIPS stack overflow → reverted to simple counters.

## Deploy

```sh
# Apply patches:
ssh openclaw 'python3 /opt/hy310/tools/patch_display_bin.py'

# Push to device:
scp openclaw:/tmp/display.bin.current hy310:/tmp/

# Install:
ssh hy310 'mount /dev/mmcblk0p1 /1 2>/dev/null
           cp /tmp/display.bin.current /1/mips/display.bin && sync'

# Physical power-cycle (firmware loads only at U-Boot).
```

## Quirks worth remembering

- **MIPS HW IRQ (CP0 IRQ 4) is never delivered to MIPS** on H713. Hypothesis
  was that NMI-node or some routing would enable it — both confirmed
  ineffective (sessions G, X). MIPS must poll instead of taking IRQs from
  the msgbox.
- **ssh reboot can kill MIPS**. If `cat /sys/class/hy300/mips/state_machine`
  returns nothing after a software reboot, the MIPS is in a stuck
  WAIT-ACK state and needs a physical power-cycle.
- **MIPS bus access to AFBD region** — display.bin has 1 ref to
  `0x05600000` byte-pattern, but it's a byte-overlap between two adjacent
  data values, not a code reference. MIPS does **not** write to AFBD pool-1
  directly. That's ARM-side.

## See also

- [cpu_comm subsystem](cpu-comm.md) — the protocol over the shared mem
- [docs/re/display-bin.md](../re/display-bin.md) — full firmware RE
- [SESSIONS.md](../../SESSIONS.md) sessions Z, X for the Vp_Init story
