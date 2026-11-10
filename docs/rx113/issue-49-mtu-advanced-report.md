<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 49: RX113 MTU advanced output — stage 1 delivery

## Delivery status

This is a **partial delivery at the stage 1 boundary**, permitted by
`.issue49-runtime/issue-49-mtu-advanced.md`. Issue #49 is not complete.
Branch: `rx113/issue-49-mtu-advanced`; base:
`3dd72f8c11e85ad8de2bfaf6246792b5438a3f53`.

| Stage | State |
| --- | --- |
| 1: normal/PWM1/PWM2 MTIOC pad waveforms | PASS: implemented and tested; normal/PWM-applicable output controls complete |
| 2: complementary/reset-synchronized PWM | Not implemented; existing unsupported-mode diagnostics retained |
| 3: POE | Not implemented; not started before stage 2 |

The remaining coupled PWM engine is too large to implement and verify in
this bounded worker run. Delivery stops before starting its implementation,
so no partially implemented advanced mode is left in the tree. This is a
scope/time boundary, not a newly discovered unresolved hardware decision.
D-006 requires an explicit PO/PMO decision about this partial delivery;
it must not be represented as completion of the issue or its full acceptance
conditions. No commit, merge, push, PR or upstream submission was performed.
Repository metadata is read-only; changes are delivered in the working tree
and `.issue49-runtime/issue49-stage1.patch`.

## Manual evidence and implemented behavior

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
in normal/PWM1/PWM2 operation. Their register/protection behavior is present
in this delivery; their advanced waveform/transfer effects remain stage 2.
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

## Validation

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
| Stage 2 coupled PWM engine | MTU3/4 up/down chronology, TCNTS crest/trough regions, all five hidden temporary registers, old/new compare arbitration, interlocks, TDDR/TCDR/TCBR/TDER/TWCR and restart behavior, TOCR polarity/PSYE and TOLBR transfer, TITCR/TITCNT/TBTER, reset-synchronized PWM, and MTU5 measurement/capture for compensation. Approximately 5-8 engineering days, consistent with Issue #29's estimate. Do not substitute independent ordinary counters or immediate buffer copies. |
| MTU4 underflow | Still part of stage 2: ELC event 0x1a and the complementary-mode trough request on TCIV4 vector 138 must be resolved together; no extra ICU vector should be fabricated. |
| Stage 3 POE | RX113 POE register model (manual section 21), qualified PORT POE pin inputs, software and oscillator-stop sources, high-impedance cutoff, status/IRQ and documented release conditions. Approximately 2-3 engineering days after stage 2, including pad tests and SYSTEM/ICU integration. |
| Full issue acceptance | Complementary crest/trough/IRQ, dead-time separation, advanced buffer transfer, reset-synchronized interlock and POE assertion/release qtests have not run because these features are not implemented. They remain mandatory before declaring Issue #49 complete. |

Input capture outside the explicitly requested MTU5 compensation path
remains outside Issue #49. Existing Issue #29 limitations for sub-PCLK
synchronizer/bus arbitration and peer-module features are unchanged.

## Patch contents

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
