# ARISC firmware — OR1K BE, 172 KB

The H713 has a third CPU besides ARM and MIPS: the **ARISC SCP**, a
small OR1K big-endian core that owns power management, the HPD pin,
and (in theory) EDID storage.

## File facts

- ISA: OpenRISC 1000, big-endian
- Size: 172 KB
- Source: extracted from `bootloader_a.bin` TOC1 item "scp" at offset
  `0xb0c00` (word-byte-reversed in storage — swap word bytes for objdump)
- Loaded by: BL31 / U-Boot, **before** Linux kernel

## Loading into Ghidra / IDA

```
Processor: OpenRISC 1000
Endianness: big-endian
Base address: 0x00000000 (entry at 0x100)
```

You'll want a small Python pre-processor to swap word bytes
(`A B C D → D C B A` per 4-byte word) before disassembly. The raw file
isn't byte-order-flipped consistently with what objdump wants.

## Memory layout

| Range | Purpose |
|---|---|
| `0x0100` | reset vector |
| `0x07970` | msgbox-recv polling loop (`sub0 port 3 FIFO_STAT`) |
| `0xbc9c` | PMU dispatcher (jump table — only PMU types) |
| `0xc5a4` | main loop (`"Wait %d ms"` printf signature) |
| `0x114e8` | outer dispatcher entry (r3=0, r4=16..21) |
| `0x11520` | sub-dispatcher → tail-call to `0x12490` |
| `0x12490` | SCPI/HDMI dispatcher, jump table at `0x15b48`, 9 cases |
| `0x12330` | HPD intermediate handler |
| `0x121e4` | HPD pin writer (writes `0x07091014`) |
| `0x1242c` | HPD subcmd 0x16 entry |
| `0x12b18` | init |

## Strings worth knowing

These were used as anchors to identify functions:

- `"Host Set EDID Version. 0x%x"`
- `"hpd %d UP/DOWN/RESET"`
- `"Chip HPD RX0/1/2"`
- `"feedback startup result [%d]"`
- `"ar100 firmware version : %s"`
- `"VS App init done"`
- `"Wait %d ms"` (main loop)

## The two dispatchers

**Big surprise from session T** (corrects the AUTONOMOUS-25 single-dispatcher
hypothesis):

### `0xbc9c` — PMU dispatcher

Handles only PMU message types: 25, 34, 36-38, 96-100. This is the standard
ARISC role on Allwinner SoCs.

### `0x12490` — SCPI/HDMI dispatcher

Jump table at `0x15b48`, 9 cases (0..8):

| Case | Function | Notes |
|---|---|---|
| 0 | EDID Set | returns `status=-3` |
| 1 | EDID Read | returns `status=-3` |
| 2 | EDID Output | returns `status=-3` |
| 3 | EDID Reset | returns `status=-3` |
| 4 | RequestPortNumber | returns `status=-3` |
| 5 | PullHotPlug | actually does HPD writes |
| 6 | … | mostly `status=-3` |
| 7 | … | mostly `status=-3` |
| 8 | … | mostly `status=-3` |

Outer entry at `0x114e8` (r3=0, r4=16..21) → sub-dispatcher `0x11520` →
tail-calls `0x12490`.

**Status -3 = "imt error" = invalid message type.** These handlers are
**hollow by design** — stock libhalhdmi sends EDID sub-commands to ARISC,
ARISC says "no thanks", and the EDID is actually written via direct
Synopsys `DMA_CONFIG10/11`. The mcu_comm path is no-op.

Cases that actually do work:

- Case 1 (REQUEST_EDID_STATUS): tested empty payload → response
  `a5 aa 01 fd 00 00 00 00` (length `0xfd` = -3 = "EDID not configured")
- Case 5 (HPD writes): writes `0x07091014`, single-bit per port

## Msgbox plumbing

ARISC listens on User1 sub-block 0 port 3 (FIFO_STAT at `0x0300346c`).

ARM→ARISC TX address: `0x0300347c` (MSG_DATA) + `0x03003430` (TX_IRQ_EN
doorbell pulse).

H713 is edge-triggered. Stock pure-IRQ pattern doesn't work — see
[cpu-comm-protocol.md](cpu-comm-protocol.md) for the workaround.

## Packet format

Outbound from ARM:

```
byte 0: 0xA5  marker
byte 1: seq   rotating
byte 2: type  (case to dispatch 0..8)
byte 3: length
bytes 4..7: pad
bytes 8..: payload
```

Inbound from ARISC: same structure with `status` byte instead of `type`.

Verified roundtrip (session O):

```
TX: a5 01 01 00 00 00 00 00          (case 1, empty payload)
RX: a5 aa 01 fd 00 00 00 00          (status -3 "EDID not configured")
```

## HPD handler — `0x121e4`

This is what actually drives the HPD pin (port 1 = bit 1 of register
`0x07091014`). Stock firmware calls it via cmd `0xB0`/`0xA0` packets over
User1.

The 3-bit register is undocumented in any Allwinner datasheet we've seen.
Discovered by RE'ing the ARISC firmware HPD handler entry path.

## What's MIPS doing in parallel

MIPS sends HPD info to ARISC via User1 sub-block 1 port 1
(`0x03003574`). When MIPS detects a 5 V change on the HDMI-RX cable,
it sends a packet to ARISC, which then drives the HPD pin.

Stock libhalhdmi has 10 sub-cmds for HDMI but ARISC responds with `-3`
to all of them except HPD writes. The real EDID write path is direct
Synopsys.

## Session history

| Session | Finding |
|---|---|
| N (2026-04-24) | ARISC firmware extracted from BL31 |
| O (2026-04-25) | ARM→ARISC HW path proved (MSG_DATA + TX_IRQ_EN doorbell pulse), bidirectional rpmsg working |
| AUTONOMOUS-25 (2026-04-25) | Dispatcher `0xbc9c` RE'd (PMU only), display.bin confirmed without EDID handler, Synopsys `DMA_CONFIG10/11` recommended as EDID path |
| T (2026-04-30) | Second dispatcher `0x12490` discovered (9 cases EDID/HDMI), packet format fully RE'd, live-test cases 0+3 empty payload → `status=-3` |

## See also

- [EDID protocol](edid-protocol.md) — what stock libhalhdmi sends
- [HDMI subsystem](../subsystems/hdmi.md) — current state
- [cpu-comm protocol](cpu-comm-protocol.md) — msgbox plumbing
