# cpu_comm — ARM↔MIPS IPC

**Status**: ✅ working, full bidirectional. The big IPC milestone in 2026.

The H713 has a MIPS coprocessor that owns all HDMI/display logic. ARM talks
to it via `cpu_comm`, a custom RPC-over-shared-memory protocol.

## Quick test

```sh
ssh hy310 '/usr/local/bin/test_brightness_call'
# Expect: brightness call ENTER/LEAVE, ~20-90 ms roundtrip
```

If that works, the IPC is alive.

## Transport

- **Shared memory**: 5 MB at ARM-phys `0x4E300000` (MIPS-VA `0xAE300000`)
- **Msgbox**: HW mailbox at `0x03003000`, user region 2 for MIPS peer
- **HW spinlocks**: `0x03004000`, sun6i-style

Address translation between ARM and MIPS:
- code/data: `phys = MIPS-VA - 0x40000000`
- shared mem: `phys = MIPS-VA - 0x60000000`

The mainline driver has full translation helpers
(`Vir2Mid`/`Mid2Vir`/`Mid2Phy`/`Phy2Mid`) since session W.

## Protocol — 168-byte CALL packet

| Offset | Field | Notes |
|---|---|---|
| 2–3 | `dst_cpu` u16 | 1 = MIPS |
| 6–7 | `flags` u16 | |
| 8–9 | `msg_type` u16 | 0=CALL, 1=RETURN, 2=CALL_ACK, 3=RETURN_ACK |
| 12–15 | `session_id` u32 | |
| 40–43 | `comp_id` u32 | **caller MUST set** — see hash below |
| 64 | `param_count` u32 | **caller MUST set** — silent no-op if missing (session Q bug) |
| 68/72/… | param values u32 | |
| 120–159 | result / return values | |

## Routine name → comp_id (Allwinner CRC32)

```python
def crc32(name, seed=0x123456):
    crc = seed
    for b in name.encode():
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc & 0xFFFFFFFF

# Name format: "<base>_<cpu_id>_<pid_low12>"
# Example: "THal_Vp_SetBrightness_1_000" → 0x7221d017
```

Known routine hashes (subset):

| Routine | comp_id | Purpose |
|---|---|---|
| `THal_Vp_SetBrightness_1_000` | `0x7221d017` | PQ |
| `THal_Vp_SetSource_1_000` | `0xeaf13de5` | source selection |
| `THal_Vp_HDMI_SetPortMap_1_000` | `0x9ce74c48` | HPD-gate-flag activator |
| `THal_Vp_HDMI_GetPortStatus_1_000` | `0xcbf83247` | port status query |
| `THal_Vp_Init_1_000` | `0x1c6ff747` | init (**Para[2] != 0 required!**) |
| `MipsHalCallback_SignalChange_0_000` | `0x3e7fbc46` | MIPS→ARM signal change |
| `MipsHalCallback_HdmiHotPlugByPortHandler_0_000` | `0x38d780e2` | MIPS→ARM HPD |

Full list: about 80-85 routines registered at MIPS boot. See
[docs/re/cpu-comm-protocol.md](../re/cpu-comm-protocol.md).

## Kernel module

**Source**: `drivers/soc/sunxi/cpu_comm/` (8 source files) — built **in-tree** as
a module (`CONFIG_HY310_CPU_COMM=m`), added by
`patches/0024-soc-sunxi-add-cpu-comm-and-msgbox-ipc.patch`. Its mailbox RPMSG
transport `drivers/soc/sunxi/msgbox/` (`SUNXI_MSGBOX_AMP=y`) ships in the same
patch.
**Output**: `hy310-cpu-comm.ko`
**DTS**: `cpu-comm` with `compatible = "allwinner,sunxi-cpu-comm", "trix,cpu_comm"`

### IOCTLs

| Code | Name | Purpose |
|---|---|---|
| `0xC0087F26` | `IOCTL_CALL` | sync 168-byte msg |
| `0xC0087F30` | `INSTALL_RT` | register a routine (96-byte descriptor) |
| `0xC0087F10` | `MALLOC` | shmem alloc |
| `0x40047F11` | `FREE` | shmem free |
| `0x80087F34` | `GET_CPUID` | |

### Userspace callback delivery (session DD addition)

Mainline driver now exposes:
- `.read(/dev/cpu_comm)` — blocks until a callback packet is queued
- `.poll(/dev/cpu_comm)` — for select/epoll
- per-fd ringbuffer
- `cpu_comm_userspace_deliver()` hook in both `comm_CallWorkAction` and
  `cpu_comm_handle_CPU2_call` paths (the second one catches `entry_cmd≤4`
  routes that bypass the workqueue)

Plus the `IOCTL_INSTALL_RT` fix from session DD: when a routine is
registered for `target_cpu=0` (ARM-side impl), the driver makes a second
channel-registration with `comp_id=1, cpu=current->pid` so the channel-id
lookup for MIPS-initiated calls works.

This is what makes `hy310-hdmird` callback-driven instead of timeout-fallback.

## SharedMem boot timing

`sunxi-mipsloader` is built-in (`CONFIG_SUNXI_MIPSLOADER=y`). At T+0.9s it
writes the SharedMem base address into the CCU share registers. MIPS reads
the address, initializes `cpu_comm`, and reaches `APP_READY=0x5`
autonomously — no ARM polling or handshake nudging required.

**Critical**: the CCU share register offset is `0x60c`, not `0x604`. The
older mainline port had the wrong offset and MIPS never saw the address.
Fixed.

## Major bug history

These were all on the way to "working":

| Session | Fix |
|---|---|
| H | `INTR_TYPE_SEND=2` was being interpreted by MIPS as CALL_ACK. Fix: use `MSG_TYPE_CALL=0` for forward CALL. |
| I | HISR worker waiter-pointer offset was wrong (`queue+36`, not the initial guess). |
| J | `comm_SpinLocksetType(2,1)` was the wrong sequence — removed, lock-2 now initializes cleanly, all 81 routines register in 34 ms. |
| K | First full IPC roundtrip with 8 sub-fixes (RX@User1+0x100, bit-layout parsing, ack/command_action bit-2 check inverted, etc.). |
| K-night | FreeCall FIFO overflow fixed by removing a cache-hack and using fresh `Comm_GetFreeCall` per send. |
| Q | `params[0]=count` packet-format bug — wrong offset → silent no-op. Fixed. |
| W | Three more mainline bugs fixed via stock `cpu_comm_dev.ko` RE: address-translation, `GetReturnbySessionId` semantic, DMB ISHST cache-sync pattern. |
| Z | Root cause of MIPS state-3 stuck: `Vp_Init` called with `Para[2]=0` caused NULL memcpy on MIPS → MMU fault → cascading kernel-MM corruption. Fix: `Para[2] = SHMEM_PHYS_BASE + 4 MB`. |
| DD | Userspace callback delivery patch (`.read`/`.poll` + channel-id fix). |

## Reliability notes

- **Msgbox TX is edge-triggered** on H713 (not level like H6). Stock's
  pure-IRQ pattern has 0% success. Workaround: pulse-doorbell, 40-60%
  RX-cb-fire. Send-data delivery is 100%.
- **MIPS cache sync**: `cpu_comm_sync_mips_cache` (DMB ISHST + spin-wait)
  is in CALL/RETURN handlers. ACK types still use the older bit-check
  pattern — possible edge-case (low priority).

## Userspace clients

`hy310-hdmird` is the main client. See [`userspace/hy310-hdmird/src/`](../../userspace/hy310-hdmird/src/).
Architecture: open `/dev/cpu_comm`, mmap 5 MB shmem, load HDCP22 key
into shmem, register 9× `MipsHalCallback_*` routines for ARM-side callback
delivery, run 12-call init + 12-call source-switch sequence.

## Debugging

```sh
# MIPS state machine:
ssh hy310 'cat /sys/class/hy300/mips/state_machine'   # expect 5 (Running)

# MIPS firmware log (ring buffer):
ssh hy310 'python3 /root/mips_elog2.py | tail -40'

# IPC test routines:
ssh hy310 '/usr/local/bin/test_brightness_call'
ssh hy310 'python3 /usr/local/bin/test_mips_call.py'
ssh hy310 '/usr/local/bin/test_all_routines'

# Module state:
ssh hy310 'lsmod | grep cpu_comm'
ssh hy310 'cat /proc/cpu_comm/RPC_calls 2>/dev/null'  # on stock-android only
```

## Open issues

- Msgbox TX reliability (40-60%). Real fix would be porting `wakeupgen`.
- ACK-type cache-sync edge case (low priority).

## See also

- [Architecture overview](../architecture.md) — three-CPU big picture
- [MIPS subsystem](mips.md) — firmware loading + state machine
- [docs/re/cpu-comm-protocol.md](../re/cpu-comm-protocol.md) — protocol deep dive
