<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 49: RX113 MTU advanced output — completion report

## Delivery and acceptance

Stages 1–3 implement the Issue #49 RX113 output path: ordinary/PWM1/PWM2
waveforms, reset-synchronized and complementary PWM, dead time and buffered
transfer/interlocks, compensation, and POE assertion/release at PORT pads.
This stage-3 delivery is relative to `eca9dd3570cfbb2b511fda1033ddb9edb8123eaa`
on `rx113/issue-49-mtu-advanced`. Validation results are recorded below.
PO/PMO review and merging remain with the repository owner.

| Acceptance area | Delivered behavior |
| --- | --- |
| Stage 1: ordinary/PWM outputs | All sixteen MTIOC signals, MPC alternatives, initial/set/clear/toggle, PWM1/PWM2 periods and duty changes, coincident matches, pad ownership and package filtering |
| Stage 2a: reset-synchronized PWM | Shared MTU3/4 count edges, six PWM pads and PSYE, polarity and timed TOLBR transfer, cycle/duty buffers and interrupt suppression |
| Stage 2b: complementary PWM | Modes 13–15, crest/trough chronology and IRQs, D-015 duty requests, dead time, hidden temporary buffers, old/new compare arbitration and transfer interlocks |
| Stage 2b: advanced controls | D-016 TWCR/CCE chronology, TDER release, TITCR/TITCNT/TBTER links, TGCR gating and MTU5 compensation/capture |
| Stage 3: POE | All seven registers, qualified/sampled POE inputs, software cutoff, physical output comparison, selective pad cutoff, OEI1/OEI2 and qualified releases |
| Stage 3: oscillator-stop source | SYSTEM OSTDCR/OSTDSR, detection, clock fallback, POE notification and both oscillator cutoff release paths |
| Reset and migration | Saved waveform/control state, sampling progress and phase, read-before-clear state, write-once locks, oscillator detection timer and request state |

D-017 explicitly limits OSTDI to request generation at the SYSTEM/ICU
boundary. CPU NMI delivery is unavailable and remains a separate shared
ICU/CPU theme, as for IWDT WUNI. No shared ICU/CPU or RX62N source is
changed. D-018 and D-019 are emulation conventions resolving manual
ambiguities, not claims about silicon. D-019 remains a candidate for
Issue #51 hardware cross-checking. These recorded boundaries do not leave
an unresolved Issue #49 implementation decision.

## Stage 3 behavior and manual evidence

Authority: supplied R01UH0448EJ0120 Rev.1.20, with printed page numbers.
The preceding audit stops are resolved by D-017, D-018 and D-019 in
`decisions.md`; that file is unchanged by this delivery. The earlier audit
patches/logs remain in `.issue49-runtime/` as historical evidence.

Section 21.2 (pp.581–588) defines ICSR1 at `0x88900`, OCSR1 at
`0x88902`, ICSR2 at `0x88908`, SPOER at `0x8890a`, POECR1 at
`0x8890b`, POECR2 at `0x8890c`, and ICSR3 at `0x8890e`.
Reserved fields read zero. Configuration fields obey their write-once
rules, including independently accessible byte fields. Status flags and
SPOER releases require a preceding read of one and a write of zero;
software cannot set hardware status flags. D-018 specifically permits
OSTSTE to be set once, cleared at any time, and never re-set before reset.

Sections 21.3.1/21.3.5 (pp.591–593) supply falling-edge detection on PCLKB
and sixteen consecutive low samples at PCLKB/8, /16 or /128. A sampled
high resets the low streak and qualifies a sampled-input flag release;
an unsampled high does not. The model keeps fractional clock phase across
clock changes. PORT qualifies all POE0#–3#/8# alternatives through bonded
pad availability, PMR and PFS=7 (Table 19.1 and section 19.2). GPIO input
levels cannot request POE without this routing.

Sections 21.2.2/21.3.2 and Table 21.3 (pp.580,583,592) define the three
comparison pairs: MTIOC3B/3D, MTIOC4A/4C and MTIOC4B/4D. POECR2 enables
the corresponding comparison/control groups; MTU TOCR1/TOCR2 and dead-time
configuration determine their active levels (Tables 20.30–37). D-019
observes every alternate pad for each function in the current package,
independent of PFS. Any alternate at the active level activates that
function's comparison input. Overlap is detected on PCLKB sampling.
OSF1 clearing requires all observed inputs in enabled groups to be
inactive, using that same observation point. Analog PFS selection cannot
hide an externally driven pad fault, while existing digital PIDR behavior
is retained.

Sections 21.3/21.5.3 (pp.589–594) select the cutoff targets: POECR1's
four independent MTU0 outputs and POECR2's three complementary pairs.
Input faults, comparator faults with OCE1, software triggers and enabled
oscillator-stop faults combine without stopping MTU counters or waveform
latches. Only PMR/PFS-selected MTU pads become high-impedance; GPIO output
drivers remain active. Removing the last qualified cutoff source restores
the currently generated waveform. OEI1/OEI2 are level requests on vectors
170/171; OSTSTF itself does not request either interrupt (Table 21.4 and
Table 14.3).

Sections 9.2.13/14 and 9.5 (pp.177–178,188–189) define protected
OSTDCR/OSTDSR access, read-before-zero OSTDF clearing, its clock-source
restrictions, forced LOCO operation under OSTDE, and notification under
OSTDIE. A board-facing SYSTEM `main-clock-failed` input represents an
external oscillator fault; it is not a direct POE test trigger. A
continuous failure with detection enabled latches OSTDF after 1 ms,
using Table 43.59's maximum detection time. Recovery before that deadline
cancels detection. Clearing OSTDF during a continuing fault permits a
new detection. A detected main-clock source falls back to nominal 4 MHz
LOCO without rewriting CKSEL; PLL operation uses Table 43.21's nominal
8 MHz free-running frequency. OSTDI is an observable named request output
and `ostdi-request` property at SYSTEM, intentionally unconnected to CPU
NMI delivery under D-017.

Notification latches POE OSTSTF and holds its clear qualification for at
least ten complete PCLKB periods, negating on a sampling edge. Enabling
OSTSTE gates cutoff; it does not gate the status latch. Clearing OSTSTF
after the notification interval and clearing OSTSTE are independent
release paths. Other latched cutoff sources continue to apply.

The applicability findings from the audits are retained: no POE module-stop
bit exists, and POE is neither an ELC source nor a destination. MSTPA9
stops MTU, not POE. No invented stop bit, ELC event or interrupt vector is
added. Software must follow section 21.5.1's prohibition on entering
software standby while using POE.

POE VMState version 1 saves registers, flags/read qualifications, locks,
sampling history, clock phase and notification qualification. PORT version
4 adds the cutoff mask; SYSTEM version 7 adds detection and request state.
MTU's existing version-11 stream layout is unchanged. Tests establish
same-build migration, including partially completed sampling and detection;
no compatibility claim is made for an old machine stream lacking POE.

## Validation

Final validation is pending completion of the running regression.

The new pad-level qtests cover the complete seven-register inventory,
reserved/write-once/read-before-clear behavior, software release with a
running waveform, all sixteen POE input alternatives, edge detection and
all three low-sampling modes, rejection of unqualified releases, sampled
glitch rejection, PCLKB division changes and independence from MTU module
stop. They check selective cutoff of every MTU0 output and each of the
three complementary groups, GPIO retention, all three comparator pairs,
active polarity, analog selection and 64-pin package filtering.

The D-019 distinguishing test leaves every MTIOC3B/3D alternative in GPIO
mode, keeps the internal outputs inactive-high, drives GPIO P22/P23 low,
and observes OSF1/OEI1. It also checks every combination of the functions'
alternate pads, rejects clearing with one active input remaining, and
observes comparator cutoff/restoration after selecting MTU pads.

The D-018 distinguishing test induces failure through SYSTEM, waits for
notification qualification, writes `ICSR3 = 0x1000`, verifies retained
OSTSTF with cleared OSTSTE and a restored MTIOC0A driver, and rejects a
subsequent OSTSTE set. The independent `OSTSTF` clear path is tested,
including rejection before ten PCLKB periods. Reset re-enables the one-set
permission. Migration checks pending low samples, saved software release
qualification, oscillator detection deadlines, subsequent cutoff and the
D-018 enable lock.

Earlier stages' manual-derived tests remain intact. They include
crest/trough and duty IRQ chronology, dead-time separation, temporary
buffer transfer and interlocks, D-015 request composition, D-016 CCE
behavior, TGCR and compensation, with physical waveform assertions and
migration. Earlier passing stage-1/2 logs are historical; final regression
results below will cover these tests again against this implementation.

Reproduce from the worktree root:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
bash .issue49-runtime/stage3d-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-poe.c hw/rx/rx113.c \
  hw/rx/rx113-port.c hw/rx/rx113-mtu.c include/hw/rx/rx113_poe.h \
  include/hw/rx/rx113_port.h include/hw/rx/rx113_mtu.h \
  include/hw/rx/rx113.h tests/qtest/rx113-mtu-test.c
```

The runtime script uses the existing `.s49` alias only for short Unix
socket paths, after verifying that it resolves to the specified runtime
TMPDIR. All build/scratch products remain in the worktree. No subprojects
were re-fetched and no scratch was put in `/tmp`.

## Patch and remaining work

The final patch will be `.issue49-runtime/issue49-stage3d-poe.patch`.
It contains the RX113-local POE device/header, RX113 PORT/SYSTEM/MTU
integration, RX build-list entry, qtests and this report. AGENTS.md,
`decisions.md`, runtime/manual/build assets, the scratch alias and all
repository metadata are excluded. Patch validation will check forward and
reverse dry runs and byte-for-byte reconstruction against the stated base.

The three audit stops are closed by the recorded decisions and the
required distinguishing tests. CPU NMI delivery remains a separate
cross-cutting theme under D-017; hardware characterization of D-018/D-019
remains outside this implementation. Existing Issue #29 exclusions such
as general input capture outside MTU5 compensation and sub-PCLK/bus
arbitration detail are unchanged. There is no new unresolved design stop.

No commit, merge, push, PR or upstream submission was performed. Changes
are left in the working tree with a separately verified patch because
repository metadata is read-only. Suggested commit subject:
`hw/rx: Implement RX113 POE cutoff and qualified release`.
