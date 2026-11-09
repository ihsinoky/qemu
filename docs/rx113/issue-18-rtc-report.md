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
- A system reset clears all registers (reset values undefined in the manual
  are 0); real hardware keeps counting through a reset (24.2).
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
