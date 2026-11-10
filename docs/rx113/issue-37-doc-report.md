# Issue #37 — RX113 DOC implementation report

Branch: `rx113/issue-37-doc`. Fork-local delivery; no commit or merge.

## Implementation and decisions

The DOC stub is replaced with an RX113-only SysBus device, mapped at
0x0008b080. DOCR, DODIR and DODSR reset to zero. Only a byte access to
DOCR and aligned 16-bit accesses to DODIR/DODSR perform register operations,
following Table 5.1. Invalid accesses return zero/discard writes and log a
guest error. Reserved DOCR bits and DOPCFCL read zero; DOPCF is read-only
and sticky until DOPCFCL is written as one.

Every DODIR word write performs the selected comparison, addition or
subtraction. DCSEL selects equality/inequality only in comparison mode;
DODSR is preserved during comparison. Arithmetic stores the low 16 bits
in DODSR and detects unsigned carry/borrow. Exact FFFFh/0000h results do
not set DOPCF. DODSR writes and register reads do not initiate operations.

The IRQ connects directly to ICU vector 57, bypassing DTC. The existing
RX113 ICU map already supplies level sensitivity, IER07.IEN1 and IPR057;
it is unchanged. The request is high exactly when DOPCIE and DOPCF are
both set, including enabling DOPCIE after a condition has occurred. Clearing
the flag or disabling DOPCIE deasserts the request.

SYSTEM calls the new retained-state gate interface for MSTPCRB.MSTPB6,
using the existing reset-exit, register-write and post-load update path.
The stopped device reads zero and discards writes; it retains all three
registers and an already asserted interrupt, following Issue #10's policy
and section 11.7.3. Stopping does not clear or mask pending IRQ state.

The `has-elc` property defaults false and is enabled by RX113 board wiring.
Its separate named `event` output pulses for each selected condition,
including repeated conditions while DOPCF is already set, independently of
DOPCIE. It connects to ELC source 0x6a. No ELC implementation change is
needed; the existing ELSR24/25 prohibition on source 0x6a remains intact.
This follows the D-008 independent event interface pattern.

Migration saves the registers and module gate and restores the IRQ level
without generating an ELC event. Reset clears the registers and IRQ;
SYSTEM reset exit restores the reset module-stop state. No RX62N or shared
peripheral implementation is modified.

## Manual coverage and limits

Primary source: local R01UH0448EJ0120 Rev.1.20 hardware manual:

- Section 40.2, pp.1310–1311: register layout, read-only flag and clear bit.
- Sections 40.3.1–40.3.3, pp.1312–1314: operation on DODIR writes and
  wrapped arithmetic (the qtest reproduces Figures 40.3 and 40.4).
- Sections 40.4–40.5.1, pp.1314–1315: condition generation and independent
  event output, irrespective of interrupt enable.
- Section 40.6.1, p.1315; section 11, MSTPCRB.MSTPB6: module-stop access.
- Table 5.1: addresses and access widths; Table 14.3: vector 57, level
  trigger, IER07.IEN1/IPR057, no DTC; Table 17.3 and the existing ELC
  selection checks: source 0x6a and ELSR24/25 restriction.

OMS=11 is explicitly prohibited; the manual defines no arithmetic for it.
The model retains its control readback and logs a guest error if DODIR is
written, without calculating or emitting a condition. This is diagnostic
handling for invalid firmware, not a promise about silicon behavior.
The manual's 2/3-PCLKB bus access latency is not cycle-accounted, consistent
with the project's exclusion of cycle-accurate timing. Arithmetic is
performed synchronously on the register write; the existing ELC models
its own delivery delay.

No unresolved design decision or deferred documented DOC function remains.
No implementation follow-up is required (estimate: zero). PMO/PO review
and commit/import remain the delivery gate.

## Validation

All checks use the final `build/qemu-system-rx` identified by
`.issue37-runtime/tested-binary.sha256`. No subproject was re-fetched.
Meson 1.12.1 and the supplied Ninja are first on PATH. FIT probes were
rebuilt with the existing rx-elf toolchain from cached, hash-verified R_BSP
ZIPs, using the unchanged FIT builder. All scratch remains beneath
`.issue37-runtime/.t` (including the relative spelling described below).

| Check | Result | Evidence under `.issue37-runtime/` |
| --- | --- | --- |
| Configure/build, rx-softmmu only | PASS; final build has no compiler warnings | `configure.log`, `build.log`, `build-final.log` |
| DOC qtest | 7/7 PASS standalone and in the suite | `doc-test.log`, `regression.log` |
| All RX qtests and quick functional tests | 28 OK, 0 failures, 4 existing skips | `regression.log` |
| RX113/RX62N FPU tests | 2 PASS | `regression.log` |
| RX113 phase 1 / LCDC / IWDT functional | 8 / 1 / 2 PASS | `regression.log` |
| ICU / LCDC / PORT / SCI / clock qtests | 5 / 6 / 6 / 3 / 9 PASS | `regression.log` |
| RTC / IWDT / MTU / S12AD / DTC / ELC qtests | 20 / 49 / 21 / 14 / 125 / 44 PASS | `regression.log` |
| FIT PLL and HOCO boot, then reset/boot | Both PASS | `fit-build-*.log`, `fit-*.log`, `verify-*/result.json` |
| SYSTEM with and without FIT ELF | Both PASS | `system.log`, `system-elf.log` |
| RX62N gdbsim U-Boot | PASS | `gdbsim-functional.log` |
| Machine help and both gdbsim smoke checks | Listed; running for 3 seconds; exit 0 | `machine-help.log`, `gdbsim-smoke.json` |
| Changed-file checkpatch | 0 errors, 0 warnings | `checkpatch-files.log` |
| Patch and whitespace | Reverse check and isolated forward dry run/application PASS | `patch-validation.log` |

The four existing skips are cdrom, qos, info_usernet and linters.
The DOC tests cover both DCSEL values, positive and negative comparison
results, the manual's addition/subtraction sequences, exact boundaries,
wraparound, sticky/read-only flags, reserved-bit masks, DOPCFCL, toggling
DOPCIE with a pending flag, level-triggered IR behavior, and the absence
of DTCER057. Access tests check reset gating, invalid widths, stopped
write discard/state retention, and reset. Migration tests preserve all
registers and the pending request with DOC running and stopped. The ELC
test generates real DOC conditions in all three modes, counts them at
CMT1 with DOPCIE=0, checks repeated events while DOPCF is already set,
and verifies no event for a noncondition, a stopped write or a disconnected
link. It also checks that ELSR24/25 retain their previous selection when
firmware attempts to select DOC.

The initial DOC test caught QEMU splitting a wrong-width 32-bit access
at DODIR into word writes. The final MMIO interface keeps such requests
whole so register width/address validation rejects them without side
effects; the original assertion is unchanged (`doc-test-initial.log`).
The first broad regression hit AF_UNIX path limits in the generic monitor
hotplug and netdev socket tests under this long worktree path. Runtime-only
harness adjustments shorten Python's socket-directory prefix and use
`../.issue37-runtime/.t` from `build/` for the C tests. Both spellings refer
to the mandated scratch directory; no scratch moves to /tmp. The complete
suite was then rerun successfully. These adjustments change no repository
test assertions or product code. Initial logs and targeted reruns remain
as `regression-initial.log`, `monitor-rerun.log` and `netdev-rerun.log`.
The first Make invocation also printed Ninja's jobserver descriptor
diagnostic during regeneration but completed successfully; the final
direct Ninja build completed without that diagnostic.

Reproduce from the worktree root with the retained runtime assets:

```sh
export TMPDIR="$PWD/.issue37-runtime/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
ninja -C build -j4
QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx" build/tests/qtest/rx113-doc-test
bash .issue37-runtime/regression.sh
python3 .issue37-runtime/package.py
```

## Delivery

The working tree and `.issue37-runtime/issue37.patch` contain eight files:
the DOC source/header, MCU/SYSTEM wiring and state pointer, build and qtest
registration, DOC tests, and this report. `changed-files.txt` records the
exact manifest; `issue37.patch.sha256` records the patch checksum and
`base-commit.txt` identifies its base. The packaging script checks reverse
application against the current tree, forward dry-run application against
an isolated HEAD fixture, then actual application and byte-for-byte
comparison of every delivered file. `git diff --check` also passes.

AGENTS.md, decisions.md, the brief, manual, runtime/build artifacts and the
pre-existing untracked `probe.txt` are excluded. Changes remain uncommitted
in this worktree because .git is read-only. Nothing was merged or submitted
upstream.
