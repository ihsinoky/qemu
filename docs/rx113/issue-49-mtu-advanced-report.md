<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 49: RX113 MTU advanced output — stage 2 partial delivery

## Delivery status

Stage 1 is committed as `3f31fe9583a0ab265880b46299d2acf84afb5a24`.
This run delivers the **reset-synchronized PWM portion of stage 2**, at
an independently working boundary permitted by
`.issue49-runtime/issue-49-stage2.md` and the continuation instruction.
**Stage 2 and Issue #49 are not complete.** Branch:
`rx113/issue-49-mtu-advanced`; this patch is relative to the stage-1 commit.

| Stage | State |
| --- | --- |
| 1: normal/PWM1/PWM2 MTIOC pad waveforms | PASS: committed; existing tests retained |
| 2a: reset-synchronized PWM | Implemented: shared MTU3/4 count edges, six pads, cycle buffers, polarity/PSYE, migration and seven new qtests |
| 2b: complementary PWM and compensation | Not implemented: modes 13–15 retain unsupported-mode diagnostics; detailed next step below |
| 3: POE | Not implemented; not started |

The complementary engine, its five temporary registers and old/new compare
arbitration do not fit in the remainder of this run with adequate validation.
This delivery stops after reset-synchronized PWM; it introduces no partial
complementary counter engine or immediate-copy substitute. This is a run
size boundary, not an unresolved hardware design decision. The D-006 merge
gate remains with the PO/PMO; this report does not claim full acceptance.
No commit, merge, push, PR or upstream submission was performed. Repository
metadata is read-only. The working-tree changes and
`.issue49-runtime/issue49-stage2-reset-pwm.patch` are the deliverables.

## Stage 2 implemented subset

Authority remains the supplied R01UH0448EJ0120 Rev.1.20 manual. References
below use its printed page numbers.

* Section 20.3.7, Tables 20.52/53 and Figures 20.35/36 (pp.477–479), and
  20.6.16 (p.541): MTU3 controls both counters with CST3 and its selected
  clock/edge. CST4 and MTU4's clock/clear settings do not independently
  drive the pair. Internal clocks use a shared deadline and edge;
  external MTCLKA/B edges use the same shared-edge function. The cycle
  clear clears both counters and suppresses both overflow requests.
* Each of the three duty matches toggles its positive/negative pair;
  cycle clear toggles all three pairs. A simultaneous duty/cycle match
  toggles once, including the cycle=duty example in Figure 20.35. The
  match decisions for both channels precede count updates and transfers.
* Sections 20.2.18–20, Tables 20.30–38 (pp.434–438): TOCR1 or TOCR2
  selects output polarity. With no dead time, negative outputs are exact
  inverses and only positive polarity bits take effect. PSYE enables
  MTIOC3A, initially high, toggling at the cycle match. TOER controls its
  six documented PWM pads; it does not gate counting or the waveform
  latch. BF=01 transfers TOLBR to TOCR2 at the cycle clear, preserving BF.
* Section 20.6.15 and Figure 20.118 (p.540): MTU3 BFA/BFB control the
  buffer pairs on both channels, while MTU4 TMDR stays at its normal-mode
  initial value. All enabled cycle/duty buffers transfer at MTU3.TGRA
  compare match, after the old compare decisions. Buffered C/D registers
  generate no compare requests on either channel. Incorrectly setting
  MTU4's BFA/BFB inhibits its corresponding C/D output as documented.
* Section 20.6.26 (p.547): PCLK/1 zero-cycle compare requests stay
  asserted for both channels, even with CST4 clear and MTU4's stored TCR
  selecting neither that clock nor that clear source. Clearing ICU.IR
  cannot manufacture another request edge while the source stays flat.
* Section 20.7.3, Figure 20.152 step (10) (p.572): counter stop restores
  the initial reset-PWM output. Module stop retains the waveform and
  fractional clock state. Section 20.8.2 normal-mode ELC operation is
  rejected for the controlled MTU4 as well as MTU3.

Migration version 4 saves the three waveform phases independently of
polarity, in addition to the existing count, fractional clock, output and
control state. Same-build migration is checked between duty and cycle
matches, including a pending cycle-buffer change. Old streams never
implemented reset PWM; no advanced-waveform compatibility claim is made
for those streams. RX62N and all other device source files are untouched.
Brushless motor gating (TGCR) is not added by this subset.

## Stage 2 validation

Validation is recorded in the `stage2-*` runtime logs; the stage-1 logs
and results below are historical and have not been overwritten.
| Check | Result and evidence in `.issue49-runtime/` |
| --- | --- |
| RX-only rebuild with supplied tools | PASS: `stage2-build.log` |
| MTU qtest | 37/37 PASS, including seven new cases: `stage2-mtu.log` |
| All configured RX qtests and quick functional tests | 32 suites OK, 4 existing skips, 0 failures: `stage2-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2-gdbsim-functional.log` |
| Genuine cached FIT PLL/HOCO, before and after reset | Both PASS: `stage2-fit-pll.log`, `stage2-fit-hoco.log` |
| SYSTEM protection/timing, without/with FIT ELF | Both PASS: `stage2-system.log`, `stage2-system-elf.log` |
| Both gdbsim machines listed and run three seconds | Both PASS: `stage2-machine-help.log`, `stage2-gdbsim-smoke.log` |
| Changed C files and raw-diff checkpatch | PASS, zero errors and warnings: `stage2-checkpatch-files.log`, `stage2-checkpatch-patch.log` |
| Patch forward/reverse dry runs and reconstructed-byte comparison | PASS: `stage2-patch-validation.log` |

`stage2-tested-binary.sha256` identifies the tested binary. Reproduce with:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
build/tests/qtest/rx113-mtu-test
bash .issue49-runtime/stage2-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c \
  tests/qtest/rx113-mtu-test.c
```

The raw-diff check uses `--no-signoff` for the uncommitted deliverable.
The patch contains exactly the three files listed below, relative to the
stage-1 commit. Validation reconstructs those base files in scratch,
checks forward application, applies and compares all reconstructed files
byte-for-byte with the working tree, then checks/applies the reverse patch
and compares with the original base files, using `git apply` with an
explicit scratch directory. This does not write `.git`. GNU `patch` passed
its forward dry run but could not preserve ownership during reconstruction
in this sandbox (`stage2-patch-validation-ownership.log`); `git apply`
avoids that unrelated metadata operation.

The seven new tests observe physical PORT pads and PIDR. They cover three
successive cycles, all six phases and PSYE, one nanosecond before edges,
CST4-only inactivity, CST3-only paired counting with a conflicting MTU4
clock, shared cycle clear, all four buffers and buffered-IRQ suppression,
per-phase polarity and timed TOLBR transfer, TOER/PSYE gates, cycle=duty
coincidence at FFFFh with overflow suppression, module stop at a fractional
count, prohibited ELC restart, external clock edges, reset, zero-cycle
flat requests, and live buffered-waveform migration. The original 30 MTU
cases and their assertions remain in place.

The zero-cycle test initially exposed an ordering defect: the MTU4 flat
request check observed MTU3 after its increment. The shared tick now takes
all compare decisions before updating either counter. The failing attempt
is retained as `stage2-mtu-zero-first.log` and
`stage2-regression-zero-first.log`; neither is counted as a passing check.
All scratch uses the supplied runtime TMPDIR (or its `.s49` alias for the
Meson socket suites); no subprojects were re-fetched.

## Stage 1 manual evidence and implemented behavior (historical)

Authority: the supplied R01UH0448EJ0120 Rev.1.20 manual, retained as
`.issue49-runtime/rx113.pdf` and `rx113.txt`. The following references are
printed manual page numbers, not PDF viewer offsets.

| Reference | Behavior |
| --- | --- |
| 20.2.3, Tables 20.12-27, pp.408-417 | TIOR initial low/high, set/clear/toggle, output prohibition and capture/buffer exclusion for all 16 MTIOC signals |
| 20.2.14, p.430 | Stopping a counter retains the output; writing TIOR while stopped reinstates its initial level |
| 20.2.16-17, pp.432-433 | TOER gates its six documented signals; reserved bits read one; TRWER protects TOER and TOCR1/2 |
| 20.2.18-20, pp.434-438 | TOCR1/2 and TOLBR masks/reset/readback, set-once TOCL protection; PSYE remains writable after TOCL |
| 20.3.3/20.3.5, pp.461, 467-470 | PWM1 A/B and C/D pairing onto A/C; PWM2 duty pads and clear-to-initial behavior; buffered duty changes and simultaneous-match retention |
| 20.5.2, p.529; 20.6.26, p.547 | Existing compare-edge timing retained; zero-period optimization preserves first output transitions and observable toggles |
| 18.3, 19.2, Tables 19.2-21, pp.353-395 | All documented MTIOC pad alternatives, package filtering, PMR/PFS selection, GPIO ownership, existing open-drain/input resolution and 64-pin PSRA behavior |

TOCR polarity, PSYE waveform generation and TOLBR-to-TOCR2 transfer apply
**only to the advanced modes** (20.2.18-20). They have no waveform effect
in normal/PWM1/PWM2 operation. Their register/protection behavior was present
in stage 1; reset-synchronized waveform/transfer effects are now added by
the stage-2 subset above. Complementary effects still remain.
This is not a claim that storing these fields implements advanced PWM.

Manual corrections resolved without inventing a hardware policy:

* Reset-synchronized PWM is 20.3.7 (pp.477-479), complementary PWM is
  20.3.8 (pp.480-510), and dead-time compensation is 20.3.11 (pp.516-519).
  The brief's reference to 20.3.11 for reset-synchronized PWM is inaccurate.
* Tables 19.17/18 print `MTIOC4BD` for PD2. The package pin lists, including
  Table 1.5, consistently identify PD2 as MTIOC4D. The routing uses MTIOC4D.

MTU outputs are sixteen level wires and sixteen enable wires, connected to
RX113 PORT. Tests observe PORT's physical `levels`/`driving` properties and
PIDR, rather than an internal timer latch. PORT retains its existing GPIO,
analog selection, external input, open-drain and package behavior. No shared
peripheral or RX62N source was modified.

Matches from one count edge are evaluated together. In PWM1, equal paired
matches leave the pad unchanged. In PWM2, a duty match coincident with
its own or a synchronous cycle clear also leaves that pad unchanged;
other duty pads restore their TIOR initial level. This preserves the
manual's history-dependent 0%/100% cases. Buffer transfers do not replace
already-decided match events. PWM2 zero writes and overflow also restore
initial levels. Both counter stop and module stop retain pad outputs.

Migration version 3 saves the timer output latch and output controls, and
the PORT signal inputs. Derived pad state is restored on load. Same-build
migration of a live PWM waveform is tested at a fractional count interval;
older streams do not contain a saved waveform latch and are not evidence
of waveform restoration. Existing migration, phase counting, cascade, ELC,
clock and module-stop tests remain unchanged.

## Stage 1 validation (historical)

| Check | Result and evidence in `.issue49-runtime/` |
| --- | --- |
| RX-only configure and build | PASS: `configure.log`, `build-final.log` |
| MTU qtest | 30/30 PASS, including nine new pad cases: `mtu-first.log`, `regression.log` |
| All configured RX qtests and quick functional tests | 32 suites OK, 4 existing skips, 0 failures: `regression.log` |
| All RX113 parts, reset, RX113/RX62N FPU, LCDC and IWDT functional | PASS within `regression.log` |
| Existing peripheral qtests, including DTC/ELC/USB/CTSU | PASS within `regression.log`; existing test expectations unchanged |
| Genuine FIT PLL/HOCO, before and after reset | Both PASS: `fit-build-*.log`, `fit-pll.log`, `fit-hoco.log`, `verify-*/result.json` |
| SYSTEM protection/timing, without/with FIT ELF | Both PASS: `system.log`, `system-elf.log` |
| Both gdbsim machines listed, run three seconds and exit cleanly | Both PASS: `machine-help.log`, `gdbsim-smoke.json` |
| Existing gdbsim U-Boot functional | PASS: `gdbsim-functional.log`; existing Linux/sash flaky annotation unchanged |
| Changed-file checkpatch | PASS: zero errors and warnings; `checkpatch-files.log` |
| Raw-diff checkpatch | Zero errors, one generic new-report MAINTAINERS warning; `checkpatch-patch.log` |
| Patch forward/reverse dry runs and byte comparison | PASS: `patch-validation.log`; reconstructed files exactly match this tree |

The nine new cases cover all 16 outputs' initial/set/clear/toggle states,
PWM1 A/C pairing on all five channels, PWM2 on MTU0..2, three consecutive
periods, one nanosecond before transitions, history-dependent 0%/100%,
simultaneous paired matches, counter-stop retention, return to normal mode,
TOER's six gates, TRWER and TOCL, reset, synchronous PWM2 clearing,
buffered PWM1 duty/period changes, module-stop retention, zero-period
first transitions and toggles with IRQs disabled, alternate MPC routes,
64-pin PSRA/unbonded pads, and live pad-waveform migration. The original
21 MTU tests retain their assertions and all pass.

The raw-diff check uses `--no-signoff` because this deliverable is an
uncommitted diff, not a signed commit/mail patch. Commit preparation is
left to the PMO. Its sole MAINTAINERS warning concerns the new fork-local
report; no implementation style warning remains.

`tested-binary.sha256` identifies the binary used by all regression and FIT
checks. Build dependency metadata damaged by the initial overlapping
builds is separately regenerated in `build-cache-clean.log`; the binary
and MTU-test executable comparison is recorded in
`build-hash-comparison.log`. Both rebuilt executables are byte-identical
to those used for the passing checks.

All scratch/build products are in this worktree; the primary `TMPDIR` is
`.issue49-runtime/.t`. The generic socket suites require a shorter Unix
socket pathname than the absolute runtime path permits. For the Meson
regression invocations only, `regression.sh` uses the short `.s49` symlink
as TMPDIR; it verifies that this resolves to the exact same
`.issue49-runtime/.t` directory. No scratch is placed in `/tmp`. Subprojects were already present and were not
re-fetched. FIT probes use the unchanged pinned-hash build script and
cached vendor archives. The scratch Python hook supplies TMPDIR to
QEMUMachine's temporary-directory argument. It does not change device code
or test expectations.

Overlapping initial build commands caused an invalid archive at link
time. The duplicate commands were interrupted, the archive moved aside,
and validation uses a subsequent single build. The failed attempt's logs
are retained; it is not counted as a passing build. The first full suite
also exposed Unix socket path limits in generic monitor/netdev tests
(`regression-first*.log`), resolved by the short alias above. The first
gdbsim attempt could not update a timestamp in the read-only neighboring
cache; copying the unchanged cached U-Boot asset into this worktree's
writable cache resolved it (`gdbsim-functional-first.log`). No test was
excluded or assertion changed to obtain the final green results.

Reproduce from the worktree root:

```sh
. .issue49-runtime/env.sh
# For a fresh build directory:
# mkdir build && (cd build && ../configure --target-list=rx-softmmu --disable-docs)
ninja -C build -j8
build/tests/qtest/rx113-mtu-test
bash .issue49-runtime/regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c hw/rx/rx113-port.c \
  hw/rx/rx113.c include/hw/rx/rx113_mtu.h include/hw/rx/rx113_port.h \
  tests/qtest/rx113-mtu-test.c
```

## Remaining work and D-006 merge gate

The required order remains stage 2, then stage 3. Engineering estimates
include manual-based qtests and regression, not just register storage.

| Remaining scope | Work and estimate |
| --- | --- |
| Stage 2 coupled PWM engine | MTU3/4 up/down chronology, TCNTS crest/trough regions, all five hidden temporary registers, old/new compare arbitration, interlocks, TDDR/TCDR/TCBR/TDER/TWCR and restart behavior, TOCR polarity/PSYE and TOLBR transfer, TITCR/TITCNT/TBTER, and MTU5 measurement/capture for compensation. Reset-synchronized PWM is now delivered separately above. Approximately 5-8 engineering days, consistent with Issue #29's estimate. Do not substitute independent ordinary counters or immediate buffer copies. |
| MTU4 underflow | Still part of stage 2: ELC event 0x1a and the complementary-mode trough request on TCIV4 vector 138 must be resolved together; no extra ICU vector should be fabricated. |
| Stage 3 POE | RX113 POE register model (manual section 21), qualified PORT POE pin inputs, software and oscillator-stop sources, high-impedance cutoff, status/IRQ and documented release conditions. Approximately 2-3 engineering days after stage 2, including pad tests and SYSTEM/ICU integration. |
| Full issue acceptance | Complementary crest/trough/IRQ, dead-time separation, advanced complementary buffer transfer/interlock and POE assertion/release qtests have not run because these features are not implemented. They remain mandatory before declaring Issue #49 complete. |

Input capture outside the explicitly requested MTU5 compensation path
remains outside Issue #49. Existing Issue #29 limitations for sub-PCLK
synchronizer/bus arbitration and peer-module features are unchanged.

## Next implementation step

Start with a manual-derived transition table for the coupled complementary
engine (20.3.8(a)–(c), Figures 20.39–42, pp.483–487), including Ta/Tb1/Tb2
and TCNTS activation, crest/trough direction changes and the initial
TDDR offset. Add all five hidden temporary registers and their pending
write transaction: MTU4.TGRD is the commit write, Ta accepts it immediately,
Tb defers it, and only the selected end-of-Tb boundary transfers it to
active registers. Do not reuse reset PWM's direct cycle-buffer transfers
for this path. Then implement the documented old/new compare arbitration
and interlocks, with crest/trough pad/IRQ tests before enabling modes
13–15. Include MTU4 underflow ELC 0x1a and TCIV4 vector 138 together in
that engine; the current delivery deliberately does not change either.

Still mandatory in stage 2: TDDR/TCDR/TCBR/TCNTS and hidden-state migration;
TDER read-before-clear and TWCR read-before-set/restart behavior;
complementary TOCR/PSYE and TOLBR transfers; TITCR/TITCNT/TBTER skip and
transfer links; and the MTU5 measurement/capture compensation path in
20.3.11 (pp.516–519). These registers/functions are **not implemented by
this patch**. Their absence is not masked by register-only storage.
Retain the unsupported diagnostics until the corresponding engine is
validated. Stage 3 POE must wait for stage 2 completion.

## Current patch contents

* `hw/rx/rx113-mtu.c`
* `tests/qtest/rx113-mtu-test.c`
* `docs/rx113/issue-49-mtu-advanced-report.md`

The patch is relative to `3f31fe9583a0ab265880b46299d2acf84afb5a24`.
It excludes AGENTS.md, runtime/manual/build assets, the scratch alias and
repository metadata. Suggested commit subject:
`hw/rx: Drive RX113 reset-synchronized PWM from a shared count edge`.

## Stage 1 patch contents (historical)

* `hw/rx/rx113-mtu.c`
* `hw/rx/rx113-port.c`
* `hw/rx/rx113.c`
* `include/hw/rx/rx113_mtu.h`
* `include/hw/rx/rx113_port.h`
* `tests/qtest/rx113-mtu-test.c`
* `docs/rx113/issue-49-mtu-advanced-report.md`

The patch excludes AGENTS.md, pre-existing `probe.txt`, the `.s49` scratch
alias, runtime assets,
manuals, toolchains, build products and repository metadata. Suggested
commit subject: `hw/rx: Drive RX113 MTIOC pads in normal and PWM modes`.
