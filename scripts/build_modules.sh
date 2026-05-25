#!/bin/bash
# HY310 Out-of-Tree Module Build Script
# Builds all out-of-tree modules against a configured kernel tree.
#
# Prerequisites: Kernel tree must be configured + zImage built
#   (run build_kernel_arm32.sh or manually: make hy310_defconfig && make zImage)
#
# Build order matters — dependencies are resolved sequentially:
#   1. TVTOP (display bus fabric — no deps)
#   2. DECD (video decoder — no deps)
#   3. Audio codec/cpudai/machine (flat directory)
#   4. Audio bridge (TridentALSA)
#   5. GE2D display (legacy, depends on TVTOP symbols)
#   6. WiFi AIC8800 (bsp + fdrv + btlpm)
# (CPU_COMM + msgbox are built in-tree, not here — patch 0024.)
#
# Author: well0nez
# SPDX-License-Identifier: GPL-2.0

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(dirname "$SCRIPT_DIR")
KERNEL_SRC="${KDIR:-/opt/hy310/kernels/working}"
CROSS_COMPILE=${CROSS_COMPILE:-arm-linux-gnueabi-}
JOBS=${JOBS:-$(nproc)}
MAKE="make -C $KERNEL_SRC ARCH=arm CROSS_COMPILE=$CROSS_COMPILE -j$JOBS"

if [ ! -f "$KERNEL_SRC/.config" ]; then
    echo "ERROR: Kernel not configured. Run build_kernel_arm32.sh first."
    exit 1
fi

echo "=== HY310 Out-of-Tree Module Build ==="
echo "Kernel: $KERNEL_SRC"
echo ""

# Step 1: TVTOP (display bus fabric)
echo "--- [1/6] TVTOP display module ---"
$MAKE M="$REPO_DIR/drivers/tvtop" modules
# Merge TVTOP symbols into kernel Module.symvers for downstream modules
if [ -f "$REPO_DIR/drivers/tvtop/Module.symvers" ]; then
    cat "$REPO_DIR/drivers/tvtop/Module.symvers" >> "$KERNEL_SRC/Module.symvers"
    echo "  → TVTOP symbols merged into kernel Module.symvers"
fi
echo ""

# Step 2: DECD (video decoder)
echo "--- [2/6] DECD video decoder module ---"
$MAKE M="$REPO_DIR/drivers/decd" modules
if [ -f "$REPO_DIR/drivers/decd/Module.symvers" ]; then
    cat "$REPO_DIR/drivers/decd/Module.symvers" >> "$KERNEL_SRC/Module.symvers"
    echo "  → DECD symbols merged into kernel Module.symvers"
fi
echo ""

# CPU_COMM is built in-tree (CONFIG_HY310_CPU_COMM=m, patch 0024) together
# with its msgbox transport - not an out-of-tree module. Nothing to do here.

# Step 4: Audio (codec + cpudai + machine)
echo "--- [3/6] Audio modules (codec, cpudai, machine) ---"
$MAKE M="$REPO_DIR/drivers/audio" modules
if [ -f "$REPO_DIR/drivers/audio/Module.symvers" ]; then
    cat "$REPO_DIR/drivers/audio/Module.symvers" >> "$KERNEL_SRC/Module.symvers"
    echo "  → Audio symbols merged into kernel Module.symvers"
fi
echo ""

# Step 5: Audio bridge
echo "--- [4/6] Audio bridge module ---"
$MAKE M="$REPO_DIR/drivers/audio/bridge" modules
echo ""

# Step 6: GE2D display (depends on TVTOP symbols)
echo "--- [5/6] GE2D display module ---"
EXTRA_SYMBOLS="$REPO_DIR/drivers/tvtop/Module.symvers"
if [ -f "$EXTRA_SYMBOLS" ]; then
    KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" $MAKE M="$REPO_DIR/drivers/display/ge2d" modules
else
    echo "  WARNING: TVTOP Module.symvers not found, GE2D may fail"
    $MAKE M="$REPO_DIR/drivers/display/ge2d" modules
fi
echo ""

# Step 7: WiFi AIC8800
echo "--- [6/6] WiFi AIC8800 modules (bsp + fdrv + btlpm) ---"
$MAKE M="$REPO_DIR/drivers/wifi" modules
echo ""

# Summary
echo "=== Build Summary ==="
echo "Out-of-tree modules:"
find "$REPO_DIR/drivers" -name '*.ko' -not -path '*/Archived/*' | while read ko; do
  echo "  $(basename $ko)"
done
echo ""
echo "Next: ./scripts/install_modules.sh [STAGING_DIR]"
