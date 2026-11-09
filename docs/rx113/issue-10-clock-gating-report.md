<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 10: Shared peripheral clocks and RX113 module stop

Implemented on `rx113/issue-10-clock-gating`, based on
`dad32fa0a57111b18d780860d2ccc989c29dc20e`. Work stayed in the instructed
worktree, within the three-hour time box. No push, merge, PR, upstream
submission, or network download was performed.

## Interfaces and retained state

The shared models export these C interfaces in their existing headers:

```c
void renesas_cmt_set_clock(RCMTState *s, uint64_t freq, bool stopped);
void renesas_tmr_set_clock(RTMRState *s, uint64_t freq, bool stopped);
void renesas_sci_set_clock(RSCIState *s, uint64_t freq, bool stopped);
```

The board opts in under the BQL after realization and before guest
execution. Subsequent calls can change frequency and/or module stop while
the module is running. Frequency is nonzero, in Hz, at most 1 GHz; use the
boolean gate rather than zero frequency. Repeating the same configuration
has no effect. `input-freq` retains its realization-only property setter;
its getter reflects the current input frequency after an API update.

* CMT and TMR account for elapsed counter ticks before a change, retain
  fractional input cycles and prescaler state, cancel previous events, and
  schedule the remaining counter distance at the new frequency. The
  managed TMR path handles independent channels, 16-bit overflow cascading,
  compare-A cascading, overflow and simultaneous compare events. Deadlines
  round up to virtual-clock nanoseconds. The existing compare-distance
  convention is retained; this change does not revise register semantics
  or the legacy timing formulas.
* SCI retains outstanding transmit completion and receive cooldown in
  billionths of an input cycle, refreshes `trtime` with the existing
  formula, and recalculates deadlines. Repeated frequency changes without
  elapsed virtual time do not consume fractional work. A managed receive
  timer wakes a throttled chardev at the updated deadline. `can_receive`,
  `receive`, and break callbacks respect the module gate. The existing
  model delivers the first host byte at transfer start; gating freezes
  subsequent completion and queued transmission, not an already delivered
  host byte.
* Stopping cancels timer activity and blocks MMIO without resetting
  registers, counters, buffered data or pending interrupt state. Reads
  return zero and writes are discarded while stopped. Zero readback is a
  deterministic emulation policy for unavailable registers, not a
  hardware-guaranteed value. Restart continues the retained work. IRQ
  sources already delivered to the ICU are not cleared or masked by the
  gate, consistent with section 11.7.3's firmware precautions.
* Managed reset clears peripheral work and cancels pending events. The
  clock-management policy is retained; RX113 SYSTEM reset exit supplies
  reset clock and module-stop values after all peripheral reset holds.
* Optional `rx-cmt/clock`, `rx-tmr/clock` and `renesas-sci/clock` migration
  subsections save frequency, gate and phase/remaining work. Existing base
  fields and version numbers remain unchanged. Clock subsections validate
  frequency and fractional state. On incoming streams without a clock
  subsection the device returns to its original property frequency and
  legacy path. The initial property frequency is destination configuration,
  not migrated mutable state.

The initialization-before-execution contract is intentional: boards opt
into the managed accounting path at initialization, rather than switching
an already executing legacy model's historical counter accounting into a
new policy. RX113 does this at its first reset exit. Runtime frequency and
gate changes thereafter are fully implemented.

## RX113 wiring and manual evidence

Authority: `.issue10-runtime/rx113.pdf`, R01UH0448EJ0120 Rev.1.20;
text extraction retained as `rx113.txt` in the same scratch directory.
Issue 8's findings and decisions D-003, D-005, D-006 and D-007 were read.

SYSTEM writes to SCKCR, SCKCR3 and PLLCR update PCLKB for both CMT units,
both TMR units and SCI0/1/2/5/6/12. Sections 9.2.1-3 and 9.7.2 give:

* LOCO 4 MHz, HOCO 32 MHz, main crystal, sub-clock 32768 Hz, or PLL as
  selected by CKSEL; `PCLKB = source / 2^PCKB` for encodings 0..6.
* `PLL = main / 2^PLIDIV * multiplier`, with STC 0x0b/0x0f selecting 6/8.
* UPLL is for UCLK and does not change PCLKB.

The MCU exposes a realization-only `main-clock-hz` board property, default
16 MHz, explicitly matching the genuine FIT generic RX113 BSP. Other
boards can configure their crystal with
`-global rx113-mcu.main-clock-hz=<Hz>`. Invalid source/divider/PLL encodings
log a guest error and retain the previous frequency; module gates still
update. Oscillator startup/stability timing from Issue 8 is unchanged.
CPU instruction timing is not driven by ICLK by this change.

Manual sections 11.2.2-3 (pp.214-216), 11.4 (p.227), and 11.7.3 (p.236)
were checked directly. MSTPCRA is now modeled, protected by PRC1, with
reset value 0xefffffff. TMR units use MSTPA5/4; CMT units use MSTPA15/14;
SCI0/1/2/5/6/12 use MSTPB31/30/29/26/25/4. Reset PCLKB is LOCO/8 =
500 kHz and all these modules are stopped. SYSTEM migration version 3
adds MSTPCRA while preserving the original version 1/2 register array
layout. New RX113 streams include the peripheral clock subsections.

The Phase 1 and LCDC test guests now unlock PRC1, release SCI0 module stop,
and relock before transmitting their existing marker. Their boot, reset,
part-capacity and display expectations are unchanged. The genuine FIT
probe already releases SCI0 stop and needs no behavioral workaround.

## RX62N invariance

`hw/rx/rx62n.c` and `hw/rx/rx-gdbsim.c` are unchanged. Their boards never
call the new APIs, so `clock_managed` and `module_stopped` stay false.
Existing counter calculations, scheduling, SCI timing cache, reset paths,
chardev callbacks and migration base fields remain the default. New
subsections are omitted for these machines. The receive wakeup timer has
no events on this path. Pre-load only restores the same default policy
for a legacy stream.

Per-file checkpatch required changing the old `DEVICE_NATIVE_ENDIAN`
annotations in CMT and SCI to `DEVICE_LITTLE_ENDIAN`. RX is a little-endian
target (`configs/targets/rx-softmmu.mak`); SCI accesses are bytes. This
annotation cleanup has no RX62N or RX113 behavioral effect.

The new regression test deliberately checks the legacy CMT counter
readback and exact 16664 ns deadline on both 48 MHz gdbsim configurations,
as well as rejection of runtime writes to the old property. The same
legacy case passes against the pre-existing Issue 6 binary, whose shared
models have not used the new APIs. FPU tests also exercise RX62N, and
U-Boot boots with the unchanged existing functional test.

## Verification

All final results use `build/qemu-system-rx`; its SHA-256 is in
`.issue10-runtime/tested-binary.sha256`.

| Check | Final result | Evidence under `.issue10-runtime/` |
|---|---|---|
| RX-only configure/build, existing pinned subprojects | PASS | `configure.log`, `build-check.log` |
| New clock qtest | 9/9 PASS | `clock-qtest-final.log` |
| Full RX qtest + quick functional suites | 20 OK, 4 existing skips, 0 failures | `regression-final.log` |
| Phase 1 and all four part variants | 8 boot/reset cases PASS | `regression-final.log` |
| RX113 and RX62N FPU tests | 2 cases PASS | `regression-final.log` |
| LCDC functional / LCDC / RTC / PORT / ICU qtests | 1 / 6 / 20 / 6 / 5 cases PASS | `regression-final.log` |
| Genuine rebuilt FIT PLL and HOCO, before/after reset | PASS | `fit-build-*.log`, `fit-*-final.log`, `verify-*-final/result.json` |
| SYSTEM oscillator/protection, without/with PLL ELF | PASS | `system.log`, `system-elf.log` |
| Both gdbsim machines listed and running after 3 seconds, clean exit | PASS | `machines.log`, `gdbsim-smoke.json` |
| Existing gdbsim U-Boot functional test | PASS; Linux/sash retains its upstream flaky skip | `gdbsim-uboot-final.log` |
| Legacy CMT golden case against Issue 6 binary | PASS, both machines | `legacy-baseline.log` |
| Changed-file checkpatch | 0 errors, 0 warnings | `checkpatch-files-final.log`, `checkpatch-report.log` |
| Whitespace / import patch validation | PASS | `patch-validation.log` |

The nine new cases cover source propagation to every child, valid PLL
encodings, UPLL independence, CMT and both TMR channels/units with half a
tick retained across divider changes, 16-bit TMR cascading, all six SCI
transmit deadlines/caches/gates, real socket-backed SCI receive cooldown
and gating, running/stopped migration, and the RX62N legacy path. Repeated
frequency changes while SCI is stopped test fractional-work conservation.
Migration tests synchronize the accelerators' private qtest clocks before
loading: that private counter is not itself migrated. An initial active
migration test omitted this harness step; its corrected version passes.

The four suite skips are existing cdrom, qos, info_usernet and linters.
The separately run Linux/sash skip is the pre-existing gdbsim flaky test,
not a new exclusion. FIT archives were copied from Issue 6 and their pinned
hashes checked by `build.py`; the U-Boot asset was reused from local cache.
All scratch, including temporary sockets, stays under this worktree.
The local `test-env/sitecustomize.py` redirects QEMUMachine's default
`/var/tmp` scratch into TMPDIR; no shared test framework was changed.
Ninja log recovery caused extra recompilation; no non-RX system target was
built. Earlier failed/interrupted build and harness logs are retained for
traceability; the final logs above are the acceptance evidence.

Reproduce with `TMPDIR=$PWD/.t`, build tools first on PATH, and:

```sh
export PYTHONPATH="$PWD/.issue10-runtime/test-env:$PWD/python:$PWD/tests/functional"
meson test -C build --no-rebuild --suite qtest-rx --suite func-rx \
  --print-errorlogs -j2
```

## D-006 residual scope

No required PCLKB propagation or MSTP interface/wiring is left as a stub.
The tests demonstrate runtime updates, retained operation, callback gating
and migration. Boards using these APIs must opt in before guest execution;
late conversion of an already running legacy model is outside the API
contract, not silently supported.

Existing shared-model limitations remain under the explicit prohibition
on changing existing register/timing specifications: SCI's existing baud
formula and immediate delivery of the first host byte, the legacy CMT
CMCR readback/counter quirks, and unmodeled external TMR clock pins. This
issue does not claim to correct them. Revising them faithfully needs a
separately approved compatibility policy and manual-based register/wire
coverage: roughly 1-2 days for CMT accounting/readback compatibility,
2-4 days for SCI baud/shift-register/backend delivery, and 2-3 days for
external TMR pad clocks plus PORT integration. These are estimates, not
new silent no-ops introduced by Issue 10. CPU ICLK timing and oscillator
failure/illegal-source behavior likewise require a wider CPU/clock-tree
scope (roughly 3-5 days), and are not acceptance claims here.

## Delivery

The repository's `.git` is read-only: `git add` fails creating
`.git/index.lock` with `Read-only file system` (`git-add.log`). Consequently
there is no commit. All changes remain in the worktree on the instructed
branch; no push or merge was attempted.

Importable patch: `.issue10-runtime/issue-10-clock-gating.patch`.
Exact changed-file list: `.issue10-runtime/changed-files.txt`.
The patch contains only the following 15 files, excluding AGENTS.md,
manuals, toolchains, build products and scratch:

* `hw/char/renesas_sci.c`
* `hw/rx/rx113.c`
* `hw/timer/renesas_cmt.c`
* `hw/timer/renesas_tmr.c`
* `include/hw/char/renesas_sci.h`
* `include/hw/rx/rx113.h`
* `include/hw/timer/renesas_cmt.h`
* `include/hw/timer/renesas_timer.h`
* `include/hw/timer/renesas_tmr.h`
* `tests/functional/rx/fit_boot/main.c`
* `tests/functional/rx/test_rx113.py`
* `tests/functional/rx/test_rx113_lcdc.py`
* `tests/qtest/meson.build`
* `tests/qtest/rx113-clock-test.c`
* `docs/rx113/issue-10-clock-gating-report.md`

The PO can apply the patch to the base above and create logical commits
for the shared interfaces, RX113 wiring/tests, and this report. Merging
remains exclusively the PO's decision.
