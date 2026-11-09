<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 29: RX113 MTU2a timer model

## Delivery and scope

Implemented on `rx113/issue-29-mtu`, based on `d723e80f43`.
The former 0x00088600..0x000888ff stub is replaced by an RX113-only
SysBus device. All eight counters (MTU0..4 and MTU5 U/V/W), their 21
comparators and all 28 ICU interrupt sources operate. Normal mode and
PWM modes 1/2 have count, compare, clear and buffer-transfer behavior.
PCLKB updates and MSTPCRA.MSTPA9 use the existing RX113 SYSTEM path.

No shared device, RX62N implementation, existing test guest, FIT source or
existing test expectation was changed. No push, merge or upstream action
was performed. The repository metadata is read-only: `git add` failed
creating `.git/index.lock` with `Read-only file system`
(`.issue29-runtime/git-add.log`). Consequently there is no commit;
the changes remain in the worktree, accompanied by the import patch
`.issue29-runtime/issue29.patch` and exact file list
`.issue29-runtime/changed-files.txt`. Suggested commit subject:
`hw/rx: Model the RX113 multi-function timer`.

## Manual evidence and implemented behavior

Authority: local `.issue29-runtime/rx113.pdf`, Renesas R01UH0448EJ0120,
Rev.1.20. Text extraction and rendered phase-counting tables are retained
under `.issue29-runtime/`. AGENTS.md and decisions D-006/D-007 were read.

| Manual reference | Implementation |
| --- | --- |
| 20.1, Tables 20.1/20.2, pp.398-400 | Eight 16-bit counters; per-channel comparator counts and clock selections |
| 20.2.1, Tables 20.4-20.10, pp.403-405 | TCR prescalers, single/both-edge selection, all compare-clear selectors and synchronous clear |
| 20.2.2/20.2.3, pp.406-418 | Channel-specific TMDR masks; normal/PWM modes; TIOR selects comparison versus capture |
| 20.2.4-20.2.7, pp.419-424 | MTU5 compare clear; TIER/TIER2; TSR direction; TBTM transfer timing |
| 20.2.12-20.2.16, pp.428-432 | TCNT/TGR widths/reset values, TSTR, TSYR simultaneous writes, TRWER read-before-clear protection |
| 20.3.1-20.3.5, pp.450-470 | Free running/periodic counts, synchronous operation, buffers, cascade, PWM compare/clear timing |
| 20.3.6, Tables 20.48-20.51, pp.471-475 | Raw MTCLK input phase counting modes 1-4 on MTU1/2 |
| Table 14.3 p.269; 20.4 pp.520-521 | Correct edge vectors 114..141, individually enabled comparison/overflow/underflow |
| 20.5.2 pp.529-531; 20.6.3 p.532 | MTU0..4 compare on leaving matching count; MTU5 compare on entering matching count |
| 20.6.17 p.542 | Own or synchronous clear suppresses a simultaneous overflow/underflow, without suppressing compare IRQs |
| 20.6.24 p.544 | Writing stopped MTU5 TGR = TCNT+1 generates a compare request and optional clear |
| 20.6.26 p.547 | Zero-period PCLK/1 comparison produces a flat interrupt request, not repeated ICU edges |
| 11.2.2 p.214; 11.4 p.227; 11.7.3 p.236; 20.6.1/20.6.21 | PRC1-protected MSTPA9 stops MTU0..5 together, retaining state and pending ICU requests |

Clock accounting retains fractional count work as a numerator over
`divider * 10^9`. Before changing frequency/gate or servicing MMIO,
the device accounts for elapsed virtual time using the previous clock.
Halving PCLKB doubles the remaining time for the same fractional work;
stopping preserves that work. Resume continues it. Divided internal
clocks count twice as fast with both-edge selection; PCLK/1 ignores CKEG
as documented. TCR/TMDR programming establishes a new count-clock phase;
the manual requires those registers to be programmed with counting stopped.

The next observable compare or wrap schedules one virtual timer. Delayed
callbacks are processed in chronological order, with simultaneous channel
events evaluated before synchronous clearing. This also handles TCG's
coarser callback delivery without losing intervening counter cycles.
The flat zero-period case can be advanced arithmetically once its request
is asserted, so a long clock step does not execute millions of identical
callbacks. A one-year qtest checks this path.

MTU0/3/4 buffers transfer C->A and D->B on comparison or, in PWM mode,
on clear according to TBTM. Buffered C/D do not compare. MTU0 F->E is
also implemented; F still compares while acting as a buffer. Compare
decisions precede transfers, so a new period applies to the next cycle.
TSYR synchronous writes apply to all selected TCNTs, and selected
synchronous-clear followers clear with the source channel.

Interrupt mapping follows Table 14.3 exactly:

| Channel | Vectors |
| --- | --- |
| MTU0 | A/B/C/D 114..117; overflow 118; E/F 119/120 |
| MTU1 | A/B 121/122; overflow/underflow 123/124 |
| MTU2 | A/B 125/126; overflow/underflow 127/128 |
| MTU3 | A/B/C/D 129..132; overflow 133 |
| MTU4 | A/B/C/D 134..137; overflow 138 |
| MTU5 | U/V/W 139/140/141 |

TSR contains the read-only direction flag, **not** sticky compare flags.
IRQ observations therefore use ICU.IR. Underflow is possible in phase
counting and cascaded down-counting, not ordinary ascending normal/PWM
operation. To test real underflow sources without changing PORT, the
MTU exposes four ideal, already-qualified `mtclk` GPIO inputs. These
implement external count edges and phase modes 1-4, including TCFD and
MTU2->MTU1 overflow/underflow cascading. They are not wired to board pads.

During module stop MMIO reads return zero and writes are discarded,
matching the existing Issue 10 emulation policy. Protected TRWER registers
also read zero when access is disabled; hardware documents those reads
as undefined. Neither zero-read policy claims a hardware-defined value.
Reset cancels events, clears counters/control and initializes TGR to FFFFh;
SYSTEM reset exit supplies the reset frequency/gate. Migration saves
registers, fractional work, clock/gate, raw inputs and continuous-request
state, then derives deadlines on load. Already-delivered edge requests
are not reasserted on load, avoiding phantom ICU interrupts.

## PWM acceptance observation

No output-pad waveform is generated by this issue. The observable PWM
contract is the cycle/duty comparison times, the counter value and the
buffered register updates. Section 20.3.5 defines PWM periods and duty
transition points by exactly those comparisons and counter clears.

The qtest programs PCLKB=32 MHz, /4, TGRA=99, TGRB=24, clear on A,
and TIOR A=low/B=high. For MTU0..4 PWM1 and MTU0..2 PWM2 it checks
three consecutive periods, including one nanosecond before each event:

* B duty comparison at `(24+1)*125 = 3125 ns`;
* A cycle comparison and TCNT=0 at `(99+1)*125 = 12500 ns`;
* the interval between B and A is 9375 ns: the specified logical
  high interval is 75% of the 12500 ns period for these TIOR settings.

This validates the register-level transition schedule, not a physical
pin level. A separate buffered PWM case changes A from 99 to 199 and B
from 24 to 49 at clear, then verifies the next 25000 ns period and
suppressed C/D compare requests. MTU5's distinct clear period is N ticks,
rather than N+1, and is tested separately for U/V/W.

## Verification

All final tests use `build/qemu-system-rx`; its digest is retained in
`.issue29-runtime/tested-binary.sha256`. Logs and reproduction scripts
are under `.issue29-runtime/`.

| Check | Result | Evidence |
| --- | --- | --- |
| RX-only configure and build | PASS | `configure.log`, `build.log`, `build-final.log` |
| New MTU qtest | 21/21 PASS | `mtu-final.log`, `regression.log` |
| RX qtest + quick functional suites | 24 OK, 4 existing skips, 0 failures | `regression.log` |
| Phase 1 / all four parts / reset | 8 PASS | `regression.log` |
| RX113/RX62N FPU | 2 PASS | `regression.log` |
| LCDC functional and qtest | 1 / 6 PASS | `regression.log` |
| RTC / PORT / ICU qtests | 20 / 6 / 5 PASS | `regression.log` |
| IWDT functional and qtest | 2 / 49 PASS | `regression.log` |
| Clock / SCI qtests | 9 / 3 PASS | `regression.log` |
| Genuine FIT rebuilt PLL/HOCO, before/after reset | 2 configurations PASS | `fit-build-*.log`, `fit-pll.log`, `fit-hoco.log`, `verify-*/result.json` |
| SYSTEM delay/protection, without/with FIT ELF | 2 PASS | `system.log`, `system-elf.log` |
| Both gdbsim machines listed; run 3 seconds and exit cleanly | 2 PASS | `machine-help.log`, `gdbsim-smoke.json` |
| Existing gdbsim U-Boot functional | PASS | `gdbsim-functional.log` |
| Changed-file checkpatch | 0 errors, 0 warnings | `checkpatch-files.log` |
| Whitespace / import patch validation | PASS | `patch-validation.log` |

Patch-level checkpatch also reports zero errors, with one generic
new-file MAINTAINERS warning (`checkpatch-patch.log`). Existing RX entries
already cover `hw/rx/` and `include/hw/rx/`; MAINTAINERS was not modified
outside the RX113-only scope.

The MTU cases cover every comparison vector, all overflow/underflow
vectors, all compare-clear selectors, all internal prescalers and both
edges, normal/PWM transfers (including E/F), sync writes/clears and wrap
precedence, masking, fractional PCLKB/gate changes on every counter,
external edges, all four raw phase modes, bidirectional cascading,
TRWER/access width/reset, the MTU5 stopped-counter special case,
continuous-request buffering, running/stopped migration and migration
without recreating a continuous-request edge.

An initial test failure after reset was a new-test harness error: it
omitted unlocking PRC1 before releasing module stop. The corrected test
uses the documented protection sequence. Final review also added explicit
coverage for simultaneous sync-clear/overflow and continuous-request
release after a buffer update. Earlier logs are retained.

The four suite skips are the existing cdrom, qos, info_usernet and linters
cases. The existing gdbsim Linux/sash flaky annotation is unchanged;
U-Boot ran using the local asset cache. No existing test was altered or
excluded to obtain these results. FIT archives were copied from Issue 27
and checked by the unchanged pinned-hash build script. Subprojects were
already present. Configure installed Python test dependencies using the
existing environment and vendored wheels; no new source or asset network
fetch was requested. All build/scratch files, including temporary sockets,
are inside this worktree. The scratch-only `test-env/sitecustomize.py`
redirects QEMUMachine's default temporary directory to TMPDIR.

Reproduce from the worktree root:

```sh
export TMPDIR="$PWD/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$HOME/workspace/projects/qemu/.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
mkdir -p "$TMPDIR" build
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
ninja -C build -j8
export QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx"
build/tests/qtest/rx113-mtu-test
bash .issue29-runtime/regression.sh
```

`regression.sh` records all regression, FIT, SYSTEM and gdbsim commands.
It uses the scratch-local cache and Python temporary-directory hook.
The tracked standalone MTU qtest needs only the built RX binary and
TMPDIR, without the scratch Python hook.

## D-006 residual scope and merge-gate decisions

The requested count/compare/IRQ/clear, normal/PWM register timing and
PCLKB/MSTP behaviors are implemented. The following are explicit remaining
features, not claims of successful emulation. Estimates are engineering
work including manual-based tests, not commitments.

| Remaining feature | Reason and expected size |
| --- | --- |
| Actual MTIOC waveforms, TIOR initial/toggle/polarity output, TOER/TOCR/TOLBR and POE cut-off, PORT/MPC routing | Requires a timer output state machine, pad multiplexing and output protection across modules; these pad paths were explicitly excluded from the core. About 3-5 days including normal/PWM 0%/100% pad cases and mode transitions. TIOR's compare/capture selection is effective now; no output level is claimed. |
| Input capture, input buffers/clear on capture, TICCR and internal capture links, MTIC5 pulse-width measurement and NFCR filters | Faithful input synchronization, edge/filter sampling, internal timer links and simultaneous capture/cascade ordering need a separate input-event implementation plus board routing. About 4-6 days for the complete path; internal links alone approximately 1-2 days with chronology and performance tests. Capture selections currently disable the corresponding comparator and log `LOG_UNIMP`; they do not fabricate captures. |
| Complementary PWM and reset-synchronized PWM, TCNTS/TCDR/TCBR/TDDR/TDER/TWCR, brushless motor gating, skipping TITCR/TITCNT/TBTER and MTU5 dead-time compensation | Coupled MTU3/4 up/down counters, crest/trough and dead-time regions, hidden temporary registers, transfer/skipping arbitration and protected output transitions form a separate state machine (20.3.7-20.3.11). About 5-8 days including counter/interrupt and waveform corner cases. Unsupported TMDR modes are diagnosed and are not approximated as ordinary PWM. |
| ADC start requests (TIER.TTGE/TTGE2), TADCR/TADCOR/TADCOBR, DTC transfer activation and ELC event operations | The peer ADC/DTC/ELC devices are still stubs. Retaining TIER enable bits does not produce these cross-module actions. Implementing each peer is a separate multi-day task, followed by approximately 1-2 days for MTU links and event-order tests. |
| Hardware bus-cycle contention, sub-PCLK synchronizer/pipeline delays, prescaler edge alignment and invalid short external pulses | The machine supplies virtual time and PCLKB frequency, not CPU/bus cycles or a physical clock waveform. Count-domain compare/clear timing is implemented; simultaneous CPU-write arbitration in 20.6.4-20.6.18 is not cycle-accurate. The MTU5 stopped TGR-write special case is committed during MMIO rather than a delayed hardware PCLK pipeline. Raw MTCLK GPIOs represent qualified ideal edges. Faithful bus/clock synchronization needs broader timing infrastructure (several days), then roughly 2-3 days of MTU contention/synchronizer tests. |
| Deep sleep/software standby integration | Existing RX113 CPU/SYSTEM lacks those state transitions. Ordinary WAIT sleep leaves MTU counting; MSTP gating is implemented. Faithful additional low-power states require CPU/clock/peripheral integration, approximately 3-5 days, before connecting MTU gating. |

Unimplemented registers log `LOG_UNIMP`, read zero and discard writes;
they do not silently retain configuration as if the corresponding feature
worked. Undefined/reserved low TSR bits deterministically read one.
Unsupported feature requests should be investigated with `-d unimp`.
Migration is supported between builds with this MTU device; importing an
older RX113 stream that had only the stub is not a compatibility claim.

The raw MTCLK phase/cascade support is intentionally supplemental. It
allows underflow interrupt acceptance to exercise a documented down-count
source within RX113-specific code; it is not a claim that an encoder
connected to existing PORT pads will work. The remaining pad/capture,
advanced PWM and peer-module features above require an explicit PO/PMO
merge-gate assessment under D-006.

## Changed files

* `hw/rx/meson.build`
* `hw/rx/rx113-mtu.c`
* `hw/rx/rx113.c`
* `include/hw/rx/rx113.h`
* `include/hw/rx/rx113_mtu.h`
* `tests/qtest/meson.build`
* `tests/qtest/rx113-mtu-test.c`
* `docs/rx113/issue-29-mtu-report.md`

The patch excludes AGENTS.md, the manual, runtime assets, `.t/`, toolchains
and build products. Apply it on the stated base with
`git apply /path/to/issue29.patch`; `.issue29-runtime/patch-validation.log`
records reverse checking against the completed worktree.
