# Keystone motor

**Status**: ⚠️ partial — sysfs interface works, physical movement is
inconsistent, limit switch is suspect.

The HY310 has a 4-phase stepper motor for keystone correction. The
software side works; the hardware on our test unit appears to have a
defective limit switch.

## Hardware

- 4-phase stepper, GPIO-driven phases
- **Limit switch**: PH14 (always reads LOW on our test unit — possible
  hardware damage or wiring fault)

## Driver

- `drivers/misc/hy310-keystone-motor.c` (out-of-tree)
- Work-queue based async movement
- Step tables configurable via DTS
- Sysfs interface for user-space control

## Known issues

### Limit switch PH14 always reads LOW

The homing sequence can't reliably detect the limit position. Either the
switch is damaged or the wiring is wrong on the test unit.

**Workaround**: software step-count guard in the homing routine — caps
maximum steps to prevent mechanical damage.

**Real fix**: probe PH14 physically. May be unit-specific.

### Physical movement inconsistent after boot

If homing fails, the motor phases may not be in a known state. A manual
phase reset or full power-cycle may be required.

## Sysfs interface

```sh
ls /sys/devices/platform/hy310-keystone-motor/
# step      — set step count (signed; negative = reverse direction)
# position  — current position (steps from home)
# homing    — trigger homing sequence
```

Example:
```sh
echo 100 > /sys/devices/platform/hy310-keystone-motor/step
cat /sys/devices/platform/hy310-keystone-motor/position
```

## Next work

1. Probe PH14 to determine if the limit switch is electrically
   connected (multimeter + manual switch press).
2. Verify GPIO phase ordering against the motor datasheet (different
   phase orderings would explain the inconsistent movement).
3. Add suspend/resume support so the motor doesn't lose calibration on
   suspend.
