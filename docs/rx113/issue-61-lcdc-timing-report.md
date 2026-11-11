<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 61: RX113 LCDC frame timing

Source: R01UH0448EJ0120 Rev.1.20, sections 9.2.15-16, 11.4,
34.2.1-34.2.3, 34.5 and Figures 34.13-34.17. Design points are
recorded as D-022 in `docs/rx113/decisions.md`.

## Implemented

| Item | Behaviour | Manual |
|---|---|---|
| LCDSCLKCR (0x80050) | LCDSCLKSEL[2:0] source select, reserved bits read 0, PRC0-protected | 9.2.15 |
| LCDSCLKCR2 (0x80051) | LCDSCLKSTP, reset value 1, PRC0-protected | 9.2.16 |
| Clock delivery | SYSTEM computes the selected source (LOCO/HOCO/main/sub/IWDT) with the CAC availability rules; LCDSCLKSTP folds to 0 Hz; MSTPCRD b11 delivered as the module-stop flag | 9.2.15, 11.2.5 |
| LCDC0 divider | Per-source-kind tables: main group codes 11h-1Bh -> 2^(code-9), 2Bh -> 2^19; sub 04h-09h and IWDT 03h-07h -> 2^(code+1); other codes prohibited (guest-error log, no LCD clock) | 34.2.3 |
| Frame scan | Position in half-LCDCL steps, projected lazily from the virtual clock; runs while SCOC = 1; frame frequency = LCDCL / time slices (Figure 34.14's worked example reproduces 64 Hz) | 34.5, 34.8 |
| Drive phase | Waveform A inverts every half period, waveform B every full period | Figures 34.15-34.17 |
| Module stop | While MSTPCRD b11 is set: LCDC reads return 0, writes are dropped, state (including the scan position) is held | 11.4 |
| Observability | QOM additions on `/machine/mcu/lcdc`: `frame-frequency` (Hz, truncated), `scan-slice`, `drive-phase`; existing properties unchanged | fork-local |

Migration: LCDC VMState version 2 (delivered clock state and scan
step appended; the anchor is re-based on load), SYSTEM VMState
version 8 (the two new registers appended, defaults restored for
older versions). The RTC PRD -> LCDC blink wiring already existed
(Issue #18); a new qtest now covers it end to end.

## Verification

* `tests/qtest/rx113-lcdc-test.c`: 11 cases (6 existing, adjusted
  only to release the module stop and to re-release it after a
  system reset, plus 5 new): clock register reset values,
  protection and masking; frame frequency for the sub-clock worked
  example, a LOCO/main-group combination and a prohibited code;
  scan-slice progression and waveform A/B phase cadence; freeze
  under LCDSCLKSTP and under module stop with state retention;
  blinking driven by the real RTC PRD. All pass. The two blink
  setups in `tests/qtest/rx113-rtc-test.c` received the same
  module-stop release.
* `tests/functional/rx/test_rx113_lcdc.py`: the guest program now
  releases the LCDC module stop before use (hand-assembled prefix,
  verified with rx-elf-objdump), as real guest software must. Passes,
  as do the rx113 and rx113_fpu functional tests.
* Full qtest regression: 19 suites, 481 tests, 0 failures
  (476 at the previous baseline + 5 new LCDC cases).
* `-M help` lists all four rx113 machines and the gdbsim machines.
* `scripts/checkpatch.pl` on all changed C files: 0 errors,
  0 warnings (the missing Signed-off-by notice appears only when
  checking a raw diff).

## Notes and leftovers

* A defect was caught by the new scan test during development:
  the step projection originally passed a 64-bit divisor to
  `muldiv64`, whose factors are 32-bit; the projection now counts
  source cycles first and divides after.
* The scan position is quantized to half an LCDCL period and is
  materialized lazily; sub-half-period remainders are dropped at
  reconfiguration points. No timer is run for the LCDC.
* Out of scope, unchanged from Issue #14: COM/SEG pad routing
  through the PORT (the pin map is Table 34.2), bias/boost analog
  voltages, and enforcement of the LCDC rewrite restrictions
  (still logged only).
