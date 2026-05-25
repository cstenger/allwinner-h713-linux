# Building the HY310 Kernel

## Prerequisites

- ARM cross-compiler: `arm-linux-gnueabi-gcc`
  (Debian: `apt install gcc-arm-linux-gnueabi`)
- Standard kernel build tools: `make`, `bc`, `flex`, `bison`, `libssl-dev`
- Python 3 for `repack_boot.py`

## Important build settings

- **MIPS IPC**: `CONFIG_SUNXI_MIPSLOADER=y` must be set (built-in, not module).
  Without it, MIPS never receives the SharedMem address and cannot reach
  APP_READY.
- **Cross-compile**: `ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-`.
- **Memory-constrained build hosts**: pass `-j2` (the script uses `JOBS=2` by
  default).

## Two build paths

### A. From this repo (recommended)

The kernel source tree already contains all HY310 changes. You don't need to
apply patches.

```sh
cd hy310-linux
export KDIR=/path/to/linux-6.16.7   # vanilla kernel tarball expanded here
./scripts/build_kernel_arm32.sh
```

### B. Apply patches to a fresh vanilla tree

If you have a clean `linux-6.16.7` and want to apply the HY310 patches
manually:

```sh
cd linux-6.16.7
for p in $(cat /path/to/hy310-linux/patches/series); do
    patch -p1 < /path/to/hy310-linux/patches/$p
done
```

The patch set (21 patches, listed in `patches/series`):

| Patch | Type | What it adds / changes |
|---|---|---|
| `0001-clk-sunxi-ng-add-h713-ccu-driver.patch` | new | H713 CCU driver (1149 LOC) |
| `0002-pinctrl-sunxi-add-h713-pio-driver.patch` | new | H713 main pinctrl (749 LOC) |
| `0003-pinctrl-sunxi-add-h713-r-pio-driver.patch` | new | H713 R-pinctrl (203 LOC) with NEW_REG_LAYOUT |
| `0004-pinctrl-sunxi-fix-irq-mux-and-graceful-resource.patch` | edit | sunxi pinctrl: IRQ mux + graceful resource |
| `0005-phy-sun4i-usb-add-h713-pmu-bit0-quirk.patch` | edit | USB PHY: PMU bit-0 quirk |
| `0006-mmc-sunxi-add-h713-v5p3x-support.patch` | edit | sunxi-mmc: v5p3x DMA reset + clock doubling |
| `0007-pwm-add-sun8i-8channel-driver.patch` | new | sun8i PWM 8-channel driver (468 LOC) |
| `0008-misc-add-hy310-board-mgr.patch` | new | hy310-board-mgr (fan, NTC, GPIO, 1295 LOC) |
| `0009-misc-add-hy310-keystone-motor.patch` | new | hy310-keystone-motor (932 LOC) |
| `0010-misc-add-sunxi-mipsloader.patch` | new | sunxi-mipsloader (1608 LOC) |
| `0011-misc-add-sunxi-nsi.patch` | new | sunxi-nsi NSI-MBUS driver (441 LOC) |
| `0015-misc-add-h713-driver-kconfig.patch` | edit | drivers/misc/Kconfig entries for above |
| `0016-dt-bindings-add-h713-clock-reset-ids.patch` | new | dt-bindings clock+reset IDs (155 LOC) |
| `0017-iommu-sun50i-decouple-arm-dma-use-iommu.patch` | edit | iommu Kconfig: decouple ARM_DMA_USE_IOMMU |
| `0018-pinctrl-sunxi-add-h713-pb-bank-to-h616.patch` | edit | pinctrl-h616: add PB bank for H713 |
| `0019-iio-adc-add-h713-lradc-driver.patch` | new | H713 LRADC IIO driver (192 LOC) |
| `0020-pmdomain-add-h713-ppu-driver.patch` | new | H713 PPU power domain (287 LOC) |
| `0021-media-sunxi-cir-add-h713-vendor-init.patch` | edit | IR receiver: H713 vendor init |
| `0022-staging-cedrus-add-h713-ve3-clock-reset.patch` | edit | Cedrus: VE3 clock+reset support |
| `0023-drm-add-sun50i-h713-hdmi-rx-driver.patch` | new | H713 HDMI-RX DRM driver (1068 LOC) + drm Kconfig/Makefile wiring |
| `0024-soc-sunxi-add-cpu-comm-and-msgbox-ipc.patch` | new | ARM↔MIPS IPC: cpu_comm (=m) + msgbox AMP transport (=y), in drivers/soc/sunxi/ |

Numbering gaps (0012-0014) are intentional — those patches were abandoned
during development.

The patches add ~18,000 lines of new H713-specific code and edit ~14 upstream
kernel files. After applying, the kernel tree builds with the
`hy310_defconfig` we ship.

## Quick build (end-to-end)

```sh
export KDIR=/path/to/linux-6.16.7
export OUTDIR=./output_arm32                # build output (defaults work)
export ROOTFS=/path/to/debian-armhf         # optional: install modules here

./scripts/build_kernel_arm32.sh
```

This produces in `$OUTDIR/`:

- `zImage` — compressed kernel
- `sun50i-h713-hy310.dtb` — device tree blob
- `hy310-mainline-arm32-boot.img` — Android Boot v3 image (for `dd` to eMMC)
- `mboot32.00`, `mboot32.01` — 4 MB chunks (for U-Boot fatload from FAT
  partition)
- Kernel modules (installed to `$ROOTFS` if it exists)

## Manual build steps

### 1. Configure

```sh
cp config/hy310_defconfig $KDIR/arch/arm/configs/
cd $KDIR
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- hy310_defconfig
make olddefconfig
```

### 2. Build kernel + in-tree modules

```sh
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j$(nproc) zImage modules dtbs
```

### 3. Build out-of-tree modules

The build script handles this, but if you do it manually you need
`KBUILD_EXTRA_SYMBOLS` to chain dependent modules. Order matters: `tvtop`
exports symbols that `ge2d` and `decd` consume.

(`cpu_comm` and its `msgbox` transport are built **in-tree**, not here — see
the patch set above. They are not out-of-tree modules.)

```sh
# 1. TVTOP (no dependencies)
make -C $KDIR M=$PWD/drivers/tvtop \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

# 2. Modules that depend on tvtop (decd, ge2d, drm shim)
KBUILD_EXTRA_SYMBOLS=$PWD/drivers/tvtop/Module.symvers \
make -C $KDIR M=$PWD/drivers/decd \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

KBUILD_EXTRA_SYMBOLS=$PWD/drivers/tvtop/Module.symvers \
make -C $KDIR M=$PWD/drivers/display/ge2d \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

KBUILD_EXTRA_SYMBOLS=$PWD/drivers/tvtop/Module.symvers \
make -C $KDIR M=$PWD/drivers/display/drm \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

# 3. Independent modules
make -C $KDIR M=$PWD/drivers/audio \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

# 4. Wi-Fi + BT
make -C $KDIR M=$PWD/drivers/wifi \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules
```

> **Anti-pattern**: appending one module's `Module.symvers` to the kernel's
> `Module.symvers` (e.g. `cat tvtop/Module.symvers >> kernel/Module.symvers`)
> pollutes the kernel symbol table and breaks reproducibility. Always use
> `KBUILD_EXTRA_SYMBOLS`.

### 4. Build the boot image

`repack_boot.py` is required — it appends the DTB to zImage, wraps in
Android Boot v3 header, and splits into 4 MB chunks for U-Boot fatload.

```sh
python3 scripts/repack_boot.py \
    --outdir output_arm32/ \
    --zimage output_arm32/zImage \
    --dtb output_arm32/sun50i-h713-hy310.dtb \
    --cmdline "console=tty0 console=ttyS0,115200 earlycon loglevel=8 \
               root=/dev/sda2 rootwait rootfstype=ext4 net.ifnames=0 \
               panic=5 clk_ignore_unused pd_ignore_unused \
               hy310_board_mgr.no_rpm_shutdown=1 cma=128M"
```

> **Why this is mandatory**: an inline Python `make boot.img` skips the DTB
> append and produces a brick. Always go through `repack_boot.py`.

### 5. Install modules

```sh
./scripts/install_modules.sh . ./staging
```

## Next

- [FLASHING.md](FLASHING.md) — flash the image to eMMC or boot from USB.
- [ROOTFS.md](ROOTFS.md) — create a Debian rootfs with the modules
  pre-installed.
