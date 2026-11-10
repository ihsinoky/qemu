<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #39: RX113 CAC implementation

Date: 2026-11-10. Status: implementation and acceptance gates complete.
Branch: `rx113/issue-39-cac`.
Base: `d35b83b45d6d0dd6d5f3ab661c003a05129582da`.
Authority: `.issue39-runtime/issue-39-cac.md`, its v2 supplement (including
PMO's transcription of D-009), the fix1 return instructions, root AGENTS.md,
and R01UH0448EJ0120 Rev.1.20 section 10, printed pages 198-208. This replaces the initial
policy-blocked audit; D-009 resolves its blocking decision.

## Implemented behavior

The RX113-only CAC device replaces the 0x0008b000 stub. Register widths,
reset values, reserved bits, read-only registers and write-one flag clears
follow sections 10.2 and Table 5.1. The first selected reference edge starts
the 16-bit counter without MENDF. Later valid edges transfer the interval
count to CACNTBR, compare inclusive bounds, then clear the counter and its
fractional count phase for the next interval. Figure 10.2 (printed page 206)
shows this sawtooth counter trace at edges (3), (4) and (5), capturing
7FFFh, BFFFh and 3FFFh respectively. These are separate interval counts.
The earlier cumulative-counter interpretation was incorrect. Overflow wraps
the counter and sets OVFF when it overflows within an interval; captures
do not clear the sticky OVFF flag.
Writing CFME=0 clears the counter, stops measurement and preserves CACNTBR
and status flags. All target/reference selections, dividers and valid edge
selections are implemented.

The three level outputs are wired directly to ICU vectors 32, 33 and 34,
without DTC activation. Each output is exactly its flag AND enable. Module
stop gates time and MMIO (reads zero, writes discarded), retaining the
counter, registers, fractional clocks, filter and interrupt state. Reset
clears device state. Migration preserves clocks, fractional accounting,
reference polarity, filter samples, flags and counter; post-load restores
interrupts and scheduling. SYSTEM's migration version increases to 5 for
RTC clock ownership and the IWDT startup deadline.

Elapsed-clock accounting retains fractional cycles, uses wide integer
arithmetic, and schedules reference half-cycles, overflow and pending filter
samples. Late callbacks replay reference/filter events chronologically.
Stable filters need no periodic callbacks. Events use nanosecond virtual
time, with fractional phase retained within each count interval and cleared
after capture; absolute oscillator phase and analog frequency variation are
outside the existing clock model. Internal divider
phase begins at enable. Filter clock configuration establishes a sampling
phase; it does not claim synchronization with a physical oscillator phase.

## D-009 and the manual's open points

- Writes to CACR1, CACR2, CAULVR and CALLVR with CFME=1 are ignored,
  retain their original values, and log a guest error. This is **an
  emulation convention**, not a guarantee of undocumented silicon behavior.
  The qtest attempts all four writes during an interval and checks both
  readback and completion with the original configuration and limits.
- CFME reflects immediately. Section 10.2.1 specifies neither the delay
  duration nor its clock domain; D-009 explicitly chooses immediate
  reflection. Repeated enables do not restart an active measurement.
- The digital filter implements three consecutive matching samples, with
  DFS selecting the undivided target clock, target /4, or target /16.
  DFS=0 bypasses it. Short pulses are rejected; accepted edges are delayed
  to the third matching sample. CFME gates counting, leaving filter
  sampling history intact. Module stop pauses the filter. Tests check
  rejection and delayed captures for all three DFS clocks, and filter
  history across CFME changes.

Prohibited FMCS/RSCS encodings supply no clock; prohibited EDGES produces
no valid edges. No hardware behavior is promised for prohibited settings.
The unspecified sub-clock analog stabilization time remains firmware's
responsibility, as in the existing RTC clock model.

## Clock and pin audit follow-ups

CAC consumes six live frequencies from SYSTEM, not local nominal constants.
Main and HOCO are available only with their stability flags set; stopping
or restarting them propagates immediately, including at stabilization timer
deadlines. LOCO follows LOSTP. IWDTCLK uses the existing IWDT's actual
clock-start state (including automatic mode, whose ILOCOCR readback alone
cannot establish availability). Manual startup is withheld for the existing
50 us tILOCO maximum; repeated writes do not restart that delay. PCLKB
tracks SYSTEM's source/divider and is unavailable to CAC if its selected
oscillator is stopped or unstable. Existing peripheral clock conventions
are retained; no shared device or RX62N code is changed.

Section 9.2.8 and section 9.8.5 define sub-clock operation as SOSTP=0 OR
RTCEN=1. The RTC publishes RTCEN to SYSTEM; either owner keeps CAC's sub
source available. With both owners off it stops. The tests specifically
exercise RTCEN with SOSTP=1. No numeric sub-oscillator startup delay is
invented; the existing model represents the nominal 32768 Hz signal.

CAC exposes named input `cacref`. PORT publishes a selected CACREF signal,
wired to that input. Tables 19.3-19.16 select P15, P27, PA0 or PC7 with
PSEL=7 and PMR=1; ASEL suppresses digital input. PORT's existing package
masks apply before selection. All four documented CACREF pads exist in
both modeled packages (64 and 100 pins). Unselected pads cannot drive CAC;
selected pad levels are combined consistently with PORT's existing input
aggregation. Section 19.1 prohibits assigning one function to multiple
pins; aggregation is not a guarantee of silicon behavior for that prohibited
configuration. The qtest covers all four routes in both packages, wrong
PSEL, PMR gating and P27 analog selection.

## Validation

All prescribed gates passed on the delivered sources. Evidence is retained
under `.issue39-runtime/`:

| Gate | Result | Evidence |
| --- | --- | --- |
| rx-softmmu build | PASS, exit 0 | `configure.log`, `build-fix1.log` |
| CAC qtest | 14/14 PASS standalone and in suite | `cac-test-fix1.log`, `regression.log` |
| All RX qtests and quick functional tests | 29 OK, 0 failures, 4 existing skips | `regression.log` |
| RX62N U-Boot functional test | PASS | `gdbsim-functional.log` |
| FIT PLL and HOCO boot, then reset/boot | Both PASS | `fit-pll.log`, `fit-hoco.log`, `verify-*/result.json` |
| SYSTEM oscillator/protection probes, with and without FIT ELF | Both PASS | `system.log`, `system-elf.log` |
| Machine help | Both gdbsim and four RX113 machines listed | `machine-help.log` |
| Both gdbsim smoke runs | Running for 3 seconds, clean exit 0 | `gdbsim-smoke.json` |
| Changed C/header file checkpatch | 0 errors, 0 warnings | `checkpatch-files-fix1.log` |
| Final patch checkpatch | 0 errors, 1 generic MAINTAINERS warning | `checkpatch-patch-fix1.log` |
| Patch forward/reverse dry runs, git apply check and byte comparison | PASS | `patch-validation-fix1.log`, `package-fix1.log` |

The four suite skips are cdrom-test, qos-test, info_usernet and linters,
matching the existing RX configuration. RX113 IRQ, LCDC, PORT, SCI, clock,
RTC, IWDT, MTU, S12AD, DTC, ELC and DOC regressions all passed. The generic
checkpatch warning asks about new-file ownership; existing Renesas RX
peripheral entries cover the new source/header. Fork-local documentation
and qtests follow the existing project layout; no maintainer was assigned.

Fix1 adds consecutive measurements without changing configuration or CFME:
external measurements across every divider/edge selection, repeated internal
reference intervals, and the exact Figure 10.2 count/limit pattern. Repeated
16.5-cycle intervals verify that partial count phase is cleared with the
counter. The overflow test checks the new interval immediately before and
at its first wrap. Source-stop and IWDT expectations now use interval
counts. All validation in the table was rerun after the correction.

The first CAC test attempts found harness errors (outbound IRQ interception
disconnected ICU wiring, and incoming qtest time needed explicit alignment).
These were corrected using ICU input observation and `qtest_clock_set`,
following existing qtest conventions. No product failure was suppressed.
Initial logs remain as `cac-initial.log` and `cac-other-initial.log`.
Ninja recovered truncated metadata during the serialized final rebuild,
which completed successfully; the earlier overlapping builds are not used
as build evidence. No subprojects were fetched.

The regression and delivered binary checksums match, recorded in
`regression-binary.sha256` and `tested-binary.sha256`. FIT ELFs and the
pinned U-Boot cache were reused from the existing verified local assets.
All build/test scratch uses `.issue39-runtime/.t` (the suite uses its
relative spelling from `build/` to keep Unix socket names short).

Reproduce from the worktree root:

```sh
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
export TMPDIR="$PWD/.issue39-runtime/.t"
make -C build -j$(nproc)
QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx" build/tests/qtest/rx113-cac-test
bash .issue39-runtime/regression.sh
python3 .issue39-runtime/package-fix1.py
scripts/checkpatch.pl --no-tree --no-signoff .issue39-runtime/issue-39-cac-fix1.patch
```

## Remaining work and delivery

No documented CAC function is intentionally deferred under D-006. No new
unresolved policy decision was needed. Physical oscillator phase, analog
variation and electrical pulse limits need a hardware comparison if those
become requirements; they are beyond the existing frequency-based SYSTEM
model. Estimate: 1-2 days to design a shared clock-phase representation,
plus hardware-dependent characterization time; this is not a CAC acceptance
waiver. No implementation follow-up is currently requested.

The deliverable is the working tree plus
`.issue39-runtime/issue-39-cac-fix1.patch`, based on the HEAD above. Runtime
inputs, logs, test fixtures and the old audit patch are excluded from the
final patch. The completed report is included. Packaging checks forward
and reverse dry-run application and compares applied files byte for byte.
`AGENTS.md`, RX62N and shared peripheral implementations are excluded.
No commit, push, PR, merge or upstream action is performed; PMO can commit
and prepare the fork-local draft PR after reviewing this delivery.
