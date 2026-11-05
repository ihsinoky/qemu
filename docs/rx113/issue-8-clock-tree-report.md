<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #8 Worker report: clock tree and module gating blocker

Date: 2026-11-05. Result: **BLOCKED; implementation stopped** under the
instruction's shared-model stop condition. No emulator or test source was
changed. Issue #8 is not complete and its acceptance gates have not passed.

Branch: `rx113/issue-8-clock-tree`.
Clone: `/home/hatch/workspace/projects/qemu/.issue8-worktree`.
Base: `bd68e2952c81c763a63e3244aa6ad56a98246391`, fetched directly from
`https://github.com/ihsinoky/qemu.git` branch `rx113/dev` before branching.
The root repository's `.git` is read-only, so a local shared clone was used.
No push, merge, PR, or upstream submission was performed.

## Blocking interface limitation

The instruction requires runtime clock changes through the existing
`input-freq` properties and prohibits modifying shared peripheral internals.
Those interfaces cannot implement the requested behavior:

1. `hw/timer/renesas_cmt.c:rcmt_properties`,
   `hw/timer/renesas_tmr.c:rtmr_properties`, and
   `hw/char/renesas_sci.c:rsci_properties` all declare `input-freq` with
   `DEFINE_PROP_UINT64`. `hw/core/qdev-properties.c:qdev_prop_uint64` does
   not enable `realized_set_allowed`. `qdev_prop_allow_set()` consequently
   rejects changes after realization. SYSTEM writes occur after realization.
   The `qdev_prop_set_uint64()` helper passes `&error_abort` to the property
   setter, so using that helper would abort rather than update the clock.
2. Even bypassing the property guard would not suffice. CMT and TMR already
   have events scheduled at absolute virtual deadlines; a field assignment
   does not account for elapsed ticks or reschedule those events. SCI caches
   character duration in `trtime`, recalculated by its private
   `update_trtime()` function on selected register writes.
3. Zero frequency is not a supported gate. CMT `update_events()`, TMR
   `update_events()`, and SCI `update_trtime()` divide by `input_freq`
   without a zero-clock stop path. SCI realization also explicitly checks
   for a nonzero input. These devices expose no clock input or gate GPIO
   that pauses their operation while preserving internal state.
4. MMIO blocking alone leaves scheduled timer callbacks and SCI chardev
   receive callbacks active. SCI `can_receive()` checks RE and `rx_next`,
   not module-stop status. IRQ masking alone leaves counters and serial
   transfers active. Resetting/unrealizing on stop loses state required by
   manual section 11.4. None satisfies actual module gating.

Directly editing child `input_freq`, timers, counters, SCI `trtime` or
chardev handlers from `rx113.c` would depend on shared-model internals,
outside the allowed existing-property interface. No such workaround was
implemented. A separate RX113 peripheral reimplementation would also depart
from the instruction to use the existing models through `input-freq`.

## Runtime evidence and reproduction

The existing Issue #6 binary at
`../.issue6-worktree/build/qemu-system-rx` was used only to demonstrate the
interface blocker. The peripheral and property sources inspected above
match those in the new branch. No new QEMU binary was built.

For each of `/machine/mcu/cmt[0]`, `/machine/mcu/tmr[0]`, and
`/machine/mcu/sci[0]`, QMP `qom-get input-freq` returned `32000000`.
Attempting `qom-set input-freq` to `16000000` returned `GenericError`:

```text
Attempt to set property 'input-freq' on anonymous device
(type 'renesas-cmt') after it was realized
```

TMR and SCI returned the same error with types `renesas-tmr` and
`renesas-sci`. The machine was paused with `-S`, so no guest firmware or
clock-register setting was needed to trigger the rejection.

Reproduce from this clone, keeping all scratch inside the clone:

```sh
mkdir -p .issue8-runtime
export TMPDIR="$PWD/.issue8-runtime"
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
export PYTHONPATH="$PWD/python"
../.issue6-worktree/build/pyvenv/bin/python3 - <<'PY'
from pathlib import Path
from qemu.machine import QEMUMachine

out = Path('.issue8-runtime').resolve()
binary = Path('../.issue6-worktree/build/qemu-system-rx').resolve()
with QEMUMachine(str(binary), base_temp_dir=str(out),
                 log_dir=str(out)) as vm:
    vm.set_machine('rx113-r5f51138')
    vm.add_args('-S', '-display', 'none', '-serial', 'none')
    vm.launch()
    for child in ['cmt[0]', 'tmr[0]', 'sci[0]']:
        path = '/machine/mcu/' + child
        print(path, vm.cmd('qom-get', path=path, property='input-freq'))
        result = vm.qmp('qom-set', path=path,
                        property='input-freq', value=16000000)
        print(result)
        assert result['error']['class'] == 'GenericError'
        assert 'after it was realized' in result['error']['desc']
PY
```

This checks a blocker, not the requested clock-frequency acceptance test.

## Manual findings for the next implementation

Authority: local `.issue1-runtime/rx113.pdf`, R01UH0448EJ0120 Rev.1.20,
sections 9 and 11; its existing text extraction was consulted. These are
design inputs, not implemented or validated behavior.

* Main oscillator: section 9.2.17, MOSCWTCR.MSTS values 0..7 select
  2, 1024, 2048, 4096, 8192, 16384, 32768, 65536 **LOCO** cycles.
  At nominal LOCO 4 MHz the delays are 0.5 us through 16.384 ms;
  reset value 4 means 2.048 ms. The counter automatically uses LOCO even
  if software has stopped it. Do not divide by the main crystal frequency.
* HOCO: section 9.2.18, HOCOWTCR.HSTS=5/6 selects 138/266 LOCO cycles,
  or 34.5/66.5 us. Other values are prohibited. OFS1.HOCOEN=0 has special
  reset stabilization semantics; it must not blindly use the software-start
  delay after reset release.
* OSCOVFSR section 9.2.12 has only MOOVF, PLOVF, HCOVF, UPLOVF. There
  are **no LOCO or sub-clock stability flags**. Do not allocate reserved
  bits for those oscillators. Section 9.2.9 requires a software wait for
  tLOCO; its five-cycle interval is a stop/restart constraint, not a stated
  startup stabilization time. Sections 9.2.8 and 9.8.5 require software
  waiting for sub-clock stabilization, and sub-clock operation also depends
  on RTC.RCR3.RTCEN. The five sub-clock cycles in the initialization flow
  are the stop interval, not the crystal startup stabilization time.
  Resolving the instruction's request for corresponding LOCO/sub flags is
  necessary before implementation. No delay value was adopted for them.
* PLL/USB PLL flags depend on main-clock stability and tPLL (9.2.12).
  Electrical-characteristic bounds for tPLL, tLOCO and sub-clock startup
  still need selection/justification; no arbitrary delay was adopted.
* Sections 9.2.1..3 and 9.7: select source via SCKCR3.CKSEL (LOCO,
  HOCO, main, sub, PLL for values 0..4). For valid encodings,
  `fPLL = fMain / 2^PLIDIV * multiplier`, with multiplier 6 or 8 for
  STC=0x0b or 0x0f. `ICLK = fSource / 2^ICK`,
  `PCLKB = fSource / 2^PCKB`, `PCLKD = fSource / 2^PCKD`, and
  `FCLK = fSource / 2^FCK`, with divisor encodings 0..6. Main oscillator
  frequency must be an explicit board assumption or configuration.
* UPLLCR section 9.2.5 selects **UCLK**, not PCLKB: USB PLL output is
  `fMain / 2^UPLIDIV * multiplier` for the valid encodings; UCKUPLLSEL
  chooses that output or the system clock source. CMT/TMR/SCI consume
  PCLKB (9.7.2), so UPLL changes must not spuriously change their clocks.
* Sections 11.2.2..3: TMR unit 0/1 uses MSTPA5/4; CMT unit 0/1 uses
  MSTPA15/14. SCI0/1/2/5/6/12 uses MSTPB31/30/29/26/25/4.
* Section 11.4 requires retained internal state, halted module operation,
  and normally unavailable register reads/writes during stop. A future
  RX113 MMIO wrapper could discard stopped writes and choose a documented
  deterministic value for unavailable reads, but must also pause underlying
  timers and serial receive/transmit. Such a read value is an emulation
  policy, not a hardware-guaranteed register value. Section 11.7.3 also
  requires care with already pending interrupt sources.

## Validation status and next scope decision

Runtime property rejection: reproduced for CMT, TMR, SCI. No source model
changes were made, so no post-change FIT PLL/HOCO, Phase 1, gdbsim, RX qtest,
or build result is claimed. Those gates remain **NOT RUN for Issue #8**.
The earlier Issue #6 report's passes are historical evidence only.
No new stabilization, period-change, or gate test was added because the
instruction explicitly requires stopping on a shared-model dependency.

To resume, PMO/PO must decide a separate scope for shared-model clock update
and pause/resume support, preserving RX62N behavior. That support must
handle elapsed ticks, pending event deadlines, cached SCI character timing,
receive callbacks, retained state, reset, and migration. Merely allowing
the integer property to be set at runtime is insufficient. After that,
implement RX113 SYSTEM wiring and tests, and execute all original gates
including genuine FIT PLL/HOCO with the prescribed rx-elf toolchain.

## Delivery

This report is the sole changed file and sole commit. List its exact hash:

```sh
git log --reverse --format='%H %s' \
  bd68e2952c81c763a63e3244aa6ad56a98246391..rx113/issue-8-clock-tree
```

Commit subject: `docs/rx113: Record Issue 8 clock interface blocker`.
Whitespace and per-file checkpatch validation are recorded in
`.issue8-runtime/`; the final Worker response states their outcome.
An incremental bundle is retained at
`.issue8-runtime/issue-8-clock-tree.bundle` (requires the base above).
From the original repository, once its `.git` is writable, import without
merging:

```sh
git fetch ./.issue8-worktree/.issue8-runtime/issue-8-clock-tree.bundle \
  rx113/issue-8-clock-tree:rx113/issue-8-clock-tree
```

PO owns any later merge decision. The original `rx113/dev` checkout and its
tracked files were not changed. Work stopped within the three-hour limit.
