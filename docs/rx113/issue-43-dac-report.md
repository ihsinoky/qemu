<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #43 — RX113 R12DAA D/A converter

Base: `7b28e0f0634cd79700279849c73d5384f5d3bdbd`.
Branch: `rx113/issue-43-dac`. Fork-local working-tree delivery; no commit,
push, merge or upstream submission.

## Model and hardware references

The DAC stub at `0x00088040` is replaced by an RX113-only sysbus device,
`/machine/mcu/dac`. Source: supplied R01UH0448EJ0120 Rev.1.20, section 37
(pp.1277–1287), section 17.3.4 and Table 43.45. No shared peripheral,
RX62N or gdbsim source is changed.

DADR0/1 require aligned 16-bit accesses; the four control registers require
byte accesses. Wrong-width and undefined accesses log guest errors, return
zero and discard writes. DADR reserved bits read zero according to DPSEL;
DACR bits 4:0 read one, bit 5 reads zero (reset `0x1f`). The other registers
mask reserved fields. Prohibited REF encodings log a guest error and retain
the previous selection, following the existing RX113 ELC convention for
prohibited selections; no voltage is invented for those encodings.

Each channel has a separate current conversion code and software-visible
data register. With DAADST=0, writes update the code immediately. With
DAADST=1, writes during a scan wait for the named `sync` input. The latest
pending data is used, independently for each channel. Turning DAADST off
makes the current register data effective immediately. An idle S12AD allows
an update after one PCLKB cycle, as section 37.3.1 specifies. A small timer
tracks that cycle across PCLKB changes and module stop. The additive
`rx113_s12ad_active()` accessor queries existing state; it does not change
ADC register, conversion or interrupt semantics.

MSTPCRA.MSTPA18 controls DAC MMIO and updates. Stopped accesses read zero
and discard writes, retaining all state (Issue #10 convention). Section
37.6.2 explicitly retains the driven analog outputs during module stop;
the QOM outputs therefore retain their last voltages. Reset clears the
data, conversion codes, pending updates and output values. DAC state,
pending timer work and stopped output values are migrated.

ELSR17 is wired to the DAC's named `elc` input. An event sets DAOE0,
without enabling DAOE1, and output starts using channel 0's code. Disabling
the link does not clear DAOE0. There is no DAC interrupt. Sections 37.2.4
and 37.5 prohibit ELC operation with DAADST=1: such input logs a guest error
and is ignored; firmware must disconnect ELSR17 before enabling DAADST.

## Reference-voltage fixture contract

Read-only QOM properties `da0-uv` and `da1-uv` expose integer microvolts.
For each enabled channel:

```
output = reference_low + floor(code * (reference_high - reference_low) / 4096)
```

This ideal output includes the low-side reference potential for a positive
span. Disabled channels, REF=000, or a nonpositive span read
zero. Zero also represents the disabled pin's high-impedance condition;
it does not mean that the physical pin is driven to ground.

The DAC links to S12AD's existing voltage fixture properties, rather than
keeping independent copies of the same physical supplies:

| REF | High property on `/machine/mcu/s12ad` | Low property | Defaults |
| --- | --- | --- | --- |
| 000 | none | none | output zero |
| 001 | `avcc-uv` | `avss-uv` | 3,300,000 / 0 uV |
| 011 | `internal-reference-uv` | `avss-uv` | 0 / 0 uV |
| 110 | `vrefh-uv` | `vrefl-uv` | 3,300,000 / 0 uV |

The 3.3 V values are explicit board assumptions inherited from Issue #31,
not a claim about every RX113 board. Runtime QOM changes to these properties
affect both converters. The shared ADC fixture already validates voltages
in the range 0..10,000,000 uV and migrates them.

Correction to the brief's premise: section 37 does not specify a numerical
internal-reference voltage, but electrical-characteristics Table 43.45
(p.1458) lists Vbgr = 1.36 V minimum, 1.43 V typical, 1.50 V maximum. To
preserve S12AD's existing behavior, this issue retains its externally
supplied, default-zero internal-reference fixture. Tests explicitly set
1,430,000 uV; this value is not silently made the machine default. A board
fixture should set that shared property to its selected/calibrated voltage.
No internal bandgap or temperature generator is synthesized.

## Synchronization choice and limits

The existing S12AD publishes an independent `scan-end` output, but no
synchronous-D/A conversion-enable signal. The brief explicitly permits
wiring the closest existing completion signal in this case. A standard
`split-irq` device fans `scan-end` out to both its unchanged ELC source 0x58
and the new DAC `sync` input. It works with ADIE=0 and does not depend on
ICU state or consume the ELC connection.

This fallback synchronizes to completion of a group-A scan, including each
continuous scan. It is not a model of the silicon's per-conversion internal
D/A-enable waveform. Group B and the first conversion of a double-trigger
pair do not emit this existing output; multi-channel/addition scans update
at scan completion rather than at intervening conversion boundaries.
A forced ADC abort does not emit scan-end. Pending data in these cases
waits for a later eligible scan-end or for DAADST to be cleared. Data newly
written while the ADC is idle still follows the one-PCLKB-cycle rule.
The ADC-stopped/DAADST=1 combination is prohibited by section 37.6.4 and
must not be used to infer physical silicon behavior.

The source never models the ADCLK-wide enable pulse's PCLKB sampling
phase, so the possible missed pulses described in Figure 37.4 are not
synthesized. This is an explicit use of the brief's fallback allowance,
not an assertion that scan-end and the internal enable signal are identical.
Under D-006, a dedicated enable/idle interface on RX113 S12AD and per-channel,
addition, group-B, double-trigger and abort tests remain a merge-review
item; estimated 0.5–1 day for digital ordering, with a separately agreed
phase convention needed to predict Figure 37.4 pulse capture exactly.

The reported voltage is an ideal conversion value. Electrical settling,
output-buffer clipping, load/impedance, inrush/noise, reference discharge
and stabilization transients, current draw, pin contention and analog
ownership of the shared reference generator are not solved. Firmware must
still follow the documented REF=000 switching sequence, internal-reference
10 us discharge / 5 us stabilization, and ADC-halted programming rules.
The model does not enforce these analog prerequisites or invent a transient
voltage. Table 43.43–45 gives only a 30 us maximum conversion settling time,
not a deterministic conversion latency; the brief requests immediate ideal
updates when synchronization is off. A calibrated analog/network model
would require board parameters and roughly 3–5 days of implementation and
characterization; integrating reference-ownership interlocks with peer
analog models would take another 1–2 days. These are explicit D-006 limits
for PMO review, following the S12AD scalar-fixture contract.

## Validation

The final RX-only build and all applicable regressions passed. Meson 1.12.1
and Ninja from `~/workspace/tools/build-venv/bin` were first on PATH; the
populated subprojects were used without re-fetching. Scratch stayed in
`.issue43-runtime/.t`. The tested binary hash is saved in
`.issue43-runtime/tested-binary.sha256`.

| Check | Result | Evidence in `.issue43-runtime/` |
| --- | --- | --- |
| Configure `--target-list=rx-softmmu --disable-docs`, `make -j$(nproc)` | PASS | `configure.log`, `fix1-build.log`, `fix1-test-build.log` |
| New DAC qtest | 8/8 PASS | `dac-qtest.log` |
| Entire `qtest-rx` and quick `func-rx` suites | 31 programs PASS, 4 SKIP, 0 failures | `regression.log` |
| RX62N U-Boot functional test | PASS | `gdbsim-functional.log` |
| Rebuild genuine FIT PLL / HOCO from pinned local ZIPs | Both PASS | `fit-build-pll.log`, `fit-build-hoco.log` |
| FIT `main`, boot then reset/boot, PLL / HOCO | Both PASS | `fit-*.log`, `verify-*/result.json` |
| SYSTEM checks without / with FIT ELF | Both PASS | `system.log`, `system-elf.log` |
| Machine help and both RX62N gdbsim machines | All listed; both running for 3 seconds, exit 0 | `machine-help.log`, `gdbsim-smoke.json` |
| Checkpatch on every changed source/build file and report | 0 errors, 0 warnings | `fix1-checkpatch-files.log` |
| Checkpatch `--no-signoff` on working-tree patch | 0 errors; MAINTAINERS advisory only | `fix1-checkpatch-patch.log` |
| Patch dry-run and exact reconstruction against HEAD | PASS, 11 files | `fix1-patch-dry-run.log`, `fix1-patch-apply.log` |
| `git diff --check` | PASS | `diff-check.log` |

For PMO fix1, the nonzero-low-reference assertions failed against the old
calculation (`fix1-test-before.log`: code zero returned 0 instead of
500,000 uV). After correction, the DAC tests and full regression commands
were rerun against the rebuilt binary. The previously accepted FIT firmware
builds were reused.

The new tests cover DACR's reset/reserved bits, both data formats, every
valid reference selection, both channels' enable states and ideal voltage
values (including nonzero VREFL and AVSS, code zero at VREFL, and zero or
negative spans), default-zero internal reference, invalid accesses, prohibited
reference encodings, ELSR17 driven by an actual DOC event with DOC interrupts
disabled, ELC disconnect, prohibited ELC/DAADST combination, ADC-synchronized
updates with ADIE=0, latest-write wins, idle-ADC one-cycle timing, clock
changes, retained stopped outputs/MMIO state, reset, and running/stopped
migration with a pending update.

The four program skips are the RX target's CD-ROM and qos tests, unavailable
user networking, and the optional linter dependency. There is also an
existing VNC crypto subtest skip. The gdbsim Linux/sash case retains its
existing `skipFlakyTest` annotation and is not a newly claimed pass. The
existing ICU, S12AD, ELC, DTC and other RX113 suites pass unchanged.
MAINTAINERS already covers `hw/rx/` and `include/hw/rx/`; no upstream
maintainer assignment is changed for this fork-only issue.

Reproduction uses the environment and commands in
`.issue43-runtime/regression.sh`, plus:

```sh
export TMPDIR="$PWD/.issue43-runtime/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
make -C build -j$(nproc)
QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx" \
  build/tests/qtest/rx113-dac-test
```

The corrected delivery is the working tree plus
`.issue43-runtime/issue-43-dac-fix1.patch` and its SHA-256 file.
`make-delivery-fix1.py` creates the patch without touching
the Git index, checks it with `git apply --check` against copies of the
original HEAD files, applies it to those copies, and compares all resulting
files byte for byte with the working tree. No commit is needed. Runtime
logs, manual, FIT assets, build products and scratch are excluded from the
patch; only the report, device, wiring and qtests are deliverables. The
existing untracked `probe.txt` is untouched.
