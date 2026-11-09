<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 27: Model the RX113 independent watchdog timer

Work branch: `rx113/issue-27-iwdt`; base
`d31ac38a43f364937c890e06a0dd6e1cafb2a0c5`.
The complete `.issue27-runtime/instructions.md`, root `AGENTS.md`, D-006,
QEMU coding style, and the supplied R01UH0448EJ0120 Rev.1.20 were read.
Changes are confined to RX113 devices/wiring, tests, build registration,
and this report. Shared peripherals, ICU and RX62N sources are unchanged.
No push, merge, PR, upstream submission or new network download was made.

## Implementation and manual basis

`rx113-iwdt` replaces the stub at `0x00088030`. It implements IWDTRR
(byte, +0), IWDTCR (word, +2), IWDTSR (word, +4), IWDTRCR (byte, +6),
and IWDTCSTPR (byte, +8), including access widths, reserved masks,
reset readback and control-register write protection. Unsupported offsets
and access widths are diagnosed as guest errors, not silently accepted.

Manual evidence:

* Sections 26.1/26.2, pp.680-688: independent count source, 14-bit counter,
  register addresses, reset values, divisors 1/16/32/64/128/256, periods
  128/512/1024/2048, window start/end positions and W0C status flags.
* Table 26.3 and Figure 26.2, p.685: start counter values are inclusive;
  an end position greater than or equal to the start disables the end.
  The end counter marks the beginning of the prohibited interval.
* Sections 26.3.1-3, pp.689-695: auto-start versus register start,
  write-once controls, `00 -> FF` refresh and invalid/interleaved sequences.
  Each control register has its own write-once latch; the first valid
  refresh locks all three until a reset that resets the IWDT itself.
* Sections 26.3.4-6, p.696: new errors replace previous error sources;
  flags survive the resulting watchdog reset; register mode stops on error
  and can restart by refresh; auto mode reloads one cycle after an error.
* Table 6.2, p.126, and section 6.2.3, p.129: watchdog reset retains the
  watchdog registers, oscillator and write protection, while other MCU
  peripherals and CPU reset. RSTSR2.IWDTRF records the watchdog reset and
  clears by read-one then write-zero. QMP `system_reset` is modeled as an
  external reset of the IWDT, clearing its state and write protection.
* Sections 9.2.10/9.7.8, pp.173/191: ILOCOCR at `0x00080035` is protected
  by PRCR.PRC0, is ineffective in auto mode, and cannot stop an oscillator
  once software starts it. Table 43.22 gives maximum tILOCO = 50 us.
* Section 26.1, section 11.2.2-5 and Table 11.1: IWDT is not a module-stop
  target. PCLKB source/divider and MSTP settings do not gate its counter.

With an unconnected WUNI output, after the first error pulse the model
advances repeated automatic NMI-mode errors arithmetically on MMIO access.
This preserves counter phase, source replacement, W0C flags and later
refreshes while allowing year-long RTC clock steps to complete promptly.
Connected outputs, pending refreshes, reset mode and sleep gating retain
count-edge timers. A qtest checks a full virtual year and a subsequent
refresh, without altering any existing RTC verification asset.

The model uses the nominal 15 kHz dedicated oscillator, with absolute
rational virtual-time deadlines rounded upward to nanoseconds. It does
not accumulate a rounded tick period; multiplication uses `muldiv64_round_up` to
avoid overflow during long runs. Register-mode software must start ILOCO
before counting can advance. Its 50 us stabilization is modeled, including
refresh requests submitted before the oscillator starts or stabilizes.
Auto mode has its dedicated clock available at reset release.

Valid refreshes complete after four divided count-clock edges (the maximum
synchronization time in section 26.3.3). The counter continues to run while
refresh is pending. A refresh at count 3 in a full window prevents underflow;
a request too late can underflow before completion. FF outside the window
records a refresh error; the completion also checks that the window has
not ended. Invalid byte sequences do not refresh or manufacture errors.
CNTVAL exposes the current integer counter without modeling PCLK sampling
uncertainty; flag clears take effect immediately. Bus-clock crossing and
analog phase are abstracted, rather than adding guest-visible arbitrary
read jitter. These tests use the specified deterministic synchronization
and nominal frequency, not electrical oscillator tolerances.

IWDTCSTPR/OFS0 sleep-stop controls affect counting when the existing RX CPU
executes WAIT. The device samples the core's halted and `in_sleep` state
on each count edge; VM pause alone is not a guest sleep transition.
The oscillator/divider keeps its phase while the down-counter is gated.

## OFS path, outputs and FIT interaction

Section 7.2.1, pp.138-139, places OFS0 at `0xffffff8c`. The IWDT links to
the MCU code-flash MemoryRegion, then reads a little-endian word at
`flash_size - 0x74` in reset **exit**, after the ROM loader restores ELF
or raw-image contents in reset hold. This follows the existing SYSTEM
OFS1 loading pattern and works for all supported flash sizes. There is no
host-side OFS override or dependency on `rom_ptr()` after its loader buffer
has been released. Options are latched at reset, never polled during a run.
The existing zero-filled no-image flash is unchanged: its zero OFS0 means
auto-start with NMI output, rather than inventing an erased-flash default.

OFS0.IWDTSTRT=0 selects TOPS (bits 3:2), CKS (7:4), RPES (9:8), RPSS
(11:10), reset selection (12) and sleep-stop (14). The software controls
retain their reset readback in this mode and writes are ineffective.
IWDTSTRT=1 selects register start and the three software control registers.
Prohibited dividers do not run an invented clock; register-start attempts
also produce a guest-error diagnostic.

Reset selection calls `qemu_system_reset_request(GUEST_RESET)` on an
underflow or refresh error. An internal source latch distinguishes this
reset from an external reset, retaining flags/configuration/protection
and recording RSTSR2.IWDTRF. Register mode stays stopped after reset;
auto mode starts again. QEMU's normal reset machinery resets the rest of
the MCU and reloads the hardware reset PC through the existing machine
reset handler. This is a chip reset, independent of the host watchdog
`-action watchdog=...` policy.

NMI selection generates a one-count-cycle WUNI request on the device's
SysBus IRQ output and diagnoses the missing delivery path with `LOG_UNIMP`.
It does **not** substitute an ordinary maskable IRQ, fast IRQ, or reset.
The existing shared ICU has no IWDT input/NMISR.IWDTST/NMIER.IWDTEN support,
and the shared RX CPU exposes only IRQ/FIR inputs and has no NMI exception
entry. Thus guest NMI handler execution is unavailable. qtest intercepts
`/machine/mcu/iwdt` output `sysbus-irq[0]` to verify actual request timing;
this is explicitly not a claim of CPU NMI delivery.

The genuine R_BSP v7.71 assets were inspected before implementation:
`generic_rx113/r_bsp_config_reference.h` defines
`BSP_CFG_OFS0_REG_VALUE=0xffffffff` and
`BSP_CFG_IWDT_CLOCK_OSCILLATE_ENABLE=0`. Neither PLL nor HOCO probe
refreshes/starts IWDT. The LPT-specific CSTPR write in `mcu_clocks.c` is
excluded by the probes' sub-clock LPT configuration. Both variants reuse
the pinned, SHA-256-verified Renesas archive already present locally;
no FIT source, configuration, linker script or existing test was modified.

## Reproduction

All scratch, logs, generated firmware and temporary files stay in the
worktree. From its root:

```sh
export TMPDIR="$PWD/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$HOME/workspace/projects/qemu/.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
export PYTHONPATH="$PWD/python"
mkdir -p .t build
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
ninja -C build -j8
QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx" build/tests/qtest/rx113-iwdt-test
meson test -C build --no-rebuild --suite qtest-rx --suite func-rx --print-errorlogs
```

The qtest creates actual raw code-flash images with different OFS0 values.
It checks all 24 period/divisor combinations at deadline minus 1 ns and at
the deadline, all 16 window combinations, repeated refreshes over many
periods, stopped refreshes, malformed/interleaved refresh sequences,
clock startup/protection, independent PCLKB/MSTP behavior, internal reset
and external reset, retained protection/source flags, WUNI pulses and
one-cycle auto reload, and migration during refresh synchronization with
a fractional tick remaining. The new functional test executes real RX
WAIT instructions with both values of OFS0.IWDTSLCSTP.

Genuine FIT build/verify commands are those in
`tests/functional/rx/fit_boot/README.md`, using `.issue27-runtime/fit-pll`
and `.issue27-runtime/fit-hoco`, with cached `r_bsp.zip` copied into each.
`.issue27-runtime/regression.sh` records the full regression commands.
The scratch-only `test-env/sitecustomize.py` redirects QEMUMachine's default
scratch into TMPDIR; no test framework or existing asset was altered.

## Validation

Final validation completed on 2026-11-09. The final tested
`build/qemu-system-rx` SHA-256 is
`56417014d3c0ee2dde6d0b46c33fe454d6c85b20d3d1ed1706100c01f887357c`.
Evidence paths below are relative to `.issue27-runtime/`.

| Check | Result | Evidence |
|---|---|---|
| RX-only configure, full build and final incremental build | PASS, exit 0 | `configure.log`, `build-final.log`, `build-unconnected.log` |
| IWDT qtest, including all options/windows, migration and one virtual year | 49/49 PASS | `iwdt-final.log` |
| Real WAIT, sleep stop/continue | 2/2 PASS | `regression.log`, functional test logs in `build/tests/functional/rx/` |
| Full RX qtest and quick functional suites | 23 OK, 4 existing skips, 0 failures/timeouts | `regression.log` |
| Phase 1, ELF/raw flash, all parts and reset | 8/8 PASS | `regression.log` |
| RX113/RX62N FPU and LCDC functional | 2/2 and 1/1 PASS | `regression.log` |
| ICU/LCDC/RTC/PORT/clock/SCI qtests | 5/6/20/6/9/3 subtests PASS | `regression.log`, `rtc-final.log` |
| Genuine FIT PLL/HOCO rebuild | 2/2 PASS | `fit-build-pll.log`, `fit-build-hoco.log` |
| Genuine FIT main before/after reset, PLL/HOCO | 4/4 observations PASS | `fit-pll.log`, `fit-hoco.log`, `verify-{pll,hoco}/result.json` |
| SYSTEM protection and oscillator deadlines, without/with FIT ELF | 2/2 PASS | `system.log`, `system-elf.log` |
| Both gdbsim machines listed and run for 3 seconds, clean shutdown | 2/2 PASS, exit 0 | `machine-help.log`, `gdbsim-smoke.json` |
| gdbsim U-Boot functional | 1 PASS | `gdbsim-functional.log` |
| All 10 changed files, checkpatch | 0 errors, 0 warnings | `checkpatch-files.log` |
| Diff whitespace and reverse import-patch validation | PASS | `patch-validation.log` |

Patch-level checkpatch with `--no-signoff` (this is an uncommitted import
patch, not a signed contribution) also reports 0 errors, with one generic
new-file MAINTAINERS warning. The hardware/header paths are already
covered by the existing `hw/rx/` and `include/hw/rx/` entries; MAINTAINERS
was not changed outside the authorized scope. File-level checks are clean.

The four suite skips are the existing cdrom, qos, info_usernet and linters
cases. The gdbsim Linux/sash case has its pre-existing flaky-test annotation;
U-Boot ran from the copied local cache. No required regression test was
changed or excluded to obtain the results.

The first full regression exposed the unobservable automatic-NMI callback
cost in RTC's long virtual-time steps (`regression-before-lazy.log`,
60-second RTC timeout). Arithmetic catch-up fixed that performance problem;
the unchanged RTC suite then passed in 3.71 seconds in the full regression.
The new one-year qtest protects that behavior. Earlier development checks
also corrected use of a 32-bit multiplier in the time-conversion helper,
qtest's rejection of zero-length clock steps, and named SysBus IRQ
interception. The initial overlapping builds were an execution mistake;
subsequent builds ran serially, and final builds/tests passed. Ninja
recovered its interrupted log; no compiler error remains.

The standalone smoke was repeated on the final binary. The final test
commands and scratch hook are retained in `regression.sh`,
`gdbsim-smoke.py` and `test-env/sitecustomize.py`.

## Remaining work under D-006

* **CPU NMI delivery is unimplemented.** This is a concrete shared-model
  constraint, not a silent success path. The request generation, flag,
  counter stop/reload and reset alternative are implemented and tested.
  Faithful delivery needs shared ICU IWDT status/enable/clear and NMI
  arbitration plus RX CPU non-maskable fixed-vector entry, wakeup, nesting,
  saved-state/return behavior and RX62N regressions. Estimated effort:
  2-4 engineering days, subject to an explicit shared-model authorization.
  The PO/PMO must consider this limitation at the merge gate.
* WAIT sleep counting is implemented. The RX113 machine has no implemented
  SBYCR/software-standby/deep-sleep transition machinery. Those additional
  low-power states cannot be entered faithfully, so their IWDT gating
  cannot be validated or claimed. Supporting those states requires the
  clock/CPU/peripheral low-power integration, approximately 3-5 days;
  connecting their resulting sleep indication to this counter is small
  (less than one day). No shared-model change was made to simulate them.
* Boot mode, voltage-monitor reset interactions and analog clock tolerances
  remain outside the machine's existing user-ROM execution/nominal-clock
  model. OFS0 is intentionally honored for the supported user-ROM mode.
  Boot-mode selection/ROM and voltage-reset infrastructure would take
  several days each; analog electrical variation is outside the digital
  virtual-product model. Existing limitations are not represented as
  completed validation of those modes.

## Delivery

The environment exposes `.git` read-only, so no commit was made.
All implementation changes remain in the worktree. The complete import
patch is `.issue27-runtime/issue27.patch`, with
`.issue27-runtime/changed-files.txt` listing exactly these 10 files:

* `docs/rx113/issue-27-iwdt-report.md`
* `hw/rx/meson.build`
* `hw/rx/rx113-iwdt.c`
* `hw/rx/rx113.c`
* `include/hw/rx/rx113.h`
* `include/hw/rx/rx113_iwdt.h`
* `tests/functional/rx/meson.build`
* `tests/functional/rx/test_rx113_iwdt.py`
* `tests/qtest/meson.build`
* `tests/qtest/rx113-iwdt-test.c`

The patch was generated using a scratch copy of the index, a local object
directory and the repository object directory as an alternate. The actual
index and `.git` were not written. `git apply --check --reverse` confirms
that the import patch exactly matches the final working tree. Runtime
assets/logs, `.t/` and fork-local `AGENTS.md` are excluded. Apply the patch
on the stated base with `git apply /path/to/issue27.patch`; suggested commit
subject: `hw/rx: Model the RX113 independent watchdog timer`.
No push or merge was performed.
