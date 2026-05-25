# display.bin — MIPS firmware RE

The MIPS coprocessor runs `display.bin`, a 1.25 MB FreeRTOS-style firmware
that owns the display engine, picture quality, and HDMI-RX state machine.

## File facts

- Size: 1.25 MB (1 256 216 bytes)
- Location on device: `/dev/mmcblk0p1` (FAT, mounted at `/1`) →
  `/1/mips/display.bin`
- Load address: ARM-phys `0x4B100000` (= MIPS-VA `0x8B100000` KSEG0)
- Loaded by: U-Boot, **before** Linux kernel boots
- Stock MD5: `0d2191ca0dad3c17cd7db6ffa47217f5`
- Current patched MD5 (with debug caves): `12d9635ed3a73065b260dd7227d133db`

## Loading into IDA Pro (or Ghidra)

```
Processor type: MIPS little-endian (MIPS32 R2 or similar)
ABI: o32
Endianness: Little-endian
Load address: 0x8B100000
```

For Ghidra, set "MIPS 32 little endian, default" with the same base.

The current IDA workspace lives at `/opt/hy310/stock-re/display.bin.i64`
on the build server, with renames + comments accumulated across 50+ RE
sessions. Worth using rather than starting from scratch.

## Memory map

| MIPS-VA range | Purpose |
|---|---|
| `0x8B100000` – `0x8B23FFFF` | code |
| `0x8B240000` – `0x8B27FFFF` | data + .bss |
| `0x8B280000` – `0x8B2FFFFF` | heap |
| `0xAE300000` – `0xAE7FFFFF` | shared memory with ARM (5 MB, KSEG1 uncached) |
| `0xAE340000+` | counter-cave instrumentation slots |

Address translation rules:

- code/data: `MIPS-VA - 0x40000000 = ARM-phys`
  Example: `0x8B12156C → 0x4B12156C`
- shared mem: `MIPS-VA - 0x60000000 = ARM-phys`
  Example: `0xAE300000 → 0x4E300000`
- HW MMIO: via the `readl_checked` helper at `0x8B17FC70`. Internally it
  applies `phys = (addr + 0xB5000000) | 0x20000000`.

## Critical functions

### cpu_comm IPC

| VA | Name | Purpose |
|---|---|---|
| `0x8B12156C` | `msgbox_IRQ_handler` | (counter-patched in session H) |
| `0x8B12254C` | `cpu_comm_msg_cb` | switch on `comm_type` (0=CALL, 1=RETURN, 2=CALL_ACK, 3=RETURN_ACK) |
| `0x8B11EF78` | `cpu_comm_handle_CPU2_call` | check `share_seq[8] & 4` |
| `0x8B11F234` | `cpu_comm_handle_CPU2_return` | RETURN handler |
| `0x8B11F4F0` | `cpu_comm_handle_CPU2_callACK` | check `share_seq[105] & 4` |
| `0x8B11F6D8` | `cpu_comm_handle_CPU2_returnACK` | — |
| `0x8B11EDD4` | `queueAction` | `osa_hisr_activate(slot)` |
| `0x8B121BB4` | `osa_hisr_activate` | schedule HISR via SWI |
| `0x8B15BB80` | `hisr_schedule_via_SWI` | `tx_queue_send_core` (normal) / enqueue (exception) |
| `0x8B103504` | `tx_queue_send_core` | ThreadX queue enqueue |
| `0x8B1219A4` | `hisr_worker_thread` | dispatch loop |
| `0x8B121238` | `call_action` | trampoline → `comm_Action` |
| `0x8B1211A0` | `comm_Action` | type dispatcher |
| `0x8B120A94` | `command_action` | CALL processor + sends ACK |
| `0x8B11D418` | `Comm_Add2NewCallFifo` | check FIFO + signal `_GotCallSem` |
| `0x8B118B84` | `Comm_ReleaseFreeCall` | dequeue from FreeCall pool |
| `0x8B118130` | `fifo_getItemWr` | FIFO full-check |
| `0x8B118304` | `fifo_requestItemWr` | — |
| `0x8B11C4C4` | `AddInRoutine` | uses `comm_SpinLock(2)` |
| `0x8B125A98` | `comm_SpinLock_wrap` | — |
| `0x8B11E8AC` | `DoIntr2CPU2` | sends type-byte to ARM endpoint |
| `0x8B121CB0` | `send_u32_to_arm_endpoint` | — |
| `0x8B121870` | `arisc_endpoint_send` | poll FIFO_STAT + write |
| `0x8B10AD78` | `hal_adapter_init` | registers 81–85 routines |
| `0x8B1247EC` | `InstallRoutine` | name-format + hash |

### Threading / sync

| VA | Name |
|---|---|
| `0x8B17FC70` | `readl_checked` (MMIO helper, KSEG1 transform) |
| `0x8B17FD10` | (MMIO write helper) |
| `0x8B15BF90` | `guru_meditation_panic` (infinite loop on assert) |

### HDMI-RX state machine

| VA | Name | Purpose |
|---|---|---|
| `0x8B131958` | `THDMIRx_Ctor` | 3 ports + 3 threads |
| `0x8B130C54` | `HDMIRX_SetPortMap` | sets `MEMORY[0x8B271C2C] = 1` (HPD gate-flag) |
| `0x8B138738` | `THDMIRx_StateMachine_Dispatcher` | 5 states |
| `0x8B13E8AC` | `THDMIRx_Port_CheckSignalReady` | reads `byte+1 bit 5 AND byte+0x1AB bit 2` — both zero in stock AND mainline, NOT the real gate |
| `0x8B13FEAC` | `HdmiRx_Video_TMDS_FreqDetect_IRQHandler` | enabled in state 4 |

### Source switch

| VA | Name |
|---|---|
| `0x8B14AB68` | `THal_Vp_SetSource` (source_id 0..11 valid) |
| `0x8B109174` | `AppTopSetSource_wrapper` (postmessage to thread) |
| `0x8B108170` | `AppTopProjector_ApplyNewSource` |

## HDMI-RX state machine

| State | Name | Effect |
|---|---|---|
| 1 | Initial | set by `Port_Init` |
| 2 | Sleep | asserts HDCP+DDC reset |
| 3 | Idle | releases HDCP+DDC, asserts TMDS reset, AEC enable |
| 4 | TransitionUp | 8 IRQ-mask writes for PHY_INT_FREQ_DET enable |
| 5 | Running | releases TMDS reset, FreqDetect runs |

Transition 3 → 4 needs `CheckSignalReady() = true`. **But the bits it
checks (byte+1 bit 5, byte+0x1AB bit 2) are zero in stock too.** They are
NOT the real gate. The real gate is `Vp_Init Para[2] != 0` — see
[docs/subsystems/mips.md](../subsystems/mips.md) for the explanation.

## Patches

| File offset | VA | Original | Patch | Purpose |
|---|---|---|---|---|
| `0x002C4` (24 B) | `0x8B1002C4` | zeros | counter-cave bytes | Trampoline at `msgbox_IRQ` entry, counter @ `0xAE340000` |
| `0x2156C` | `0x8B12156C` | `D0 FF BD 27` | `B1 00 C4 0A` | `j 0x8B1002C4` (Session H) |
| `0x1A52C` | `0x8B11A52C` | `1A 40 C5 0E` | `00 00 00 00` | nop spam-kill PLF-CPU-ready (line 917) |
| `0x238D8` | `0x8B1238D8` | `1A 40 C5 0E` | `00 00 00 00` | nop ShStartAddr-spam (line 1141) |
| `0x600` (64 B) | `0x8B100600` | zeros | counter-cave (Session X) | Hook `sub_8B17FD10` entry; counters @ `0x4E340020/24/28/2C` |
| `0x7FD10` | `0x8B17FD10` | prologue | `j 0x8B100600` + nop | Session-X RETURN-write tracker |

### Patches that were tried and reverted

- 7 counter-caves at `queueAction`/`osa_hisr_activate` (file offsets
  `0x500/0x520/0x540/0x560/0x590/0x5B0/0x5D0`) — sessions H/I. Reverted
  when no longer needed.
- Lock-skip NOPs (`0x1C67C`/`0x1C70C` in `AddInRoutine` spinlock-2) —
  session J reverted after lock-2 init bug was properly fixed.
- Session-J Phase-1 cave with atomic counter (ll/sc + hex formatting) —
  caused MIPS stack overflow. Reverted. Simple 24-byte caves (6
  instructions) are safe; anything larger risks overflow.

## Deploy procedure

```sh
# Apply patches on build server:
ssh openclaw 'python3 /opt/hy310/tools/patch_display_bin.py'

# Push to device:
scp openclaw:/tmp/display.bin.current hy310:/tmp/

# Install:
ssh hy310 'mount /dev/mmcblk0p1 /1 2>/dev/null
           cp /tmp/display.bin.current /1/mips/display.bin && sync'

# Physical power-cycle (display.bin only loaded by U-Boot at boot).
```

## elog (firmware log)

Three modes selected by `/1/mips/display_cfg.xml`:

- Mode 0: no log
- Mode 1: ~120 KB ring buffer at `0x4B272D9C`
- Mode 2: 2 MB linear buffer (good for debugging)

Tools on the device:

```sh
# Ring buffer:
cat /sys/class/hy300/mips/elog_full | tail -100

# Linear mode 2:
python3 /root/mips_elog2.py | tail -50

# Unscramble (newer firmware encodes lines):
python3 /root/unscramble_elog.py
```

Optional patch: Phase-1 seq-prefix (`[SSSSSSSS]` 8-digit counter) prepended
to log lines for tracing. See [SESSIONS.md](../../SESSIONS.md) and
`PLAN-MIPS-LOGGING-OVERHAUL.md`.

## Counter-cave instrumentation

We use small assembly patches that increment counters at `0x4E340000+`
when reached. Reading the counter from ARM-side tells us how often a
function fires.

The currently deployed cave at `0x8B100600`:

```
# 16 instructions, 64 bytes
# Hook entry of sub_8B17FD10 (MMIO write helper)
# Increments 4 counters based on (addr, value) match:
#   counter[0] @ 0xAE340020: total writes
#   counter[1] @ 0xAE340024: writes to 0x03003174 (MIPS→ARM type byte)
#   counter[2] @ 0xAE340028: writes with value==1 (RETURN type-byte)
#   counter[3] @ 0xAE34002C: writes with value==2 (CALL_ACK)
```

Read from ARM via `hdump 0x4E340020 0x10` (on stock) or via `/dev/mem`
on mainline.

## See also

- [MIPS subsystem](../subsystems/mips.md)
- [cpu_comm protocol](cpu-comm-protocol.md) — caller-side
- [SESSIONS.md](../../SESSIONS.md) sessions H–X for the RE narrative
