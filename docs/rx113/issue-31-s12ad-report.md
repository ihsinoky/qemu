<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 31: RX113 S12ADb

Base: `598fadfc66` (PR #30), branch `rx113/issue-31-s12ad`.
The 0x00089000..0x000890ff stub is replaced by an RX113-only device.
No shared peripheral, CPU, RX62N implementation, or existing test was changed.
No push, merge, PR, or upstream submission was performed.

## Implementation and manual basis

All references below are to R01UH0448EJ0120 Rev.1.20, the supplied RX113
hardware manual. The PDF was converted locally using `pdftotext -layout`;
no new source or test asset was downloaded.

| Reference | Implemented behavior |
| --- | --- |
| Table 5.1, pp.112-114; 36.2, pp.1237-1257 | Register addresses, byte/word access widths, reset values, writable/reserved masks and read-only data registers |
| 36.2.5-9; 36.3.1-4, pp.1242-1248,1258-1266 | Single, continuous and independent group A/B scans; ascending AN000..AN015, AN021 order; ADST start, completion and forced cancellation |
| 36.2.5, 36.2.14, Tables 36.5/6 | Software start independent of TRGE; selected MTU/ELC triggers; qualified ADTRG0# falling edge; busy triggers discarded without queuing |
| 36.2.10-12, 36.3.7 | Per-channel one-to-four consecutive conversions and addition, including AN021 and extended inputs; sums are committed after each conversion |
| 36.2.1-4, 36.2.13, 36.3.6 | Right/left alignment, addition results shifted by two, automatic data clearing on word reads |
| 36.3.2.4, 36.3.4.2-3 | Double trigger alternates ADDRy/ADDBLDR; first synchronous scan suppresses IRQ; software scans do not consume the synchronous pair; group B preserves the pair |
| Table 36.8, p.1267 | Separate PCLKB/ADCLK start, sample, conversion and termination phases, with high-speed/normal processing times |
| 9.1, 9.2.1, 9.7.2, pp.160-164,193 | ADCLK from PCLKD; independent SCKCR.PCKD and PCKB divisors, using the existing SYSTEM source selection |
| 11.2.2, p.214; 36.8.5-6 | PRC1-protected MSTPA17; register access disabled during module stop; 1 us wait before a fresh start after release |
| Table 14.3, p.269; 36.4 | S12ADI0 vector 102, GBADI vector 103, both edge requests, independently masked by ADIE/GBADIE |
| 36.5 | Named scan-end event output independent of ADIE, excluding group B and the first double-trigger scan |
| 19.2.15, p.395; 36.2.17, 36.6 | PJ6/PJ7 ASEL reference selection, internal high reference selection with 1 us discharge and 5 us stabilization |

Section 9 explicitly identifies PCLKD as the S12AD operating clock.
Table 36.1's note repeats PCKB for both clocks; this model follows the
clock-generation chapter and its independent PCKD field, rather than
using PCLKB for ADCLK.

For a single scan of n channels, timing follows Table 36.8:

```
tD    = 2 PCLKB + 3 ADCLK (software or MTU/ELC)
      = 4 PCLKB + 3 ADCLK (qualified ADTRG0#)
tSPL  = (ADSSTRn + 1) ADCLK per conversion
tCONV = 23 ADCLK (ADHSC=1), 29 ADCLK (ADHSC=0)
tED   = 2 ADCLK + 1 PCLKB
tSCAN = tD + sum(tSPL + tCONV) + tED
```

The terminating PCLKB uses the documented upper end of the interrupt
latency window. Data is committed after each conversion; the final
interrupt and ADST clear follow tED. Continuous scans omit tED and only
pay tD on the initial scan. Addition repeats sampling and conversion on
the same channel before advancing. At 32 MHz in high-speed mode with
ADSSTR=20, one conversion produces data after 49 cycles (1531.25 ns),
and its single-scan interrupt follows at 52 cycles (1625 ns).

Sample-and-hold captures the input at the end of tSPL. Changing the
injected input during tCONV does not change the held result. Fractional
clock work is carried between phases, so 31.25 ns cycles do not each
round to 32 ns. Frequency/divisor changes first account for elapsed work
at the old rate, then rescale the unfinished phase. Module stop freezes
that work and retains registers; release resumes it. Hardware requires
ADST=0 before MSTP entry; retaining an in-flight conversion is an explicit
emulation extension for deterministic pause/resume, not a claim about
prohibited hardware sequences. While stopped, MMIO reads zero and writes
are discarded, matching this fork's Issue 10 gate policy.

Reset cancels conversion, restores controls/data/sampling defaults and
leaves external voltage and trigger drivers intact. Migration retains
those drivers, held sample, addition progress, double-trigger state,
clock/gate and fractional work. Loading derives timer deadlines without
recreating already-delivered interrupts. Migration from the old stub-only
machine is not supported as a compatibility contract.

## Voltage injection and PORT interaction

The device lives at `/machine/mcu/s12ad`. Read/write uint64 QOM properties
use integer microvolts, accepted from 0 through 10000000:

| Properties | Reset-independent external default |
| --- | --- |
| `an000-uv` through `an015-uv`, `an021-uv` | 0 uV |
| `temperature-uv`, `internal-reference-uv` | 0 uV; no temperature or bandgap value is synthesized |
| `avcc-uv`, `vrefh-uv` | 3300000 uV, explicit default virtual-board supply assumption |
| `avss-uv`, `vrefl-uv` | 0 uV |

For example, after releasing MSTPA17 and allowing the 1 us restart wait:

```json
{"execute":"qom-set","arguments":{"path":"/machine/mcu/s12ad","property":"an000-uv","value":1650000}}
```

With the default AVCC/AVSS references the ideal 12-bit result is 2048.
The model computes `floor((Vin - Vlow) * 4096 / (Vhigh - Vlow))`, saturated
to 0..4095. Inputs below/above the references saturate deterministically;
this does not simulate the electrical effects of violating pin ratings.
A nonpositive reference span logs a guest error and yields zero. Supply
properties must be set to the board's actual values for meaningful tests.
The internal-reference input and internal high-side reference share the
same externally supplied `internal-reference-uv` property.

The uninjected-channel policy is a **grounded virtual fixture**, chosen
because no electrical floating-node network exists in this machine. Zero
is reproducible and corresponds to the lower rail for the default board;
it is not a prediction of an unconnected physical pin. There are no
channel-number patterns, ramps, random samples or fabricated sensor
readings. The fixture can replace every input and reference independently.
Voltages survive reset and migrate, following the existing PORT external
GPIO driver pattern. Property writes synchronize the ADC before changing
its input, so an already-held sample is preserved.

Section 18.2 diagrams branch the analog input before the ASEL-controlled
digital input switch. Sections 19.3.2-3 require PMR/PDR/PCR=0 and ASEL=1
to avoid loss of analog accuracy. Accordingly, ASEL does not gate the
ideal voltage-to-code path. Existing PORT behavior already disables
GPIO/IRQ participation for ASEL pins. No digital pad voltage, output
contention or pull-up is converted into a guessed analog voltage. For
references, PJ6PFS.ASEL directly selects AVCC versus VREFH, and
PJ7PFS.ASEL selects AVSS versus VREFL. The ADC reads those existing PORT
registers through a fixed device link; PORT code/behavior is unchanged.

## Trigger connection contract

The ten named `trigger` GPIO inputs are ideal qualified signals:

| Input index | Source |
| --- | --- |
| 0 | ADTRG0#, falling edge; initial level high |
| 1..8 | TRG0AN, TRG0BN, TRGAN, TRG0EN, TRG0FN, TRG4AN, TRG4BN, TRG4ABN, rising edge |
| 9 | ELC, rising edge |

Indices 1..9 match the ADSTRGR selector values exactly. Selectors, TRGE,
EXTRG, group and busy state are checked inside the ADC. They are not
hardwired group-A/group-B bypasses. The one-element named `scan-end`
output supplies the ADC event to a future ELC. These interfaces are
intentionally unconnected to MTU, ELC or PORT pads in this issue.

## Validation

All final execution uses the binary recorded in
`.issue31-runtime/tested-binary.sha256`. Evidence and reproduction script
are in `.issue31-runtime/`; the existing test sources are unchanged.

| Check | Result | Evidence |
| --- | --- | --- |
| Configure, RX-only build | PASS | `configure.log`, `build-review.log` |
| New S12AD qtest | 14/14 PASS | `s12ad-review.log`, `regression.log` |
| RX qtest and quick functional suites | 25 OK, 4 existing skips, 0 failures | `regression.log` |
| Phase 1, all four MCU parts and reset | 8 PASS | `regression.log` |
| RX113/RX62N FPU | 2 PASS | `regression.log` |
| LCDC functional / qtest | 1 / 6 PASS | `regression.log` |
| RTC / PORT / ICU | 20 / 6 / 5 PASS | `regression.log` |
| IWDT functional / qtest | 2 / 49 PASS | `regression.log` |
| MTU / clock / SCI | 21 / 9 / 3 PASS | `regression.log` |
| Genuine FIT rebuilt PLL and HOCO, before/after reset | Both PASS | `fit-build-*.log`, `fit-*.log`, `verify-*/result.json` |
| SYSTEM oscillator/protection checks, without/with FIT ELF | Both PASS | `system.log`, `system-elf.log` |
| gdbsim-r5f562n7 / gdbsim-r5f562n8 | Listed, both run 3 seconds and exit 0 | `machine-help.log`, `gdbsim-smoke.json` |
| Existing gdbsim U-Boot functional test | PASS | `gdbsim-functional.log` |
| Changed-file checkpatch | 0 errors, 0 warnings | `checkpatch-files.log` |
| Whitespace and import patch reverse validation | PASS | `patch-validation.log` |

The new qtest covers voltage endpoints/quantization/saturation, uninjected
inputs, all 17 data-register mappings and scan order, continuous timing,
every MTU/ELC selector for both groups, ignored busy/disabled/wrong-source
triggers, asynchronous timing, both conversion speeds, all clock divisors
and all sampling-register mappings, distinct PCLKB/PCLKD ratios, fractional
clock changes, MSTP in every phase, retained ICU requests, forced stops,
one-to-four additions, mixed normal/addition channels including AN021,
double triggers interleaved with software or group B, formatting/ACE,
reference switching/discharge, extended inputs, sample-and-hold, access
widths, reset and running/stopped migration.

Initial new-test failures came from a 1 ns end-delay expectation and a
fresh scan issued before the required post-MSTP wait; both harness errors
were corrected without weakening the model. Final review corrected
HVREFDIS to be inactive when OCSVSEL=0, and added a regression case.
Earlier logs remain available. The initial overlapping build attempts
also produced transient regeneration/link failures; the serial final
build and complete regression pass use the finished source tree.

The four suite skips are the existing cdrom, qos, info_usernet and linters
cases. FIT ZIPs were copied from Issue 29 and checked by the unchanged
pinned-hash builder; both configurations were rebuilt with the supplied
RX toolchain. gdbsim uses the copied local asset cache. Configure used
its normal existing/vendored Python dependencies. The scratch-only
`test-env/sitecustomize.py` routes QEMUMachine defaults to TMPDIR. No
source, test asset, or build directory was placed in `/tmp`.

Reproduce the new qtest and standard suites from the worktree root:

```sh
export TMPDIR="$PWD/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$HOME/workspace/projects/qemu/.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
mkdir -p "$TMPDIR" build
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
ninja -C build -j8
export QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx"
build/tests/qtest/rx113-s12ad-test
meson test -C build --no-rebuild --suite qtest-rx --suite func-rx --print-errorlogs
```

`.issue31-runtime/regression.sh` records the full executed commands,
including both FIT verifiers, both SYSTEM checks, the gdbsim functional
test and two-machine smoke. FIT rebuild commands follow the unchanged
`tests/functional/rx/fit_boot/README.md`, using the supplied toolchain and
scratch-local verified ZIPs.

## D-006 residual scope and merge-gate decisions

All requested scan, input injection, data, conversion timing, IRQ,
software/internal trigger and clock/MSTP behavior is implemented. No ADC
control register is left as a silent stored-only stub. The following
remain explicit integration/electrical limits; estimates include tests.

| Remaining behavior | Reason and expected size |
| --- | --- |
| MTU request wiring, ELC routing and DTC transfers | MTU's advanced ADC-request generation, ELC and DTC are not implemented by this issue. Qualified ADC input and event output ports are supplied, as the instructions allow. Approximately 1-2 days for MTU request generation/link ordering after the relevant MTU modes exist; ELC/DTC need their own multi-day implementations and about 1-2 additional integration/test days. |
| ADTRG0# pad multiplexing, pulse-width qualification and clock synchronizer phase | The input represents an already-qualified falling edge, rather than a physical waveform on P16/P27/PB0/P25/P07. The machine lacks common sampled-clock/pad timing infrastructure. Table 36.8 trigger latency is modeled; invalid short pulses and sub-cycle synchronizer phase are not. Approximately 2-3 days for PORT routing, sampling/filter state and boundary tests. |
| Analog charge, source impedance, settling/noise, accuracy errors and digital/analog contention | A scalar voltage fixture cannot faithfully solve the RC/pad network described in 36.7-8. Sampling timing and hold are implemented, but charge-dependent invalid first sensor results and TRGE analog power transients are not synthesized. A reusable electrical/sensor model and calibrated parameters would take roughly 3-5 days before ADC integration; board-specific characterization is additional. Firmware must still follow documented sampling and stabilization requirements. |
| Temperature/bandgap generation and CMPB/DAC interaction | Extended input conversion is implemented using supplied voltages. Automatic temperature-to-voltage calibration, CMPB's reference ownership inhibition and DAC synchronization require peer analog devices, currently absent. Roughly 2-4 days for calibrated sources/ownership, plus 1-2 days for ADC/DAC ordering after the peer models exist. |
| Software standby/deep sleep and bus-cycle contention | Existing CPU/SYSTEM does not expose these full power transitions or bus arbitration. MSTP and ordinary virtual-time operation work; physical shutdown/restart and simultaneous bus-cycle races need wider infrastructure, approximately 3-5 days plus 1-2 days of ADC-specific checks. |

These are not claims of physical analog fidelity. They require explicit
PO/PMO assessment under D-006; the qualified peer-trigger boundary is
specifically permitted by this issue's instructions.

## Delivery

The repository's `.git` is read-only in this environment. The attempted
`git add` failed creating `.git/index.lock` (`git-add.log`), so no commit
was created. Changes remain in the working tree. The importable patch is
`.issue31-runtime/issue31.patch`, with its SHA-256 and exact file list in
`issue31.patch.sha256` and `changed-files.txt`. Apply on the stated base
with `git apply /path/to/issue31.patch`. Patch-level checkpatch has zero
errors; a generic new-file MAINTAINERS advisory is recorded separately.
The patch excludes AGENTS.md, the manual, runtime assets, `.t/` and builds.

Changed files:

* `hw/rx/meson.build`
* `hw/rx/rx113-s12ad.c`
* `hw/rx/rx113.c`
* `include/hw/rx/rx113.h`
* `include/hw/rx/rx113_s12ad.h`
* `tests/qtest/meson.build`
* `tests/qtest/rx113-s12ad-test.c`
* `docs/rx113/issue-31-s12ad-report.md`
