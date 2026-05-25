# Power management

**Status**: ✅ working — reboot, poweroff, watchdog, power domains.

## Reboot / poweroff

Both verified. Reboot uses the watchdog. Poweroff works.

```sh
reboot
poweroff
```

**Caveat**: software reboot (`reboot` or `systemctl reboot`) may not
reliably recover MIPS from a stuck state. If `cat /sys/class/hy300/mips/state_machine`
returns nothing after a software reboot, MIPS is in a WAIT-ACK stuck
state and you need a physical power-cycle.

## Watchdog

| Watchdog | Address | Compatible |
|---|---|---|
| Main WDT | `0x02051000` | `sun6i-a31-wdt` |
| R_WDOG (backup) | `0x07020400` | — |

Main watchdog is at `0x02051000`, **not** the H6 address `0x030090a0`.
Do not copy H6 DTS verbatim.

## ARISC

OR1K firmware (172 KB) is loaded by U-Boot before kernel handoff. ARISC
handles low-level power sequencing (PMU, HPD pin). The kernel sees ARISC
as an already-running peer via the Msgbox.

## Power domains

Driver: `allwinner,tv303-pmu` at base `0x07001000`.

| Domain | Offset | Description | Boot state |
|---|---|---|---|
| `pd_gpu` | +0x000 | GPU (Mali-G31) | ON |
| `pd_tvfe` | +0x080 | TV frontend | ON |
| `pd_tvcap` | +0x100 | TV capture | ON |
| `pd_ve` | +0x180 | Video engine | ON |
| `pd_de` (?) | +0x200 | display engine? | ON |
| unused | +0x280 | — | all zeros |

All domains come up from U-Boot. No power domain driver is strictly
needed for initial bringup, but should be implemented for proper PM.

Reference base: `0x07001000` (NOT `0x07010000` as initially assumed —
session M corrected this).

## Boot arguments related to power

```
panic=5             # auto-reboot 5s after kernel panic
clk_ignore_unused   # required (see below)
pd_ignore_unused    # required, same reason
```

`clk_ignore_unused` and `pd_ignore_unused` must stay until the clock and
power-domain trees are fully described in DTS. Removing them prematurely
causes random clocks/domains to be gated and breaks peripherals.

## Suspend / resume

Not yet tested. The hardware supports it (ARISC manages low-power modes),
but we haven't wired up the kernel-side path. PR welcome.

## See also

- [Boot subsystem](boot.md) — how U-Boot brings up MIPS and ARISC
- [Architecture overview](../architecture.md) — three-CPU coordination
