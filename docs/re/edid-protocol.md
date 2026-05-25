# EDID / HPD protocol — ARM ↔ ARISC

Reverse-engineered from stock `libvideo.so` (CHdmiEdidCec + CDeviceControl
wrapper) and `libhalhdmi.so` (Thal_HDMI_* impl with byte-level sub-command
codes).

Stock source path (from debug strings):
`vendor/aw/homlet/tvsystem/hal/bsp/HAL_SX6/HDMI/Src/thal_hdmi_api.c`

## The stack

```
Application (e.g. hy310-edid-setup-tool)
        |
        v
hy310_arisc.ko ioctl SEND/RECV via /dev/hy310_arisc
        |
        v
sunxi_arisc_rpm.ko (rpmsg API)
        |
        v
sunxi_msgbox_amp.ko (MMIO)
        |
        v
ARISC firmware (OR1K BE, 172 KB)
   - HPD pin driving (0x07091014)
   - EDID storage (theoretically — see below)
   - MIPS ←→ HDMI-RX HW
```

Stock equivalent:

```
tvserver → libvideo.so::CHdmiEdidCec::thal_send_Data
        → ioctl(/dev/mcu_comm, 0xC0845F01, *_tagMcuCommParam)
        → mcu_comm_dev.ko (kernel char-dev)
        → sunxi_arisc_rpm_send(type=0, length, data)
```

Stock kernel hardcodes BOP `type=0` for all outbound calls. Sub-command
comes from `payload[0..1]` (LE u16). Stock userspace knows 9 EDID
sub-commands + 30+ CEC cases.

## SEND — `_tagMcuCommParam` struct (132 bytes)

```c
struct _tagMcuCommParam {
    uint32_t length;     /* [0..3]   payload length, becomes BOP[3] */
    uint16_t sub_cmd;    /* [4..5]   LE u16 sub-command id */
    uint8_t  arg1;       /* [6]      command-specific (port/version) */
    uint8_t  arg2;       /* [7]      command-specific (mode/flag) */
    uint8_t  data[124];  /* [8..131] payload data */
};
```

Userspace builds this on the stack, calls
`ioctl(fd, 0xC0845F01, &param)`.

Kernel composes BOP frame to ARISC:

```
BOP[0]   = 0xA5           (marker)
BOP[1]   = seq            (rotating, kernel-managed)
BOP[2]   = 0              (hardcoded — generic TV control case)
BOP[3]   = param.length   (= 2..67 typical)
BOP[4..7]= pad
BOP[8..] = sub_cmd_lo, sub_cmd_hi, arg1, arg2, data...
```

## Sub-command encoding

```
sub_cmd = (op_class << 8) | op_kind

op_kind:
  0x11 = SET / ACTION    (write, reset, audio-mode, hot-plug, port-remap)
  0x15 = QUERY / INQUIRE (read, status-check, request-EDID)

op_class:
  0x00 = port routing            (PortRemap)
  0x01 = port-info / EDID-write  (RequestHDMIPortNumber, UpdateEDID)
  0x02 = hot-plug / EDID-status  (PullHotPlug, CheckEDIDUpdateStatus)
  0x03 = EDID configure          (SetEDIDVersion, RequestEDID)
  0x04 = audio + 5V              (SET5VFlag, SetEDIDAudioMode)
  0x05 = audio mode              (SetEDIDAudioMode high bits)
  0x20 = module-level            (ResetEDIDModule)
```

## EDID sub-commands

| Function | sub_cmd | length | arg1 | arg2 | data | Notes |
|---|---|---|---|---|---|---|
| RequestEDID | `0x0315` | 3 | port | — | — | "Inquire HDMI-N EDID" |
| UpdateEDID (4× chunks) | `0x0111` | 67 | `(4·blk)\|frag` | — | 64 B EDID chunk | 4 calls per 256 B EDID |
| CheckEDIDUpdateStatus | `0x0215` | 2 | — | — | — | "Inquire EDID Status" |
| SetEDIDVersion | `0x0311` | 3 | version | — | — | "HDMIs EDID version = X" |
| SetEDIDAudioMode | `0x0511` | 4 | port | mode | — | `_tagEDID_DYNAMIC_MODE` |
| ResetEDIDModule | `0x2011` | 2 | — | — | — | reset all EDID state |
| RequestHDMIPortNumber | `0x0115` | 2 | — | — | — | "Inquire HDMI port number" |
| SET5VFlag | `0x0411` | 4 | port | flag | — | "HDMI-N, 5V status = X" |
| PullHotPlug | `0x0211` | 4 | port | active? | — | force HPD assert/deassert |
| PortRemap | `0x0011` | 5 | data[0] | data[1..2] | — | port routing table |

## UpdateEDID detail (256 B → 4 BOP frames)

```c
Thal_HDMI_UpdateEDID(uint8_t *edid_256B, uint8_t block_id);
```

Sends 4 separate BOP frames back-to-back:

```
Call 1: sub_cmd=0x0111, length=67, arg1=(block<<2)|0, data=edid[0..63]
Call 2: sub_cmd=0x0111, length=67, arg1=(block<<2)|1, data=edid[64..127]
Call 3: sub_cmd=0x0111, length=67, arg1=(block<<2)|2, data=edid[128..191]
Call 4: sub_cmd=0x0111, length=67, arg1=(block<<2)|3, data=edid[192..255]
```

`block_id` selects which EDID block (for 2-block EDID: 0 = base,
1 = extension). Lower 2 bits of `arg1` = fragment index 0..3.

No inter-frame handshake — fire all four, then poll
`CheckEDIDUpdateStatus`.

## Receive layer (mcu_comm + cecmsgprocess)

### Kernel layer (mcu_comm_dev → userspace)

`read(fd, buf, 72)` returns an assembled message from the Recv queue:

```
buf[0]  = 0xff   (RX marker, kernel-synthesized per BOP-strip)
buf[1]  = channel byte:
          0xf7 = CEC
          0xf8 = HDMI / EDID
buf[2]  = sub-case
buf[3]  = data length
buf[4..]= data
```

Stock thread filters `buf[0] == 0xff`, otherwise skips. Multi-message
read is possible: two messages can come in one `read(72)` — splitter
looks for second `0xff` with valid sub-byte.

### Userspace layer

```c
switch (buf[1]) {
    case 0xF7:  /* CEC */
        if (buf[2] == 0xFC && buf[3] == 3) {
            /* CEC ACK from sender — buf[4..6] = header, opcode, ACK */
            push_ack_to_queue();
        } else {
            Thal_HDMI_CEC_ReceiveHandler(&buf[2]);
        }
        break;
    case 0xF8:  /* HDMI/EDID */
        Thal_HDMI_ReceiveHandler(&buf[2]);
        /* receiver sees: arg[0]=sub-case, arg[1]=length, arg[2..]=data */
        break;
    default:
        /* "Received unknown message" log */
}
```

### `Thal_HDMI_ReceiveHandler` sub-cases

| sub_case | meaning | data layout |
|---|---|---|
| 1 | EDID-Ready notification | `data[0]=1` → "Edid Data Ready", else "Not Ready" |
| 2 | EDID-Package | `data[0]` = page index, `data[1..64]` = 64 B EDID chunk |
| 3 | HDMI-Ports info | "There are HDMI Ports" (port count?) |
| other | unknown — "Unknown HDMI Message" log |

256 B EDID readback comes back as 4× `sub_case=2` packets (page 0..3).

## The DMA-write puzzle

What we actually use for EDID write is the Synopsys `DMA_CONFIG10/11`
direct path, not the ARISC sub-command flow. The ARISC EDID handlers
(dispatcher at `0x12490` in ARISC firmware) all return `status=-3`
("imt error") — hollow by design. See [arisc-firmware.md](arisc-firmware.md).

The DMA path:

1. Enable `WRITE_EN` + slave 0x50 in the Synopsys HDMI-RX EDID-RAM block.
2. Byte-stream the EDID to `DMA_CONFIG10`.
3. Readback `DMA_CONFIG10/11` returns **zero**, but external laptops read
   the EDID correctly (`SGD SX8 1920x1080p@60`).

The readback mystery is unresolved. Possibly:

- A write-only port + a separate read-only mirror we don't have the
  address of
- A different memory plane that the read doesn't reach
- Memory-mapped peripheral with side-effect-on-read semantics

Works empirically. Treat with respect.

## HPD pin

Address `0x07091014`, 3-bit register, undocumented.

Port N controls bit N. From ARISC firmware HPD handler at `0x121e4`:

```
write 0x00 = port LOW (no HPD assert)
write 0x07 = all 3 ports HIGH (HPD asserted)
```

Sequence used by mainline driver (session P v7):

1. `controller_enable` writes `0x00` early at T+0.9 s
2. `delayed_work` callback writes `0x07` at T+9 s after EDID loaded
3. The 8-second sustained LOW + LOW→HIGH edge forces the laptop to
   re-read EDID

Plug/unplug auto-detection doesn't work — IRQ 266 never fires (the
real HPD interrupt source is somewhere else; ARISC writes `0x07091014`
but doesn't fire a GIC IRQ).

## See also

- [HDMI subsystem](../subsystems/hdmi.md) — usage side
- [ARISC firmware](arisc-firmware.md) — what ARISC actually handles
- [Display bringup](display-bringup.md) — TVTOP root cause history
