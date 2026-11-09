<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 20: RTC adjustment, clock output and oscillator control

Manual: R01UH0448EJ0120 Rev.1.20, sections 24 and 9.2.8. The supplied
`.issue20-runtime/rx113.pdf` was read locally using pdftotext. The original
12 RTC qtest cases and their expectations are unchanged. RX62N and shared
models are unchanged.

## Implemented behavior and manual basis

- RADJ.PMADJ=01 adds ADJ[5:0] source-clock cycles to the prescaler;
  PMADJ=10 subtracts them; PMADJ=00 does nothing (24.2.20). With AADJE=0,
  each RADJ write applies the correction once (24.3.8.2). With AADJE=1,
  the write configures subsequent automatic corrections (24.3.8.1).
- Automatic adjustment occurs on second-counter carries to multiples of
  60/10 seconds in calendar mode and 32/8 seconds in binary mode, selected
  by AADJP (24.2.18). It uses the RTC counter's phase, not elapsed host time.
  Setting PMADJ=00 stops further adjustment (24.3.8.4).
- One ADJ unit is exactly 1/32768 second (24.2.20). The fractional divider
  phase is stored in 1/128 ns units: one source cycle is 3906250 such units.
  This avoids rounding each correction to a 1/256-second tick or to whole
  nanoseconds. Only the final QEMU timer deadline is rounded up to a
  nanosecond. Subtraction delays the next divider tick without decrementing
  calendar/binary counters or replaying interrupts.
- Corrections shift the existing PRD, CUP and ALM deadlines and the LCDC
  PRD signal. Automatic adjustment boundaries are scheduled even if the
  next enabled PRD is later, so a positive correction cannot make its IRQ
  arrive late (24.5.2). Lazy reads and timer-driven counting use the same
  prescaler update path.
- RTCOUT is exposed as the read-only boolean QOM property
  `/machine/mcu/rtc.rtcout` (use QMP `qom-get` with path
  `/machine/mcu/rtc` and property `rtcout`). RTCOE=0 reports low; RTCOE=1
  reports the selected divider signal. RTCOS=0 selects R64CNT.F1HZ and
  RTCOS=1 selects F64HZ (24.2.17-18, Figure 24.1). It is evaluated at the
  current virtual time, including while no interrupt timers are enabled.
  START=0 freezes its phase; software reset/ADJ30 reset its divider phase;
  system reset clears RTCOE. RADJ shifts its edges (24.5.3). This observation
  is at the RTC peripheral output, before external pin multiplexing.
- RTCEN requests the sub-clock. The RTC's `system-subclock` GPIO input
  accepts the other request, namely !SOSCCR.SOSTP. The effective oscillator
  runs if either request is asserted (24.2.19 and 9.2.8). With START=1 and
  neither request, counting and interrupt timers stop; resuming preserves
  the fractional phase. Read-only QOM `subclock-running` exposes the result.
- RTCDV[2:0] is retained only: the manual defines it exclusively as crystal
  drive capacity for low/standard CL (24.2.19), not a divider, frequency
  selector or digital counting control. This is the explicitly permitted
  electrical-only exception, not an omitted functional RTC setting.
- The fractional phase survives system reset. Software RESET and ADJ30
  clear it with the prescaler. VMState version 2 adds phase and input level;
  version 1 remains accepted, retaining its fractional time in last_ns until
  the first synchronization. Migration itself was not added to the test
  matrix for this issue.

## Scope conflict (resolved: PMO approved the integration, 2026-11-09)

The integration proposal described below was approved by the PMO as
part of this issue's scope — it completes the RTCEN behavior this
issue was chartered to implement, stays within RX113-specific files,
and also corrects the SOSCCR reset value to the manual's 0 (9.2.8).
It has been applied to the tracked sources and re-verified there
(RTC qtest 20/20 on the tracked build).

The RTC-side oscillator behavior is implemented and tested through its
GPIO input. The input defaults to an asserted SYSTEM request, consistent
with the manual's SOSCCR reset value of 0. However, the existing SYSTEM
model in `hw/rx/rx113.c` stores SOSCCR reset as 1 and has no clock output.
Consequently the machine does not yet propagate guest SOSCCR writes to
this RTC input. Guest RTCEN alone cannot stop counting on this machine
until that connection is made.

The issue instructions limit rx113.c changes to wiring for RTCOUT. Correct
RTCEN/SOSCCR integration also needs the SOSCCR reset value corrected to 0
(9.2.8); otherwise all original tests that start with RTCEN=0 would stop
counting. A clarification was requested before expanding that scope.
The concrete, unapplied proposal is
`.issue20-runtime/rtc-system-integration.patch`: SOSCCR reset fix plus a
named SYSTEM output, connection to the RTC input, propagation on accepted
writes/reset exit/post-load, and a guest-MMIO integration qtest. It passes
`git apply --check` against the current modified worktree. It is not applied
to the tracked rx113.c or RTC test sources.

A scratch copy of rx113.c with that patch was compiled using the exact
Meson compile flags. A separate preview QEMU executable was linked from
that object and the final build's other objects. A scratch copy of the
RTC qtest adds the proposed twentieth test. The preview passes all twenty
RTC cases, including genuine guest SOSCCR writes, PRCR protection, both
clock request paths, oscillator gating/resumption and system resets.
Both PLL/HOCO FIT probes and SYSTEM probes also pass with this preview.
Thus the proposal is concrete and tested; only applying it to tracked
sources awaits the scope decision.

D-006 assessment: this remaining connection is **not heavy or complex**
(roughly 20 changed lines plus a guest-MMIO qtest, approximately 30-60
minutes including regression). Deferring it cannot be justified by D-006's
complexity exception. It is pending a scope decision; this report does not
claim complete Issue 20 acceptance while it remains unapplied.

## Existing limits

This issue does not extend the earlier RTC work to CPU-access/carry race
interrupts, asynchronous register readback, standby wake-up, or persistent
VBAT state. They are separate from the three requested register effects.
In particular, prescribed firmware waits remain software obligations:
24.2.20 says a second adjustment within 320 source cycles *may* invalidate
one; behavior of such a sequence is not specified. Tests use legal spacing.
Similarly, the model supplies an ideal nominal 32768-Hz source as soon as
it is requested. Analog crystal stabilization and drive/load/noise effects
require a board/crystal parameter model; the manual only gives typical
0.5-second stabilization, not a guaranteed digital-ready deadline
(9.2.8, Table 43.22). Modeling that faithfully is substantial analog/board
work (several days or more plus crystal characterization), not a reason to
leave RTCEN's digital start/stop behavior unimplemented.

The pre-existing limits should not be treated as D-006-approved deferrals:
CUP on a simultaneous R64CNT read and asynchronous register synchronization
are digital features, likely 1-2 worker days each with firmware-level race
and reset tests. Standby recovery needs RX CPU WAIT/ICU wake-up integration
and power-domain reset/clock tests (roughly 3-5 days). Persistence across
power loss needs machine-level backup supply/reset semantics and a host
storage policy (roughly 2-3 days). Those estimates describe the separate
work; the limited file scope and unchanged existing expectations prevent
silently folding them into Issue 20.

## Verification

Results with the current tracked-source changes:

| Check | Result | Evidence under `.issue20-runtime/` |
|---|---|---|
| RX-only build | Pass, `TARGET_DIRS=rx-softmmu` | `configure.log`, `build.log`, `rebuild-final.log` |
| RTC qtest | 19/19 pass, original 12 unchanged | `rtc-qtest.log` |
| RX qtest suite | 11 OK / 2 skipped / 0 failed | `regression.log` |
| RX functional suite | 7 OK / 2 skipped / 0 failed | `regression.log` |
| Genuine FIT PLL and HOCO, before/after reset | Pass, freshly rebuilt ELFs | `fit-build-*.log`, `fit-pll.log`, `fit-hoco.log`, `verify-*/result.json` |
| SYSTEM oscillator/protection probes, without/with ELF | Pass | `system.log`, `system-elf.log` |
| Both RX62N gdbsim machines | Listed; run for 3 s, SIGTERM exit 0 | `machine-help.log`, `gdbsim-smoke.log` |
| checkpatch on modified C/header files | 0 errors / 0 warnings | `checkpatch-files.log` |
| Whole delivery patch checkpatch | 0 errors / 1 warning (new report prompts generic MAINTAINERS check) | `checkpatch-patch.log` |
| Whitespace | `git diff --check` passes | direct check |

Functional coverage includes all eight Phase 1/part-variant boot tests,
both FPU tests and the LCDC functional test. The qtest suite includes all
five ICU tests, six LCDC tests and nineteen RTC tests. The four existing
suite skips are cdrom, qos, info_usernet and linters.

Seven added RTC cases cover maximum positive/negative one-shot adjustment
and ALM/CUP/PRD deadlines, all four automatic periods in both directions
and repeated applications, 128 legally spaced single-cycle corrections,
1-Hz/64-Hz RTCOUT edges/enable/stop/reset/adjustment, a later 2-second PRD
rescheduled by automatic correction (including LCDC), fractional phase
retention/RESET/ADJ30/no-adjustment settings, and RTCEN/drive capacity with
an injected SYSTEM request.

The untracked `test-env/sitecustomize.py` changes QEMUMachine's default
scratch directory from unwritable `/var/tmp` to TMPDIR, as in the Issue 18
follow-up; repository test helpers are unchanged. Meson runs with
`--no-rebuild` against the freshly linked executable. An unnecessary
incremental Ninja invocation again encountered build-log recovery and was
cancelled; no source changes followed the final successful product build.

The **unapplied integration proposal** has separate evidence:
`wiring-preview-qtest.log` (20/20 RTC tests),
`wiring-preview-regression-final.log` (18 OK / 4 skipped / 0 failed),
`wiring-preview-fit-{pll,hoco}.log`, and
`wiring-preview-system{,-elf}.log`. The suite wrapper selects the preview
binary; its regular RTC suite still has nineteen tests, while the separate
integration executable adds the twentieth. An initial preview-only VNC
failure was due to moving the executable away from its adjacent
`qemu-bundle` data directory (missing en-us keymap). Linking the original
build's resource directory into the preview directory fixed that test
setup, and the complete repeated suite passed. No product/test source was
changed to accommodate it. Both proposed diffs have checkpatch 0 errors /
0 warnings (`checkpatch-wiring.log`, `checkpatch-integration-test.log`).

Build and scratch stay in `build/`, `.t/`, and `.issue20-runtime/`, with
`TMPDIR=$PWD/.t` and `~/workspace/tools/build-venv/bin` first on PATH.
The FIT probes are rebuilt with the specified Issue 6 toolchain and an
existing SHA-256-verified R_BSP archive, without a new FIT download.
Configure's mkvenv installed Python test dependencies (pycotap, qemu.qmp,
setuptools/wheel/pip); it was allowed network access. No manual, source or
other external asset was newly fetched for this implementation.


## Delivery

`git add` failed with `Unable to create .../.git/index.lock: Read-only
file system` (`.issue20-runtime/git-add.log`). No commit, push or merge was
made. The tracked-source edits remain in the working tree:

- `hw/rx/rx113-rtc.c`
- `include/hw/rx/rx113_rtc.h`
- `tests/qtest/rx113-rtc-test.c`
- `docs/rx113/issue-20-rtc-regs-report.md` (new)

`.issue20-runtime/issue20-rtc-regs.patch` contains these four files,
including this report; `.issue20-runtime/changed-files.txt` lists them.
On a clean checkout at the same base, apply that patch first. The separate
`.issue20-runtime/rtc-system-integration.patch` is the tested, unapplied
scope-expansion proposal, to be applied second only after the scope decision.
It changes `hw/rx/rx113.c` and adds the twentieth RTC qtest. Do not interpret
its scratch build as an already applied working-tree change. The next
worker should apply the approved proposal, update this pending status and
rebuild from the resulting tracked sources before final delivery.
