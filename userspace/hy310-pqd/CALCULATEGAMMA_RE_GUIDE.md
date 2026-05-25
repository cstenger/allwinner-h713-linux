# CalculateGamma — Complete Reverse-Engineering Guide

**For a fresh agent that has NEVER seen this project.**
Pretend nothing is obvious.  Follow every step literally.

## 0. What you are doing and why

You are reverse-engineering a single function called **`CalculateGamma`**
from a closed-source Android ARM32 shared library called
`libhaldisplay.so`.  The goal is to produce a C++ re-implementation
that we can compile into our daemon.

When finished, our daemon will be able to:
- Take the user's "gamma factor" slider (0..10) from `tvpq.db`
- Take the 33 control-point gamma curve from `tvpq.db::Gamma_Point`
- Take the current colour-temperature / picture-mode
- Compute 6 gamma LUT tables (2 KB each)
- Write them to physical RAM addresses `0x4B48C2F0..0x4B4958F0`
  **BEFORE** the MIPS co-processor boots

The MIPS firmware reads those addresses during `hal_adapter_init` and,
if the data is valid, registers all 64 Picture-Quality routines so that
our daemon's ARM→MIPS IPC calls actually land in MIPS-side handlers
instead of being silently dropped.

**This is the only remaining blocker before the whole PQ pipeline
works end-to-end.**

Read `BACKGROUND.md` first if you want the full context, but this guide
is self-contained.

## 1. Tools you have on openclaw (pre-installed, ready to use)

You will do **ALL** the RE work on **openclaw** (the x86_64 Debian
build server).  SSH in as `ssh openclaw`.  You have:

| Tool           | Location                                          | What it does                                  |
| -------------- | ------------------------------------------------- | --------------------------------------------- |
| **IDA Pro 9.1**| `/opt/ida-pro-9.1/idat`                           | Headless IDA (no GUI) — Hex-Rays decompiler   |
| **IDA GUI**    | `/opt/ida-pro-9.1/ida` (if DISPLAY is available)  | Interactive mode                              |
| **Capstone**   | `python3 -c "import capstone"` (already works)    | Pure disassembler, ARM/Thumb-2                |
| **pyelftools** | `python3 -c "from elftools.elf.elffile import ELFFile"` | Parse ELF headers, symbols, sections    |
| **cross-g++**  | `/usr/bin/arm-linux-gnueabihf-g++`                | Build the port to target armhf (hy310 arch)  |
| **libsqlite3-dev:armhf** | already installed                       | Needed if you touch tvpq.db in your port      |

Pre-computed IDA databases already exist — **do not re-analyse from
scratch**, just open them:

```
/tmp/pq_re/libhaldisplay.so              ← the binary
/tmp/pq_re/libhaldisplay.so.{id0,id1,id2,nam,til}  ← the IDB
/tmp/pq_re/libUtility.so  (+ .id0 etc.)  ← reference IDB
/tmp/pq_re/libtvpq.so     (+ .id0 etc.)  ← caller context IDB
```

A partial Hex-Rays decompilation of `CalculateGamma` is already saved
at `/tmp/pq_re/haldisp_decomp.txt` (also copied into
`/opt/hy310/gamma_re/existing_decomp.txt`).  Start by READING that
file before running the tool again.

### How to invoke IDA headless (no GUI needed)

Write a small Python script, run `idat -A -Sscript database`:

```bash
ssh openclaw
cat > /tmp/my_script.py << 'PYEOF'
import idaapi, ida_auto, ida_hexrays
ida_auto.auto_wait()
# … your analysis here …
idaapi.qexit(0)
PYEOF
cd /tmp/pq_re
sudo -u openclaw TVHEADLESS=1 /opt/ida-pro-9.1/idat \
    -A -S/tmp/my_script.py libhaldisplay.so > /dev/null 2>&1
```

Useful IDA Python APIs you will need:

```python
import idaapi, ida_hexrays, idc, idautils, ida_bytes, ida_funcs

ida_auto.auto_wait()                    # let auto-analysis finish

cf = ida_hexrays.decompile(0xBC48)      # decompile by address
str(cf)                                  # → pseudocode as text

idc.get_func_name(0xBC48)               # → "CalculateGamma" or similar
idc.generate_disasm_line(0xBC48, 0)     # single-line disasm
ida_bytes.get_dword(0xBC48)             # read 4 bytes as u32
ida_bytes.get_qword(0xBC48)             # read 8 bytes
idautils.Functions()                    # iterate all function starts
idautils.XrefsTo(0xBC48)                # find callers

from ida_segment import get_segm_by_name
seg = get_segm_by_name(".rodata")       # find section
```

### How to invoke Capstone (pure disasm)

```bash
ssh openclaw
python3 << 'PYEOF'
import capstone
with open('/opt/hy310/gamma_re/libhaldisplay.so', 'rb') as f:
    f.seek(0xBC48)
    data = f.read(0x109C)
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB |
                                        capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True
for ins in md.disasm(data, 0xBC48):
    print(f'0x{ins.address:06x}  {ins.bytes.hex():<8}  '
          f'{ins.mnemonic:<8} {ins.op_str}')
PYEOF
```

**Tip:** always use Capstone + IDA **together**.  IDA shows you the
structure and intent; Capstone shows you exactly which instructions
produce each effect.  Mismatches between them reveal bugs in either's
interpretation.

## 2. Where every file is

The target function lives inside a 62 KB ARM32 ELF.  All the reference
material is pre-staged on **openclaw** at `/opt/hy310/gamma_re/`:

```
/opt/hy310/gamma_re/
├── README.md                  ← this guide (same content)
├── libhaldisplay.so           ← the binary with CalculateGamma
├── libUtility.so              ← used for Name2ID + CRC32 (reference)
├── libtvpq.so                 ← calls CalculateGamma; context
├── display.bin                ← MIPS firmware (receiver of the LUTs)
├── tvpq.db                    ← SQLite, holds the 33 control points
├── crc_table.bin              ← 1 KB CRC32 table (already extracted)
└── existing_decomp.txt        ← partial IDA decomp of CalculateGamma (1372 lines)
```

The original copies of the stock libraries also live at
`/opt/hy310/stock-re/super_extracted/vendor/lib/` plus
`/opt/hy310/stock-re/display.bin` on the same machine.

On **Windows** (your Claude Code host, where you edit source) copies
live at `C:\Users\User\AppData\Local\Temp\hy310\hy310-pqd\` (the
daemon's source project).

The **hy310** board itself is the *target* at runtime — you don't do
RE there.  Use it only for validation (section 7.2).

## 3. The function you must reverse

```
Binary:       libhaldisplay.so
File offset:  0xBC48  (48712 decimal)
Load address: 0x00000000 (relative — ELF is PIE)
Symbol:       CalculateGamma
Size:         4252 bytes (0x109C)   ← that's a lot of code
Instruction set: ARM/Thumb-2 mixed, heavy NEON SIMD usage
Calling conv: ARM AAPCS, first 4 params in r0..r3
```

Function signature (from IDA's partial decomp — see existing_decomp.txt):

```c
int __fastcall CalculateGamma(char **a1);
```

`a1` is a pointer to an array of `char*`, each pointing to a small piece
of input state.  Stock reads:

- `a1[1]` → `ucGammaFactor` (0..10, the user slider)
- `a1[2]` → `ucColorTemp`
- more indices → other globals we haven't fully mapped

## 4. Two tooling paths — choose one

### Path A: use IDA's Hex-Rays decompiler (recommended)

Pros: produces C-like pseudocode; IDA handles NEON intrinsics as
recognisable names (`vld2_s16`, `vshll_n_u16`, …).
Cons: IDA output is sometimes imprecise with register allocation; you
have to cross-check with disassembly when control flow gets odd.

```bash
# On openclaw (run these exact commands)
ssh openclaw
cat > /tmp/decomp_cg.py << 'PYEOF'
import idaapi, ida_auto, ida_hexrays
ida_auto.auto_wait()
out = open('/tmp/pq_re/CalculateGamma_full.txt', 'w')
try:
    cf = ida_hexrays.decompile(0xBC48)    # the file offset
    out.write(str(cf) if cf else 'decompile returned None\n')
except Exception as e:
    out.write(f'error: {e}\n')
out.close()
idaapi.qexit(0)
PYEOF
cd /tmp/pq_re
sudo -u openclaw TVHEADLESS=1 /opt/ida-pro-9.1/idat \
    -A -S/tmp/decomp_cg.py libhaldisplay.so > /dev/null 2>&1
cat /tmp/pq_re/CalculateGamma_full.txt
```

This produces 4 KB+ of pseudocode.  Save it, read it, walk the control
flow from top to bottom.  Ignore the 200-odd `v123`-style temporaries —
they disappear once you group them into named variables in your port.

### Path B: use Capstone (pure disassembly)

Pros: deterministic, no tool black-box, exact per-instruction view.
Cons: you see raw ARM/Thumb-2 and must build structure by hand.

```bash
# On openclaw — capstone is already installed
ssh openclaw
python3 - <<'PYEOF'
import capstone

BIN   = '/opt/hy310/stock-re/super_extracted/vendor/lib/libhaldisplay.so'
OFF   = 0xBC48          # file offset of CalculateGamma
SIZE  = 0x109C          # 4252 bytes

with open(BIN, 'rb') as f:
    f.seek(OFF)
    data = f.read(SIZE)

# The ELF is an ARM Linux shared object; .text here is Thumb-2.
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB |
                                        capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True
for ins in md.disasm(data, OFF):
    print(f'0x{ins.address:06x}  {ins.bytes.hex():<8}  '
          f'{ins.mnemonic:<8} {ins.op_str}')
PYEOF
```

Save that to `calgamma.asm` and read it alongside the IDA pseudocode —
they are complementary.

## 5. What to look for while reading

### 4.1 The input-parameter block

The function starts with a big conditional assignment cluster that
populates a `goldGammaInfo` struct with values from `a1[1..N]`.  Each
sub-block is gated by "is this pointer non-null":

```c
if ( v7[1] ) {
    ucGammaFactor       = *v7[1];
    dirty_flag |= 0x01;
}
if ( v7[2] ) {
    ucColorTemp         = *v7[2];
    dirty_flag |= 0x02;
}
…
```

Your port needs the same struct.  Name it `GammaInput` and define
one field per observed `v7[i]`.

### 4.2 The "no-change fast-path"

After populating the struct, there's a check like `if (!dirty_flag) return 0;`.
This avoids re-generating LUTs when nothing actually changed.  Keep this
in your port — it matters for performance.

### 4.3 The dispatch table

Near the top of the real work there's an array lookup:

```c
static constexpr double gamma_exp_table[11] = { … };
double exp = gamma_exp_table[ucGammaFactor];
```

Find that table — it's a ro-data blob of 11 doubles (88 bytes) in
`.rodata`.  Dump it with:

```bash
# Find the .rodata section first
readelf -S libhaldisplay.so | grep rodata
# then hexdump the range around the reference
```

The table maps the user slider to a specific gamma exponent.  In stock
the slider 0..10 corresponds to exponents roughly 1.6 … 3.6.

### 4.4 The NEON loops

`CalculateGamma` contains several loops that look like this (pseudocode):

```c
for (int i = 0; i != 2048; i += 16) {
    int16x4x2_t pair = vld2_s16((const int16_t *)(input + i));
    uint32x4_t  pack = vaddw_u16(vshll_n_u16(pair.val[1], 12), pair.val[0]);
    *(uint32x4_t *)(output + i) = pack;
}
```

These are the **packed-sample writers**.  They take 2048 bytes of
16-bit samples and produce 512 × u32 where each u32 holds two 12-bit
packed values.  **This is not LUT generation — it's format conversion
for the hardware's bulk-write port.**  The actual LUT math is the
curve interpolation code, which happens before these packing loops.

### 4.5 The curve-math core

Between the input-ingest and the packing loops you'll see arithmetic
like:

```c
for (int i = 0; i < 1024; ++i) {
    double t = (double)i / 1023.0;
    double y = pow(t, exp);                 // primary curve shape
    y *= gain[channel];                     // per-channel gain (R/G/B)
    y += offset[channel];                   // per-channel offset
    // …per-source corrections…
    lut[i] = clamp((int)(y * 4095), 0, 4095);
}
```

The specific transformations and their order are what you need to
recover.  Expect:
- a base power-law curve from `ucGammaFactor`
- per-colour-channel gain/offset from `ucColorTemp`
- possibly extra segments for `BlackExtension` / `DCI` / `ucPictureMode`

### 4.6 The 6 LUT outputs

Six 2 KB buffers are produced.  They correspond to 6 `dwGammaType`
values that MIPS registers at boot:

| dwGammaType | DRAM phys   | MIPS VA (KSEG0) | Source mode |
| ----------- | ----------- | --------------- | ----------- |
| 0x30030000  | 0x4B48DAF0  | 0x8B48DAF0      | CM_CVBS     |
| 0x30030001  | 0x4B48F2F0  | 0x8B48F2F0      | CM_HDMI     |
| 0x30030002  | 0x4B48C2F0  | 0x8B48C2F0      | CM_SVD      |
| 0x30030003  | 0x4B4906F0  | 0x8B4906F0      | (??)        |
| 0x30030004  | 0x4B491EF0  | 0x8B491EF0      | (??)        |
| 0x30030005  | 0x4B4934F0  | 0x8B4934F0      | (??)        |

`display.bin` at VA `0x8B152A60` registers the 6 LUTs in a loop — see
that code for the exact pairing.  Your port must produce **all 6**,
not just one.

### 4.7 The hardware write

The last ~200 bytes of `CalculateGamma` call `WriteGammaLUTByColor`,
which uses `WriteRegBulkU32` to push the packed samples to display
engine registers 0x05208000 / 0x05208800 / 0x05209000.  **Skip this
in your port — we already handle it in `src/pqgamma.cpp`.**  Your
output is just the six raw 1024-entry int16 LUTs; the daemon writes
them either to DE2 (for display-calibration gamma) or to the MIPS
DRAM region (for MIPS internal gamma).

## 6. Deliverable

Produce a new file in the project:

```
src/pq_calculate_gamma.cpp
include/pq_calculate_gamma.h
```

With this API:

```cpp
namespace hy310::pqgamma {

struct GammaInput {
    uint8_t ucGammaFactor;       // 0..10
    uint8_t ucColorTemp;         // 0..3
    uint8_t ucPictureMode;       // 0..6
    uint8_t ucBlackExtension;    // 0..3
    uint8_t ucDCI;               // 0..3
    // …whatever else CalculateGamma's a1[i..] accesses
};

using Lut1024 = std::array<int16_t, 1024>;

struct GammaOutputSet {
    Lut1024 cm_cvbs;    // 0x30030000
    Lut1024 cm_hdmi;    // 0x30030001
    Lut1024 cm_svd;     // 0x30030002
    Lut1024 cm_type_3;  // 0x30030003
    Lut1024 cm_type_4;  // 0x30030004
    Lut1024 cm_type_5;  // 0x30030005
};

/* Pure function — given input state, produce all 6 LUTs.
 * Never touches /dev/mem or hardware. */
GammaOutputSet calculate_gamma(const GammaInput& in,
                                const std::array<int16_t, 33>& control_points);

} // namespace
```

## 7. Validation

You can validate your port in two ways:

### 6.1 Byte-for-byte against stock (strongest)

If you can boot Stock Android on the board:
1. Let Stock Android finish boot so its libtvpq initialises MIPS fully
2. `ssh` in, `dd if=/dev/mem bs=1 skip=$((0x4B48C2F0)) count=36864 of=/tmp/ref.bin`
3. Run your port with the same input state
4. `cmp` the outputs — should match byte-for-byte

If you can't boot stock, skip this step.

### 6.2 Live round-trip

1. Boot mainline Linux
2. Use your port to write the 6 LUTs to `0x4B48xxxx` via `/dev/mem`
   **before** `sunxi-mipsloader` releases MIPS from reset (requires a
   kernel module tweak — not in scope for RE but the daemon needs a
   hook)
3. Watch `/sys/class/hy300/mips/elog_full` during boot
4. Expected change: the "Can not get gamma LUT data" message
   disappears and `dmesg` shows more than one
   `id_util_cpucomm.c:374)THal_…` registration line

If you see 60+ routine names registered instead of one, you're done.

## 8. How to hand your work back

Put your `src/pq_calculate_gamma.cpp` + header under
`/opt/hy310/hy310-pqd/` on openclaw, make sure `make` still builds,
and update `BACKGROUND.md` §6 to say the blocker is resolved.

## 9. Things that are NOT your job

- Don't touch the kernel.  The glue that injects the LUTs into DRAM
  before MIPS boot is separate work.
- Don't re-implement `WriteGammaLUTByColor` — we have it.
- Don't touch FusionDale IPC — it's proven working.
- Don't attempt to run `libhaldisplay.so` under a bionic shim.  We did
  this deliberately to stay Android-free.

## 10. Time budget

Realistic estimate: **1–2 working days** for a competent RE engineer.
Break it down:
- ~3 h reading existing decomp + asm, identifying the struct fields
- ~2 h recovering the gamma_exp_table + any hard-coded per-mode arrays
- ~4 h porting the curve math
- ~2 h writing tests against the 33-point input data from tvpq.db
- ~2 h validation against live hardware

## 11. Quick sanity checks as you go

As you port, verify each stage:

```
input ucGammaFactor = 3  →  exp ≈ 2.2           (stock default)
identity curve (33 points straight line)
  → lut[0]   = 0
  → lut[512] ≈ 2048
  → lut[1023] = 4095

gamma 2.2 curve
  → lut[0]   = 0
  → lut[512] ≈ 891       (darker in mid-tones than identity)
  → lut[1023] = 4095
```

These match Stock's own output and are already unit-tested in
`tests/test_gamma_interp.cpp`.

Good luck.
