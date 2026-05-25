# Known issues

Living list. Sorted by impact. If you fix one, update or remove the entry.

## Blockers

### HDMI-RX picture is 4×1 grayscale tiling

**Symptom**: with `h713_drm` + `mips_scanout_addr=0x4c3ef000`, the HDMI input
shows up on the LVDS pipeline but tiled 4×1 horizontally, in grayscale.

**Cause**: channel-1 of the AFBD scanout is in XRGB (32 bpp) mode while
MIPS writes NV12 (12 bpp). Each row's 1920 NV12-Y bytes are interpreted as
480 XRGB pixels, then tiled 4× to fill the row. Chroma plane is ignored.

**Workaround**: none right now. The HY310 has no HDMI output port — the
DLP projector is the only display, and this is what it shows. SSH + UART
console still work fine for development.

**Real fix**: swap the DT-binding blacklist — make `sunxi_ge2d` active
instead of `h713_drm`. The mainline port at `drivers/display/ge2d/` has
the right architecture (vsync IRQ, OSD plane init via `LogoRegData.bin`,
`/dev/fb0` framebuffer). Cost: one reboot, decide what to do about the
DRM/KMS dependency for Wayland.

See [docs/subsystems/display.md](subsystems/display.md).

---

### GPU + Wayland desktop disabled

**Symptom**: no GPU acceleration, no Wayland compositor.

**Cause**: temporarily disabled to isolate the HDMI-RX path. `panfrost` was
aggressively using the display pipeline, which made register-state diffs
unreadable.

**Real fix**: re-enable after `sunxi_ge2d` is active and the HDMI-RX path
is clean.

---

### Video decoding (Cedrus) disabled

Same root cause + fix as GPU. `sunxi-cedrus` is built but not loaded.

---

## High-priority bugs

### IR remote NEC decoder unreliable

**Symptom**: LIRC raw timing is correct but the kernel NEC decoder
mismatches.

**Cause**: stock H713 IR driver uses 3 R_CCU clocks (bus/pclk/mclk) with
`clk_set_parent` that configures a different prescaler than our D1-based
R_CCU (2 clocks only). Resolution is ~25% off.

**Fix options**:
- Add H713-specific clock indices to the R_CCU driver.
- Implement the 3-clock setup in `sunxi-cir`.

**Workaround**: lircd with raw timing config.

---

### Msgbox TX reliability 40-60%

**Symptom**: ARM→ARISC sends sometimes don't get a response cb.

**Cause**: H713 Msgbox is edge-triggered, not level-triggered like H6.
Stock's pure-IRQ TX pattern (set sticky TX_IRQ_EN, wait for IRQ) has 0%
success on H713.

**Current workaround** (in `sunxi_msgbox_amp.c`): pulse-pattern. Prefill
`MSG_DATA`, pulse `TX_IRQ_EN`, `udelay(10)`, clear. Tagged as `WORKAROUND`.
Send-data delivery is 100%; only the RX-callback fires inconsistently
(40-60%).

**Real fix unknown**. Stock uses vendor `wakeupgen` IRQ-controller as a
routing layer. Porting wakeupgen is the obvious path, hasn't been done.

---

### HDMI-RX HPD plug/unplug not auto-detected

**Symptom**: IRQ 266 (`hdmi-rx@5000000`) is registered but `hits` count
stays 0 — plugging/unplugging an HDMI cable doesn't notify the kernel.

**Cause**: the HW interrupt source for HPD is somewhere else (ARISC writes
`0x07091014` on HPD events but doesn't fire a GIC IRQ).

**Workaround**: polling watcher (planned, not deployed).

**Practical consequence**: the HDMI source must be connected **before** power
-on. A source plugged in after boot is never detected.

---

### MIPS first boot fails — reboot required

**Symptom**: on a cold start the MIPS co-processor does not come up, so the
HDMI-RX / display path is dead.

**Workaround**: reboot once. If MIPS still doesn't come up, pull the **power
cable** — a soft reboot does not reset MIPS, only a full power cycle does.

**Cause**: not yet root-caused (first-boot init/timing). Tracked for a later
fix.

---

### EDID DMA-block write puzzle

**Symptom**: `DMA_CONFIG10/11` readback returns zero, but external laptops
read the EDID correctly (`SGD SX8 1920x1080p@60`).

**Cause**: mechanically not understood. The write must be going through
some path that isn't readable through the same register. Could be a
write-only port, could be a different memory plane.

**Workaround**: empirically works as-is.

---

## Medium-priority

### CMA pool too small for Weston

**Symptom**: `EGL_BAD_ALLOC` on `CREATE_DUMB` when Weston tries to
allocate buffers.

**Fix**: increase `cma=` boot parameter or clean up reserved-memory
overlaps.

---

### Address-translation cache-sync edge cases

**Symptom**: rare ACK-type message handling glitches.

**Detail**: `cpu_comm_sync_mips_cache` (DMB ISHST + spin-wait) is deployed
for CALL/RETURN handlers (session W) but ACK types still use the older
bit-check pattern. Stock does the DMB+wait for ACK types too — possible
edge case.

---

### Keystone motor limit switch defective

**Symptom**: PH14 always reads LOW. Homing sequence can't reliably detect
end-of-travel.

**Cause**: hardware (broken switch or wiring) on the test unit. May be
unit-specific.

**Workaround**: software step-count guard added to homing routine.

---

### Physical buttons (LRADC keys) non-functional

**Symptom**: stock DTS defines 6 LRADC key thresholds, but the LRADC
voltage stays at 63 (max) regardless of button presses.

**Cause**: unknown. Buttons may be wired to different GPIOs, may be on a
different ADC channel, or may not be connected on this board variant.

**Workaround**: IR remote works.

---

## Subsystems not started

| Subsystem | Notes |
|---|---|
| TVCAP / INCAP frame capture | Out-of-tree `sunxi_tvcap_rx.ko` skeleton exists but isn't reviewed against current state |
| HDMI audio | DTS nodes ready, no driver work yet |
| ARISC dispatcher 0x12490 | EDID/HDMI handlers all return `status=-3` ("imt error") — by design hollow, not useful for us |

## Things we know are wrong but accept

### `clk_ignore_unused` must stay in cmdline

Until the clock tree is fully described with all parent relationships,
removing this argument causes random clocks to be gated and peripherals
break. Long-term fix: complete CCU model. For now: required boot arg.

### `MSG_DATA`-only writes to user-region 1 are HW-blocked

Confirmed empirically. The doorbell pulse pattern works around it. Stock
uses level-triggered IRQs which avoid this, but we can't easily replicate
that without `wakeupgen`.

---

## Things that are NOT bugs (don't waste time)

These were tried and turned out to be red herrings. See
[SESSIONS.md](../SESSIONS.md) discarded-hypotheses table for the full
list. Highlights:

- The "obvious" HDMI-RX lock bits (`CMU_STATUS`, `PHY_STATUS`,
  `byte@0x06840001 bit 5`) are zero in stock too. They are not the gate.
- DTS edits to add the missing msgbox SPI 108/109 IRQs do nothing — mainline
  GIC doesn't route them. Stock uses `wakeupgen`.
- snps mainline PHY-init pattern times out on H713 (15/15 register writes).
  H713 PHY is not snps-PHYCREG-compatible.
- `decd.ko` is dead code for HDMI-RX. Stock uses `ge2d_dev.ko`. Five
  sessions confirmed this.
- MIPS firmware is not internally broken. Stock Android shows picture on
  the same HW with the same firmware — the problem is always on the
  ARM/Linux side.
