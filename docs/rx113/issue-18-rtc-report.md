<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 18: RX113 realtime clock (RTCA) model

Source: R01UH0448EJ0120 Rev.1.20, section 24 (Table 5.1 for addresses,
Table 14.3 for interrupts). No new network access was needed (the FIT probe
reused the `r_bsp.zip` already fetched for Issue 6).

## Implemented

New `hw/rx/rx113-rtc.c` / `include/hw/rx/rx113_rtc.h` (`rx113-rtc`) replaces
the `rx113-rtc` unimplemented-device stub at 0x0008c400 (size 0x100).
Wiring is in `hw/rx/rx113.c` (`register_rtc()`); QOM path
`/machine/mcu/rtc`. RX62N and shared models are untouched; the LCDC is
unchanged (its existing `prd` GPIO input now has a driver).

| Item | Manual basis |
|------|--------------|
| Register layout/offsets (R64CNT 0x00 ... RADJ 0x2e), 8/16-bit widths, reserved-bit read behavior | 24.2, Table 5.1 |
| Calendar mode: BCD sec/min/hour/weekday/date/month/year, 2000-2099 leap rule, 24h and 12h (PM bit) modes | 24.2.2-24.2.8, 24.1 |
| Binary mode: 32-bit BCNT0-3 counted per second, selected by RCR2.CNTMD | 24.2.2-24.2.5, 24.2.18 |
| R64CNT: 128 Hz counter (bit0 = 64 Hz ... bit6 = 1 Hz) | 24.2.1 |
| Alarm: ENB-gated compare of sec/min/hour/weekday/date/month/year (calendar) or ENB[31:0]-masked BCNT compare (binary); flag set on a fresh match only | 24.2.9-24.2.16, 24.4 (1) |
| RCR1: AIE, CIE, PIE, PES[3:0] (0110b = 1/256 s ... 1110b = 1 s, 1111b = 2 s; others disable) | 24.2.17 |
| RCR2: START, RESET (clears prescaler, alarm regs, RADJ, AADJE/AADJP), ADJ30 (round to nearest minute, calendar mode), HR24, CNTMD; RESET/ADJ30 read back 0 | 24.2.18, 24.3.3, 24.3.4 |
| Time base: virtual clock, 1/256 s = 3906250 ns, exact periods | 24.1 |

## Interrupt vectors (Table 14.3)

| Source | Vector | IER | IPR |
|--------|--------|-----|-----|
| CUP | 63 | IER07.IEN7 | IPR063 |
| ALM | 92 | IER0B.IEN4 | IPR092 |
| PRD | 93 | IER0B.IEN5 | IPR093 |

All are edge-detected in the ICU (pulse). PRD additionally drives the LCDC
`prd` input through a second sysbus output, so the LCDC blink phase
(`blink-phase`) toggles with every PRD while LCDM1.BLON is set (34.4.2).
This closes the remaining item of the Issue 14 report.

## Out of scope / retained only / simplifications

- RCR3 (sub-clock oscillator control), RCR1.RTCOS, RCR2.RTCOE (RTCOUT) and
  RADJ with RCR2.AADJE/AADJP: stored and read back only; no effect on counting
  (time error adjustment and RTCOUT are not modeled). VBAT backup is not
  modeled; the RTC always counts the virtual clock while START = 1,
  independent of SYSTEM sub-clock control.
- CUP is raised only on the carry into RSECCNT/BCNT0, not on the
  "carry to R64CNT while R64CNT is read" condition (24.4 (3)).
- Register writes take effect immediately (no count-source synchronization
  delay); writes to counters while START = 1 are accepted.
- System-reset retention is implemented by the follow-up below. Undefined
  power-on values remain deterministically zero.
- Alarm with no ENB bit set never matches (manual does not define it).
- Alarm interrupts are forwarded only while AIE = 1 at the time of the match.
- Out-of-range BCD values are handled without crashing (no defined behavior in
  the manual: "does not operate correctly").
- Software standby recovery by ALM/PRD is not modeled.

## Verification

New `tests/qtest/rx113-rtc-test.c` (8 cases, registered for `CONFIG_RX113`):
set/readback and counting incl. BCD carries (leap day, month, century wrap,
R64CNT), 12-hour rollover, ADJ30/software reset, binary-mode carry across
lanes, register widths, PRD at all 10 selectable periods observed via
ICU.IR[93] (no early IRQ, IRQ exactly on the period, none for PIE = 0 or
reserved PES), alarm (ALM, vector 92) and carry (CUP, vector 63) interrupts,
and RTC PRD toggling LCDC `blink-phase`.

Results (build/ in this worktree, `--target-list=rx-softmmu`):

- `rx113-rtc-test`: 8/8 pass.
- `meson test --suite qtest-rx`: 11 OK, 2 skipped, 0 fail (includes
  rx-icu-test, rx113-lcdc-test).
- `meson test --suite func-rx`: 7 OK, 2 skipped (linters, info_usernet), 0
  fail; includes rx113, rx113_fpu (Phase 1/FPU/part variants), rx113_lcdc.
- FIT probe: PLL and HOCO `verify.py` pass; `system.py` passes.
- `qemu-system-rx -M help` lists the gdbsim machines.
- `scripts/checkpatch.pl --no-tree -f` on all changed C/header/meson files:
  0 errors, 0 warnings.

## Remaining

- CUP on R64CNT-read carry, RTCOUT, time error adjustment, standby wake-up.
- Link RCR3.RTCEN / SOSCCR to the RTC count source if clock gating becomes
  relevant.

## Follow-up: retain RTC state across machine reset

The RTC now retains its counters, alarm registers, adjustment settings and
reset-retained control bits across machine resets. The virtual-time anchor,
sub-second prescaler phase, periodic interrupt phase and alarm-match state
also survive, so both calendar and binary counting continue from the previous
state, including fractional seconds. A stopped counter remains stopped.
Reset only clears RCR1.CIE, RCR1.RTCOS and RCR2.RTCOE; timer scheduling is
updated to account for CIE being disabled. The reset callback does not emit
interrupts while other devices are being reset.

Manual basis, checked against `.issue18-runtime/rx113.pdf`,
R01UH0448EJ0120 Rev.1.20, section 24:

- 24.2 explicitly says that bits marked `x` are not initialized by reset and
  that the counters continue while START is 1, including during reset.
- 24.2.1-24.2.16 mark counter and alarm data as retained (`x`); reserved
  read-as-zero fields keep their existing masks.
- 24.2.17 gives RCR1's reset value as `xxxx0x0x`: CIE and RTCOS clear;
  AIE, PIE and PES retain their settings.
- 24.2.18 gives RCR2's reset value as `xxxx000x`: RTCOE clears, START,
  AADJE, AADJP, HR24 and CNTMD retain their settings. RESET and ADJ30 already
  self-clear in the model. The software-reset targets in note 1 are unchanged.
- 24.2.19-24.2.20 retain RCR3's low four bits and all of RADJ.
- 24.3.1 requires initial settings after power-on; 24.5.7 reiterates that RTC
  registers are not initialized by reset.

The initial device instance still starts with all register data zero
(including the model's deterministic choice for undefined power-on bits),
using QOM's zero initialization. This is distinct from later machine resets:
QEMU also passes RESET_TYPE_COLD for QMP `system_reset`, so that reset type
cannot be used to identify the first power-on. No new state fields or VMState
format changes are needed. Guest software reset and ADJ30 code are unchanged.

Four qtests are added without changing the original eight expectations:

- `power-on`: zero initial register values and stopped initial counter.
- `system-reset-calendar`: running calendar retention across repeated QMP
  resets, leap-day carry, fractional tick and periodic phase continuity,
  alarm/adjustment/control retention, clearing only the specified control
  bits, continued ALM/PRD and disabled CUP, and subsequent software reset.
- `system-reset-binary`: retained binary mode, all counter lanes, alarm and
  enable lanes, carry and fractional-second continuity with no interrupt
  timers or register reads before reset (lazy synchronization).
- `system-reset-stopped`: retained calendar data and sub-second phase while
  stopped, then continued counting after guest restart.

Remaining differences: the existing oscillator/clock-gating, RTCOUT,
time-adjustment, CUP-on-R64CNT-read, standby and register-synchronization
limitations above remain. In particular, the 1/128-second wait before
reading after reset and six count-source cycles before writes (24.5.5) are
not enforced; accesses remain immediate. This change covers resets within a
running QEMU instance; actual power loss/VBAT persistence is not modeled.


Follow-up verification uses `build/` (`TARGET_DIRS=rx-softmmu`), with
`TMPDIR=$PWD/.t` and `~/workspace/tools/build-venv/bin` first on PATH.
Evidence is in `.issue18-runtime/followup/`:

- `rtc-qtest.log`: 12/12 pass, including all eight original cases.
- Fresh FIT builds using the Issue 6 `rx-elf` toolchain and the existing
  verified R_BSP archive: PLL and HOCO `verify.py` pass, including SCI0
  output before and after QMP reset (`verify-pll/`, `verify-hoco/`).
- `system.log`, `system-elf.log`: SYSTEM probe passes both without an ELF
  and with the freshly built PLL ELF.
- `gdbsim-smoke.log`: both gdbsim machines stay running for three seconds,
  then exit 0 on SIGTERM. This is an idle-machine smoke, not Linux boot.
  `machine-help.log` lists both machines.
- `checkpatch.log`: both changed C files have 0 errors and 0 warnings.
  The complete diff also has 0 errors and 0 warnings with
  `scripts/checkpatch.pl --no-tree --no-signoff -` (`checkpatch-diff.log`);
  `git diff --check` passes.

The first combined suite run (`regression.log`) passed all RX-specific
functional tests and RX qtests, but three generic functional programs
(`monitor_hotplug`, `version`, `vnc`) failed because QEMUMachine's default
scratch directory is hard-coded to `/var/tmp`, which is read-only here.
For the repeat run, the untracked execution helper
`test-env/sitecustomize.py` changes only that constructor default to
`TMPDIR=$PWD/.t`; no repository test or product source is changed. The
repeat uses `--no-rebuild` with the just-built binaries, avoiding another
unnecessary build-log recovery/recompile. The two skipped qtests are cdrom
and qos; the two skipped functional programs are info_usernet and linters.

Delivery: staging failed with `Unable to create .../.git/index.lock:
Read-only file system`, so no follow-up commit could be created. Changes
remain in the working tree in `hw/rx/rx113-rtc.c`,
`tests/qtest/rx113-rtc-test.c`, and this report. No push or merge was done.

With the scratch-directory helper, `meson test -C build --no-rebuild
--suite qtest-rx --suite func-rx --print-errorlogs` passes: RX qtest 11 OK /
2 skipped, functional 7 OK / 2 skipped, total 18 OK / 4 skipped / 0 failed
(`regression-scratch.log`). This includes 8 Phase 1/part-variant boot cases,
2 FPU cases, 1 LCDC functional case, 5 ICU qtests, 6 LCDC qtests and all
12 RTC qtests. The helper skips loading QEMUMachine in the Meson launcher's
Python when qemu.qmp is absent; `regression-final.log` records the repeat
with this launcher-only warning removed.
