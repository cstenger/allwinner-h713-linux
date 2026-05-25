# Desktop configuration (optional)

> **Currently not active in the alpha build.** The GPU + Wayland
> compositor are temporarily disabled while the HDMI-RX picture pipeline
> is being debugged. These configs are kept here for when we re-enable
> the desktop.

These files provide a working Wayland desktop environment that previously
ran on the HY310. The plan is to re-enable it once `sunxi_ge2d` is the
active display driver and the LVDS/DLP picture path is clean.

## What's included

- `labwc.service` — systemd service for autostarting the Labwc compositor
- `labwc/` — Labwc window manager config (keybinds, menu, autostart)
- `waybar/` — status bar configuration and styling

## Required packages

```sh
apt install labwc waybar wofi thunar foot swaybg xwayland dbus-x11 wlr-randr
```

## Installation (for when GPU is re-enabled)

```sh
# Copy labwc service
cp labwc.service /etc/systemd/system/
systemctl enable labwc

# Copy user configs
mkdir -p ~/.config/labwc ~/.config/waybar
cp labwc/* ~/.config/labwc/
cp waybar/* ~/.config/waybar/

# Make sure the DRM module loads at boot
cp ../modules-load.d/h713_drm.conf /etc/modules-load.d/
```

## Current state — read first

In the current alpha build, the GPU (`panfrost`), video decoder
(`sunxi-cedrus`), and Wayland compositor are all **disabled**. Reasons:

1. The HDMI-RX picture path is being debugged. The GPU was aggressively
   using the display pipeline and made register-state diffs unreadable.
2. The current display driver (`h713_drm`) is a thin shim, not a real
   DRM/KMS driver. Wayland on top of it works but isn't useful while
   the underlying picture is broken.
3. CMA pool size needs revisiting before Wayland buffer allocation
   succeeds reliably (`EGL_BAD_ALLOC` on `CREATE_DUMB`).

When the LVDS picture works cleanly with `sunxi_ge2d`, we'll re-enable
GPU + Wayland and revisit these configs.

## For now: console + fbterm

If you want a graphical-ish environment without compositor:

```sh
apt install fbterm
fbterm
```

The HY310 has no HDMI output port (only HDMI input), so external-monitor
fallback isn't an option either. The DLP projector currently shows the
broken HDMI-RX content (4×1 grayscale tiling). SSH + UART are the
practical dev paths until the picture is fixed.

## See also

- [STATUS.md](../../STATUS.md) — what's currently enabled/disabled
- [docs/subsystems/display.md](../../docs/subsystems/display.md) — what's
  broken and what's next
