# cpu_comm protocol — deep dive

For the subsystem overview and "how to use", see
[docs/subsystems/cpu-comm.md](../subsystems/cpu-comm.md). This page is for
people working on the protocol itself.

## Bus layout

```
+----------+                     +----------+
|   ARM    |  168-byte CALL pkt  |  MIPS    |
| user pid |  via shared memory  |  thread  |
+----------+                     +----------+
     |                                |
     |   Msgbox notify (edge)         |
     |   ARM→User2.sub-block(0)       |
     |   port 1 MSG_DATA = type-byte  |
     |--------------------------------|
     |                                |
     |   MIPS handles + ACKs          |
     |   ACK type 2 → ARM             |
     |   RETURN type 1 → ARM          |
     |   via User0.port 1             |
     |                                |
     v                                v
  shared memory regions for caller/callee data
```

## Shared memory layout

5 MB at ARM-phys `0x4E300000` (= MIPS-VA `0xAE300000`). Sub-regions:

| Offset | Size | Purpose |
|---|---|---|
| `+0x00000` | 1 MB | session pool, FreeCall pool, returnPipeLine |
| `+0x36000` | 960 B | HDCP22 key (set by `hy310-hdmird`) |
| `+0x40000` | various | TSE picture-quality buffers |
| `+0x400000` | various | factory-PQ memcpy target (Vp_Init Para[2] points here) |

The exact offsets are derived from stock cpu_comm_dev.ko + libvideo.so RE.

## CALL packet — 168 bytes

```
0                  8                 16                24
+--------+---------+--------+--------+--------+--------+--------+--------+
| rsv0       | dst | rsv1   | flags  | msg_type     | session_id        | 0..15
+--------+---------+--------+--------+--------+--------+--------+--------+
| dst_pid (u32)             | rsv2 (u32)        | rsv3 (u32)            | 16..31
+--------+---------+--------+--------+--------+--------+--------+--------+
| ... reserved/internal ...                                              | 32..39
+--------+---------+--------+--------+--------+--------+--------+--------+
| comp_id (u32)             | rsv (u32)         | rsv (u32) | rsv (u32) | 40..55
+--------+---------+--------+--------+--------+--------+--------+--------+
| rsv (u32) | rsv (u32)     | param_count (u32) | param[1] (u32)        | 56..71
+--------+---------+--------+--------+--------+--------+--------+--------+
| param[2] (u32)            | ... up to param[12]                       | 72..119
+--------+---------+--------+--------+--------+--------+--------+--------+
| return values (40 bytes)                                              | 120..159
+--------+---------+--------+--------+--------+--------+--------+--------+
| trailer (8 bytes)                                                     | 160..167
+--------+---------+--------+--------+--------+--------+--------+--------+
```

**Caller MUST set** (otherwise silent no-op — session Q bug):

- `msg[2..3]` `dst_cpu` u16 — 1 = MIPS, 0 = ARM
- `msg[40..43]` `comp_id` u32 — see hash function below
- `msg[64]` `params[0]` = parameter count u32
- `msg[68..]` parameter values u32 each

Optional:
- `msg[8..9]` `msg_type` — 0=CALL (default), 1=RETURN, 2=CALL_ACK, 3=RETURN_ACK
- `msg[12..15]` `session_id` — caller-defined, MIPS returns the same ID

## Name → comp_id (Allwinner CRC32 variant)

```python
def crc32(name, seed=0x123456):
    crc = seed
    for b in name.encode():
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc & 0xFFFFFFFF

# Name format: "<base>_<cpu_id>_<pid_low12>"
# cpu_id: hex digit (0 for ARM-side, 1 for MIPS-side)
# pid_low12: 3-digit hex of (pid & 0xFFF)
# 
# Example: "THal_Vp_SetBrightness_1_000" → 0x7221d017
```

## Known routine hashes

ARM→MIPS (call into MIPS):

| Routine | comp_id | Purpose |
|---|---|---|
| `THal_Vp_SetBrightness_1_000` | `0x7221d017` | PQ |
| `THal_Vp_SetSource_1_000` | `0xeaf13de5` | source selection |
| `THal_Vp_HDMI_SetPortMap_1_000` | `0x9ce74c48` | HPD-gate-flag activator |
| `THal_Vp_HDMI_GetPortStatus_1_000` | `0xcbf83247` | port status query |
| `THal_Vp_HDMI_ReloadHdcp14Key_1_000` | `0x449effe8` | HDCP key reload |
| `THal_Vp_HDMI_SetHPDTimeInterval_1_000` | `0x6eb4c96a` | HPD timing |
| `THal_Vp_Init_1_000` | `0x1c6ff747` | init (**Para[2] != 0 required!**) |
| `THal_Vp_RegisterSignalChangeCallback` | `0x671ceca6` | (source_id, callback_comp_id) |
| `THal_Vp_SetHDMIHotPlugByPortCallback` | `0xba3e5a70` | (callback_comp_id) |
| `Thal_Vp_SetBacklightPwmInfo` | `0xb46ce545` | 4 args, undocumented |

MIPS→ARM (callbacks from MIPS):

| Routine | comp_id | Purpose |
|---|---|---|
| `MipsHalCallback_SignalChange_0_000` | `0x3e7fbc46` | (state) — state=3 means new signal valid |
| `MipsHalCallback_HdmiHotPlugByPortHandler_0_000` | `0x38d780e2` | (port) |
| `MipsHalCallback_HdmiSignalValidCallback_0_000` | `0x819cd918` | — |
| `MipsHalCallback_OnNewSPDPacket_0_000` | `0xf0b55406` | — |
| `MipsHalCallback_OnNewGCPacket_0_000` | `0x1727f0d0` | — |
| `MipsHalCallback_OnNewAVIPacket_0_000` | `0xa3bf5110` | — |
| `MipsHalCallback_OnNewACRPacket_0_000` | `0x12601a68` | — |
| `MipsHalCallback_DisplayLatencyChange_0_000` | `0xad9f0a86` | — |
| `MipsHalCallback_OnNewAudioInfoPacket_0_000` | `0xff9b970b` | — |
| `MipsHalCallback_OnNewVSIPacket_0_000` | `0xf87faf28` | — |

There are about 80–85 routines total registered by MIPS at boot via
`hal_adapter_init`. Full list can be extracted by running
`/usr/local/bin/test_all_routines` on the device.

## Msgbox layout (H713)

3× user regions at `0x03003000`, each 0x400. Sub-blocks per remote peer
(256 B each):

| Offset | Register | Purpose |
|---|---|---|
| +0x10 | Version | read-only, `0x00020000` |
| +0x20 | RX_IRQ_EN | `BIT(2*port)` |
| +0x24 | RX_IRQ_STAT | W1C |
| +0x30 | TX_IRQ_EN | `BIT(2*port + 1)` |
| +0x34 | TX_IRQ_STAT | W1C |
| +0x60 | FIFO_COUNT | `+4*port` |
| +0x70 | MSG_DATA | `+4*port`, auto-pop on read, push on write |
| +0x80 | TX_INIT | `+4*port` |

**Write-protection map** (empirical):

| Register | User0 (ARM-region) | User1 (ARISC-region) | User2 (MIPS-region) |
|---|---|---|---|
| `RX_IRQ_EN` +0x20 | ARM W ✓ | blocked | blocked |
| `TX_IRQ_EN` +0x30 | blocked | ARM W ✓ | ARM W ✓ |
| `MSG_DATA` +0x70 | RW ✓ | RW ✓ | RW ✓ |

DTS amp-mapping (stock): `cpus=1 (ARISC), mips=2`.

`adj` formula (for TX dst): `adj(local, remote) = (local < remote) ? remote-1 : remote`.

TX addresses (verified):

| Direction | Path | Address |
|---|---|---|
| ARM→MIPS CALL | User2 sub-block 0 port 1 MSG_DATA | `0x03003874` |
| MIPS→ARM (TX) | User0 port 1 MSG_DATA | `0x03003174` |
| MIPS→ARISC HPD | User1 sub-block 1 port 1 | `0x03003574` |
| ARM→ARISC | User1 sub-block 0 port 3 MSG_DATA + TX_IRQ_EN doorbell pulse | `0x0300347c` + `0x03003430` |

## H713 vs H6 — edge vs level

H6 Msgbox is level-triggered. H713 is **edge-triggered**. Stock's pure-IRQ
TX pattern (`set sticky TX_IRQ_EN, wait IRQ`) has **0% success rate** on
H713.

Current workaround (in `sunxi_msgbox_amp.c`):

```c
// ARM→ARISC pulse-doorbell pattern:
writel(msg_data, MSG_DATA_addr);   // prefill
writel(BIT(7), TX_IRQ_EN_addr);    // pulse
udelay(10);
writel(0, TX_IRQ_EN_addr);         // clear
```

Send-data delivery is 100%. RX-callback fire is 40–60% (HW race). Tagged
as `WORKAROUND` in the source. Real fix would be porting stock vendor
`wakeupgen` IRQ-controller (the routing layer that makes H6-style
level-triggered TX work on H713).

## Stock cpu_comm_dev.ko functions (IDA-named)

| Address | Name | Purpose |
|---|---|---|
| `0xc4dc-0xc524` | `Vir2Mid`, `Mid2Vir`, `Mid2Phy`, `Phy2Mid`, `Trid_SMM_transAddr`, `Tird_SMM_VirtoPhysAddr`, `Tird_SMM_PhystoVirAddr` | address translations |
| `0xcc94` | `cpu_comm_handle_CPU2_return` | IRQ handler for MIPS→ARM RETURN — pattern: LDRB+TST bit 4 → AND ~bit4 → STRB → DMB ISHST → spin-LDRB+TST until MIPS cache-flush completes → `queueAction` |

Flag offsets in shared memory:
- CALL/RETURN: `+8`
- ACK/RETURN_ACK: `+105`

## MIPS firmware functions (IDA-named in display.bin.i64)

See [display-bin.md](display-bin.md) for the full list. Key entries for
cpu_comm:

| VA | Name | Purpose |
|---|---|---|
| `0x8B12156C` | `msgbox_IRQ_handler` | Counter target @ `0xAE340000` |
| `0x8B12254C` | `cpu_comm_msg_cb` | switch on `comm_type` |
| `0x8B11EF78` | `cpu_comm_handle_CPU2_call` | check `share_seq[8] & 4` |
| `0x8B11F234` | `cpu_comm_handle_CPU2_return` | RETURN handler |
| `0x8B11F4F0` | `cpu_comm_handle_CPU2_callACK` | check `share_seq[105] & 4` |
| `0x8B11F6D8` | `cpu_comm_handle_CPU2_returnACK` | — |
| `0x8B11D418` | `Comm_Add2NewCallFifo` | check FIFO + signal `_GotCallSem` |
| `0x8B118B84` | `Comm_ReleaseFreeCall` | dequeue from FreeCall pool |
| `0x8B11E8AC` | `DoIntr2CPU2` | sends type-byte to ARM via `send_u32_to_arm_endpoint` |
| `0x8B121CB0` | `send_u32_to_arm_endpoint` | — |
| `0x8B121870` | `arisc_endpoint_send` | poll FIFO_STAT + write |
| `0x8B17FC70` | `readl_checked` | MMIO read helper (KSEG1 transform) |
| `0x8B17FD10` | (MMIO write helper) | counter target (Session X) |
| `0x8B10AD78` | `hal_adapter_init` | 81–85 routines registered |
| `0x8B1247EC` | `InstallRoutine` | name-format + hash |

## Debugging

```sh
# MIPS firmware log (Mode 1 ringbuffer):
ssh hy310 'cat /sys/class/hy300/mips/elog_full | tail -50'

# Mode 2 linear buffer:
ssh hy310 'python3 /root/mips_elog2.py | tail -50'

# Force a CALL + watch for RETURN:
ssh hy310 '/usr/local/bin/test_brightness_call'
# Expect: brightness call ENTER/LEAVE, ~20–90 ms roundtrip
```

## See also

- [cpu-comm subsystem](../subsystems/cpu-comm.md) — usage-side overview
- [MIPS subsystem](../subsystems/mips.md) — firmware loading + state
- [display-bin.md](display-bin.md) — full MIPS firmware RE
