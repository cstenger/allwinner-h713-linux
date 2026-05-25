# ge2d_dev.ko vs sunxi_ge2d / h713_drm / sunxi_decd

The most expensive lesson of the project: **stock uses `ge2d_dev.ko` for
HDMI-RX scanout, not `decd.ko`**. We spent ~5 sessions trying to drive
HDMI-RX through `decd`. This page documents what we found.

## The driver landscape

Stock Android has multiple modules touching the display pipeline:

| Stock module | Real role |
|---|---|
| `ge2d_dev.ko` | The real display driver. Plane mgmt, vsync IRQ, OSD init, LVDS FIFO control. ~36 functions, ~50 KB. |
| `decd.ko` | Video-playback decoder. AFBC-compressed input from ffmpeg/OMX. NOT used for HDMI-RX. |
| `sunxi_tvtop.ko` | Bus fabric + clock manager. Other modules register as clients. |
| `hidtvreg_dev.ko` | Universal MMIO accessor. RE-only, not needed for production. |

Our mainline port has:

| Mainline module | Status | Purpose |
|---|---|---|
| `sunxi_tvtop` | active | Port of stock tvtop, mostly clean |
| `sunxi_ge2d` | **blacklisted** | Full 3000-line port of `ge2d_dev.ko`. The real driver. Blacklisted due to DT-binding conflict. |
| `h713_drm` | active | Thin DRM/KMS shim with `mips_scanout_addr` override hack |
| `sunxi_decd` | loaded but inactive | Port of stock `decd.ko`. Has info-page-allocator fix. Dead code for HDMI-RX. |

## The DT-binding conflict

Both `h713_drm` and `sunxi_ge2d` claim `compatible = "trix,ge2d"`. The
DT-tree has node `5240000.ge2d` — only one driver can bind. Currently we
blacklist `sunxi_ge2d` — **wrong choice in retrospect**.

The next planned step is to swap the blacklist. See
[docs/subsystems/display.md](../subsystems/display.md).

## `ge2d_dev.ko` — the real stock driver

Symbol table reveals 36+ functions. Key ones:

| Symbol | Size | Function |
|---|---|---|
| `init_osd_plane.constprop.12` | 3472 B | OSD plane initial setup, register-stream driven |
| `tgd_put_plane_info` | 11064 B | main plane info handler (biggest function) |
| `tgd_set_checkboard_style` | 1768 B | test pattern |
| `tgd_set_vinterpolation` | 744 B | vertical interpolation (scaling/conversion) |
| `tgd_vblender_irq` | 1532 B | **vsync IRQ handler** |
| `tgd_config_vblender_irq` | 612 B | vsync IRQ setup |
| `tgd_init_planesetting` | 476 B | initial plane config |
| `tgd_is_plane_open` | 472 B | plane state query |
| `tgd_close_osd3` | 364 B | OSD plane close |
| `tgd_show_plane` | 324 B | plane visibility |
| `tgd_wait_for_vblender_interrupt` | 328 B | sync helper |
| `tgd_flip_plane` | 232 B | page-flip routine |
| `lvds_reset_fifo` | 384 B | LVDS FIFO control |
| `lvds_fifo_status` | 128 B | LVDS state query |
| `svp_set_cmap` | 2556 B | color mapping |
| `svp_get_cmap` | 668 B | color mapping query |
| `disable_fastlog_mode` | 180 B | log mode toggle |
| `free_fastlogo_func` | 220 B | logo cleanup |
| `ge2d_resume_operation` | 352 B | resume from suspend |
| `ge2d_suspend` | 528 B | suspend |
| `ge2d_resume` | 708 B | resume |

Plus a full set of tracepoints:
`frame_tracing`, `mux_tracing`, `hwreg_tracing`, `vsync_tracing`,
`ge2d_tracing`.

**Dependencies**: `vs_io_helper`, `sunxi_tvtop`, `backlight`. Registers
as `sunxi_tvtop` client via `sunxi_tvtop_client_register()`.

## Mainline `sunxi_ge2d` — what's already ported

`drivers/display/ge2d/` (about 3000 lines across 9 files):

- `sunxi_ge2d_core.c` — 16-step probe directly ported from `ge2d_drv_probe`
- `sunxi_ge2d_osd.c` — OSD plane init + interrupt handling
- `sunxi_ge2d_fbdev.c` — `/dev/fb0` framebuffer (1920×1080 ARGB8888)
- `sunxi_ge2d_svp.c` — color mapping + `/dev/ge2d` chardev
- `sunxi_ge2d_panel.c` — panel GPIO + LVDS watchdog
- `sunxi_ge2d_backlight.c` — backlight integration
- `sunxi_ge2d_dlpc3435.c` — DLPC3435 I2C helper
- `sunxi_ge2d_firmware.c` (1010 lines) — `LogoRegData.bin` parser +
  register-stream applier (this is where the OSD plane init magic
  happens)
- `sunxi_ge2d_dt.c` — DTS parsing

The 16-step probe order matches stock:

1. Parse DTS panel properties
2. Map MMIO (4 regions: OSD, LVDS, OSD_B, AFBD)
3. Enable clocks + reset
4. `/dev/ge2d` chardev
5. **OSD interrupt init** (programs vblender IRQ mask:
   `afbd2_base+0x168 = 0xFFFFFFFF`, `+0x16C = 0x10`)
6. Vsync timestamp init
7. OSD frame init (fence context)
8. Request IRQs (vblender + AFBD hardirqs)
9. Panel GPIO request
10. Backlight init
11. Framebuffer init (1920×1080 ARGB8888)
12. PM runtime
13. `sunxi_tvtop_client_register`
14. OSD resume init → delayed work loads `LogoRegData.bin`
15. LVDS watchdog thread
16. DLPC3435 init

## How we know decd is dead code for HDMI-RX

Multiple lines of evidence (sessions DD/DD-NIGHT/EE/autonomous):

1. **No FRAME_SUBMIT ioctl callers in stock userspace.** Searched all
   `.so` and `.bin` in stock for `0x40706400` (DECD_IOC_FRAME_SUBMIT)
   as 32-bit immediate. Result: zero matches outside `decd.ko` itself.
2. **`/dev/decd` is opened by `libmips.so` and `svp-suspend` only**, and
   both use it only for `MAP_LINEAR_BUFFER` (memory mapping), never
   `FRAME_SUBMIT`.
3. **Pool-1 fill via `decd_submit_test` works** (pool-1 byte-for-byte
   stock-conform after FRAME_SUBMIT) but visual stays **black** through
   every mode-bit permutation, magic constant fix, info-page collision
   fix, etc.
4. **The `decd.ko` codebase looks built for AFBC video frames**:
   info-page expects compressed-NV12 metadata, `dec_reg_video_channel_attr_config`
   has paths for "compressed" vs "raw" inputs, magic constant
   `0x61770000` checks per-frame info-page validity. None of this fits
   the raw NV12 streaming pattern that MIPS produces from HDMI-RX.

`decd` is for **video playback** (decoded H.264 / MPEG2 frames from
ffmpeg / OMX), not HDMI-RX scanout. The mainline port keeps it loaded
because it's harmless, but it's effectively unused.

## The mainline `decd` info-page fix

While debugging the "is decd the answer?" hypothesis, we did find a
**real bug** in the mainline port that's worth preserving:

```c
/* drivers/decd/decd_frame.c, mainline before fix: */
u32 video_info_buffer_init(struct dec_frame_submit_desc *desc)
{
    return lower_32_bits(desc->y_phys) + 4096;
}
```

This collides with the MIPS Y plane. With `y_phys = 0x4c3ef000`, the
info-page lands at `0x4c3f0000`, which is +4 KB inside the 1920×1080×1.5
= `0x1FA400`-byte Y buffer. MIPS overwrites the info-page with Y pixel
content → AFBD decompressor sees garbage → black.

**Fix** (deployed):

```c
u32 video_info_buffer_init(struct dec_frame_submit_desc *desc,
                           struct dec_frame_item *item)
{
    struct dec_video_info_page *page = alloc_video_info_page();
    if (!page || !item) return 0;
    item->video_info_page = page;
    return (u32)page->paddr;
}
```

This now matches the dma-buf path's allocator (using `decd_reserved`
pool at `0x4d941000` + 128 KB). Verified: info-page now lives at
`0x4d941000`, no collision.

Visual is still black because `decd` is the wrong driver — but the
mainline port is correct now.

## Magic constant correction

Mainline `decd.ko` validates info-page contents with magic `0x61766b40`
("@kva"). Stock magic is **`0x61770000`** ("..wa"). The RE engineer
who wrote the mainline port mis-read the constant from the disassembly.

Corrected by capstone-disassembling stock `decd.ko`:

```python
# At 0x4c1c in stock dec_reg_video_channel_attr_config:
#   ldr r3, [pc, #0x68]   ; load magic at 0x4c88
#   cmp r1, r3
#   beq #0x4c2c           ; skip pr_warn if matched
#
# Constant at 0x4c88: 0x61770000  (NOT 0x61766b40 — relocation table
# confirms no symbol fixup at this offset)
```

This is fixed in the source on the staging branch.

## `tgd_flip_plane` — what the real driver does per flip

Disassembly shows it builds a plane-config struct with register
addresses (one struct per plane = ch0 or ch1):

| Struct offset | plane=1 (ch1) | plane=0 (ch0) | Register |
|---|---|---|---|
| +0x24 | 0x05280080 | 0x05280040 | DE2 sub-block |
| +0x28 | 0x0524c000 | 0x05248000 | GE2D / OSD plane |
| +0x2c | 0x0529c000 | 0x05288000 | DE2 sub-block (unknown) |
| +0x30 | **0x05600140** | **0x05600100** | **AFBD ch1/ch0 ctrl** ✓ |
| +0x3c | 0x05200034 | 0x0520002c | VBlender |
| +0x40 | 0x051c006c | 0x051c0060 | LVDS sub-block |
| +0x44 | 0x051c019c | (same) | LVDS sub-block |
| +0x224 | plane_id | plane_id | flag |

**New register regions identified** (not in our `h713_drm` port):
- `0x05280000` / `0x05288000` / `0x05290000` — DE2 sub-blocks
- `0x0524c000` — GE2D region +48 KB (= OSD plane for ch1)
- LVDS sub-blocks at +0x60-+0x6c and +0x19c (our `h713_drm` only
  touches LVDS +0x10/+0x14/+0x24/+0x28)

So `h713_drm` is touching ~3 register regions; `ge2d_dev.ko` touches 7+.
That's the gap the `sunxi_ge2d` mainline port fills.

## `tgd_vblender_irq` — only 3 reads + 3 writes per vsync

Surprisingly small surface for the vsync IRQ handler:

```
0x627c: bl io_accessor_read_reg     ; read at base + 0x9c
0x62fc: bl io_accessor_read_reg     ; read at base + 0x18
0x637c: bl io_accessor_read_reg     ; read at base + 0x18
0x6558: bl io_accessor_write_reg    ; write at base + 0x18
0x658c: bl io_accessor_write_reg    ; write at base + 0x18
0x65c0: bl io_accessor_write_reg    ; write at base + 0x18
```

`base = ge2d_dev_struct->[0x1c]` (a global pointer set up in
`ge2d_drv_probe`). Plus 6 printk calls — all error handlers
(`read_reg(0x%08x) failed: %d` and `write_reg(0x%08x) failed: %d`).

Tiny attack surface to debug if the mainline port's vsync IRQ misfires.

## Path forward

1. Swap the blacklist (`h713_drm` out, `sunxi_ge2d` in).
2. Boot, check `/dev/fb0` appears (1920×1080 ARGB8888).
3. Test HDMI-RX → projection. If picture is still wrong, iterate on
   `ge2d_vblender_hardirq` register writes — only 3 + 3 surface.
4. Re-enable GPU + Wayland + Cedrus on top of working fbdev.

## See also

- [Display subsystem](../subsystems/display.md) — current state, path
  forward
- [Display bringup](display-bringup.md) — TVTOP root-cause history
- [SESSIONS.md](../../SESSIONS.md) sessions DD-EE + autonomous capstone
  for the decd dead-end narrative
