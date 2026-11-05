<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #8 Worker report: RX113 oscillator stabilization delays

Date: 2026-11-06. Branch: `rx113/issue-8-delays`.
Base: `bd68e2952c81c763a63e3244aa6ad56a98246391`, confirmed against
`git ls-remote origin refs/heads/rx113/dev` at work start.
Clone: `/home/hatch/workspace/projects/qemu/.issue8-delays-worktree`.
The root repository's `.git` is read-only, so this local shared clone holds
all commits. The first commit cherry-picks `dd14111987` from
`rx113/issue-8-clock-tree`, retaining its blocker report in the tree.
No push, merge, PR, or upstream submission was performed.

Result: **PASS** for the split oscillator-delay scope. This continuation
reused the existing implementation, test drafts, probe inputs and partial
build rather than starting over after the environment restart.

## Scope and timing authority

Only the SYSTEM model in `hw/rx/rx113.c` changes emulator behavior.
RX62N and shared CMT/TMR/SCI/ICU/CPU models are untouched. D-005 records the
PO's split: frequency propagation and module gating move to a separate issue.
The existing peripheral input remains 32 MHz.

Authority: local `.issue1-runtime/rx113.pdf`, R01UH0448EJ0120 Rev.1.20,
SHA-256 `a41f74559bd8f8aa36749519da119bcc28dcb40f6a23e6f5d403b9113fd19356`.
Sections 7.2.2, 9.2.4/6/7/11/12/17/18 and Table 43.22 were consulted.
LOCO's nominal 4 MHz means one wait-counter cycle is 250 ns. These
counters run independently of software's LOCOCR.LOSTP setting.

| Wait selector | LOCO cycles | Adopted delay |
|---|---:|---:|
| MOSCWTCR.MSTS=0 | 2 | 500 ns |
| MSTS=1 | 1024 | 256 us |
| MSTS=2 | 2048 | 512 us |
| MSTS=3 | 4096 | 1.024 ms |
| MSTS=4 (reset) | 8192 | 2.048 ms |
| MSTS=5 | 16384 | 4.096 ms |
| MSTS=6 | 32768 | 8.192 ms |
| MSTS=7 | 65536 | 16.384 ms |
| HOCOWTCR.HSTS=5 (reset) | 138 | 34.5 us |
| HSTS=6 | 266 | 66.5 us |
| PLL / USB PLL | not a LOCO counter | 50 us after main stability |

Main/HOCO values come from sections 9.2.17/18, pp.181-182.
Table 43.22, p.1428, specifies **maximum tPLL = 50 us** for both PLL
and USBPLL; the model deterministically uses that maximum. A PLL enabled
before the main oscillator is stable waits for MOOVF, then starts its own
50 us interval. A PLL enabled later waits 50 us from its enable. No crystal
frequency, PLL multiplier, or electrical frequency accuracy is simulated.
HSTS=5 is the documented counter wait, even though it does not secure the
56 us maximum analog tHOCO. With OFS1.HOCOEN=0, hardware secures tHOCO before
CPU reset release (9.2.18 note 2): HCOVF therefore starts set, without a
new software-start wait. Subsequent software stop/start uses HOCOWTCR.

## Implementation semantics

A QEMU_CLOCK_VIRTUAL timer services the earliest pending deadline. Only
MOOVF/PLOVF/HCOVF/UPLOVF are stored; reserved bits always read zero. LOCO
and sub-clock get no OSCOVFSR flags. Status writes remain ineffective.
Stop clears the corresponding flag and cancels its deadline; stopping main
also clears/cancels both dependent PLLs. Restart receives a full new wait.
Stop completion remains immediate, as in the preceding model; analog stop
delays and software-standby sequencing are outside this change.
Repeated enable writes and unrelated register writes do not restart waits.
Wait settings are sampled when the corresponding interval starts; changing
a running counter's setting does not retime it. Software must follow the
manual's restrictions on rewriting wait controls.

Prohibited wait encodings log LOG_GUEST_ERROR and leave the oscillator's
flag clear until stopped and restarted with a valid setting. This is an
explicit deterministic emulation policy for prohibited software behavior,
not a claim about silicon. Protection masks and readback remain unchanged.
Reset cancels the timer and all deadlines before restoring option-byte
semantics. Finalization frees the timer. VMState version 2 saves flags,
deadlines and the timer; version 1 input reconstructs the previous model's
immediately stable enabled oscillators. Whole-machine migration has not
been validated by this task.

## Verification

All following gates ran against this clone's newly completed binary in the
continuation, not the Issue #6 binary:

| Gate | Result | Local evidence |
|---|---|---|
| Incremental rx-softmmu build | PASS | `.issue8-delays-runtime/build-resume.log` |
| Genuine FIT PLL, boot and reset | PASS | `.i8/pll/result.json`, `execution.log` |
| Genuine FIT HOCO, boot and reset | PASS | `.i8/hoco/result.json`, `execution.log` |
| Exact oscillator waits, default zero-filled OFS1 | PASS | `.issue8-delays-runtime/system.log` |
| Exact oscillator waits, FIT OFS1.HOCOEN=1 | PASS | `.issue8-delays-runtime/system-elf.log` |
| Phase 1 ELF and raw flash boot/reset | PASS, 2 tests | `.issue8-delays-runtime/phase1.log` |
| gdbsim n7/n8 and machine list | PASS | `.issue8-delays-runtime/gdbsim.log` |
| RX qtest | PASS, 9 programs; 2 skipped; 0 failed | `.issue8-delays-runtime/qtest.log` |
| Changed-file checkpatch | 0 errors, 0 warnings | `.issue8-delays-runtime/checkpatch-files-final.log` |
| Patch checkpatch | 0 errors, 1 warning | `.issue8-delays-runtime/checkpatch-patch-final.log` |
| `git diff --check` | PASS | no whitespace errors |

Patch checkpatch reports only the generic MAINTAINERS reminder for the two
new fork-local reports (including the cherry-picked blocker report). No
MAINTAINERS change is needed for these project records. Checks use
`--no-tree --no-signoff`; per-file checks additionally use `-f`.

Both FIT configurations print `RX113 FIT main reached` before and after
QMP reset. PLL observes SCKCR3=0x0400 and OSCOVFSR=0x25; HOCO observes
SCKCR3=0x0100 and OSCOVFSR=0x09. Both leave MSTPCRB=0x7fffffff and PRCR=0.
The genuine pinned R_BSP v7.71 inputs were rebuilt using the specified
`../.issue6-worktree/.issue6-runtime/toolchain/bin/rx-elf-gcc` (GCC 14.2.0).
The builder verifies the pinned package SHA-256; the Issue #6 report
records source provenance and harness limitations, which still apply.

The oscillator test checks immediately after enable, 1 ns before the
deadline and at the deadline. It covers all eight main waits and both HOCO
waits, PLL enable before and after main stability, both PLL flags together,
read-only status, reserved bits, repeated enables and unrelated writes,
stopped LOCO, pending-wait cancellation, restart, prohibited settings and
reset during pending waits. Both option-byte reset paths pass. The retained
pre-restart `system-before.log` shows the new test rejecting the previous
instantaneous model at its immediate-after-enable assertion; this is
baseline evidence from the earlier run, not a newly rerun baseline gate.
The gdbsim smoke runs each RX62N machine for three seconds and verifies
clean SIGTERM shutdown; it does not claim a guest Linux boot. RX qtest
includes all five RX ICU subtests.

## Reproduction

Run from the clone above. Build dependencies were downloaded from the
wrap-defined official URLs at their pinned revisions. Only rx-softmmu
was configured. All build/scratch output stays in the workspaces.

```sh
export PATH="$HOME/workspace/tools/build-venv/bin:$PWD/../.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
mkdir -p ../.t8 .issue8-delays-runtime .i8
export TMPDIR="$PWD/../.t8"
meson subprojects download dtc berkeley-softfloat-3 berkeley-testfloat-3 keycodemapdb
mkdir -p build
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
make -C build -j4
python3 tests/functional/rx/fit_boot/build.py .issue8-delays-runtime/probe
python3 tests/functional/rx/fit_boot/build.py .issue8-delays-runtime/probe-hoco --hoco
export PYTHONPATH="$PWD/python"
build/pyvenv/bin/python3 tests/functional/rx/fit_boot/verify.py build/qemu-system-rx \
  .issue8-delays-runtime/probe/boot.elf .i8/pll
build/pyvenv/bin/python3 tests/functional/rx/fit_boot/verify.py build/qemu-system-rx \
  .issue8-delays-runtime/probe-hoco/boot.elf .i8/hoco
timeout 30s python3 tests/functional/rx/fit_boot/system.py build/qemu-system-rx .i8/sys
timeout 30s python3 tests/functional/rx/fit_boot/system.py build/qemu-system-rx \
  .i8/sys-elf --elf .issue8-delays-runtime/probe/boot.elf
export QEMU_TEST_QEMU_BINARY="$PWD/build/qemu-system-rx"
export PYTHONPATH="$PWD/python:$PWD/tests/functional"
export MESON_BUILD_ROOT="$PWD/build"
build/pyvenv/bin/python3 tests/functional/rx/test_rx113.py
make -C build check-qtest-rx -j4
```

## Remaining work and delivery

Frequency propagation, module gates and the shared-model interfaces remain
separate-issue work. Sub-clock software waits, analog startup/stop characteristics,
voltage constraints, clock validity checking and general FIT compatibility
remain outside this scope. No new shared-model blocker was worked around.
The FIT probes exercise genuine R_BSP startup followed by the existing
custom polling SCI0 application, not a Renesas SCI FIT driver.

Commits are retained on `rx113/issue-8-delays`, including blocker-report
cherry-pick `f90ff7d985`. The implementation/test commit is titled
`hw/rx: Delay RX113 oscillator stabilization flags`; the final documentation
commit records D-005 and this report. Obtain the complete commit list with:

```sh
git log --reverse --format='%H %s' \
  bd68e2952c81c763a63e3244aa6ad56a98246391..rx113/issue-8-delays
```

The incremental bundle is `.issue8-delays-runtime/issue-8-delays.bundle`;
it requires the base above. It is checked with `git bundle verify` after
creation. From the original repository, once its `.git` is writable,
import the branch without merging:

```sh
git fetch ./.issue8-delays-worktree/.issue8-delays-runtime/issue-8-delays.bundle \
  rx113/issue-8-delays:rx113/issue-8-delays
```

Build products, probe inputs and local logs remain untracked; only RX113
source, tests and documents are committed. The PO owns the merge decision.
All continuation work completed within the additional three-hour limit.
