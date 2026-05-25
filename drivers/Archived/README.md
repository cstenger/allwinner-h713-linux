# drivers/Archived — Pre-Refactor Reference Drivers

This directory contains read-only reference copies of drivers that were
refactored in commit `91ce2c6` (2026-04-10). They are **not compiled**, not
linked into any Makefile, and must not be used as-is.

## Why these exist

Before the refactor, `cpu_comm`, `tvtop`, and `decd` lived as kernel patches
in `patches/` and were applied at compile time. Their source existed only in
patch-diff form. The files here are the last state of those patch-embedded
implementations, extracted as plain source for reference — for example when
comparing against the current out-of-tree modules or tracing back a specific
implementation decision.

`sunxi-mipsloader.c` and `sunxi-nsi.c` are earlier reverse-engineered
monolithic files that preceded the modular approach entirely.

## Contents

| Directory / File         | Origin Patch                              | Date       | Description                          |
|--------------------------|-------------------------------------------|------------|--------------------------------------|
| `cpu_comm/`              | `0014-soc-sunxi-add-cpu-comm-ipc.patch`   | 2026-04-09 | ARM↔MIPS IPC (channel, FIFO, RPC)   |
| `tvtop/`                 | `0012-misc-add-sunxi-tvtop.patch`         | 2026-04-03 | TV subsystem top (clocks, power)     |
| `decd/`                  | `0013-misc-add-sunxi-decd.patch`          | 2026-04-03 | Display engine codec                 |
| `sunxi-mipsloader.c`     | `0010-misc-add-sunxi-mipsloader.patch`    | 2026-04-01 | MIPS coprocessor loader (monolithic) |
| `sunxi-nsi.c`            | `0011-misc-add-sunxi-nsi.patch`           | 2026-04-01 | ARM↔MIPS NSI interface (monolithic)  |

## Current drivers

The actively maintained versions are:

- [`drivers/tvtop/`](../tvtop/) — out-of-tree module
- [`drivers/decd/`](../decd/) — out-of-tree module
- `cpu_comm` — now **in-tree** (`patches/0024-soc-sunxi-add-cpu-comm-and-msgbox-ipc.patch`,
  built from `drivers/soc/sunxi/cpu_comm/`, `CONFIG_HY310_CPU_COMM=m`)

See [`docs/subsystems/cpu-comm.md`](../../docs/subsystems/cpu-comm.md) and
[`docs/subsystems/display.md`](../../docs/subsystems/display.md) for subsystem
documentation.
