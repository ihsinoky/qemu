<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 63: RX113 LCDC COM/SEG pad routing (Stage 2)

Source: R01UH0448EJ0120 Rev.1.20, sections 19.3.4, 34.2.2, 34.5,
34.8 and Tables 19.3 to 19.16, 34.10 and 34.11, Figures 34.15 to
34.17. Design points are recorded as D-023 in
`docs/rx113/decisions.md`.

## Implemented

| Item | Behaviour | Manual |
|---|---|---|
| Pin map | All LCDC pin functions use PSEL = 24. COM0-3 on PC5-PC2, COM4-7 shared with SEG11/12/13/15 on PB7/PB6/PB5/PB3, the remaining SEG00-SEG39 on Ports 1, 5, A, B, C, D, E, F per the tables | Tables 19.3-19.16 |
| Pad source | The PORT pulls the LCDC pad state through the `lcdc` link on every pad update; the LCDC projects its scan lazily as in Issue #61, so no timer is added. The PORT `levels`/`driving` QOM getters now refresh the pads before reading, so observations always show the current scan position | 34.5 |
| Selected common | Driven, at the drive-phase level (waveform A inverts every half LCDCL period, B every full period). In static mode COM0-3 all carry the same selected signal | Figures 34.15-34.17, 34.8 (1) |
| Segments | Driven while SCOC = 1. A segment whose data bit for the current slice is 1 (select voltage) is driven opposite to the selected common; a 0 bit (deselect) follows the common's level, so a pixel is lit exactly when its segment is anti-phase to its common | 34.8 (2), Table 34.11 |
| Dual pads | PB3/PB5/PB6/PB7 drive their COM function in 8-time-slice mode and their SEG function otherwise; in 8-slice mode SEG11-SEG13 and SEG15 are not segment outputs | 34.8 (2) |
| SCOC = 0 | Ground level (driven, level 0) on all LCDC pins | 34.2.2 |
| Module stop | A stopped LCDC drives nothing; the pins fall back to the PORT's undriven state | 11.4 |

Digital abstraction (D-023): pad levels are boolean, so the bias
voltages of Table 34.11 have no representation. Non-selected
commons sit at a deselect bias level on hardware; here they are
left undriven, which keeps the selection observable through the
PORT `driving` mask and never produces a false full-swing
differential across an unselected pixel. VL1-VL4 and CAPH/CAPL
pins (also PSEL = 24) are analog power pins and stay unmodelled.

Migration: no new state. The PORT gains only the `lcdc` link
(set in `register_port()` before realize, like the POE links);
the pad state is recomputed from the LCDC on every update.

## Verification

* `tests/qtest/rx113-lcdc-test.c`: 4 new cases (15 total).
  pads-scan: PSEL/PMR setup per 19.3.4, COM0/COM1 hand-over across
  slices, waveform A phase inversion, a lit segment anti-phase to
  its common and an unlit one in phase, the dual PB7 pad acting
  as SEG11 in 4-slice mode. pads-waveform-b: the phase holds for
  a whole slice. pads-ground-stop: ground drive at SCOC = 0 and
  no drive under module stop. pads-8-slice: PB7 idles until
  slice 4, then drives as COM4.
* Full qtest regression: 19 suites, 485 tests, 0 failures
  (481 at the previous baseline + 4 new LCDC pad cases).
  Functional tests rx113_lcdc, rx113, rx113_fpu and rx113_iwdt
  all pass. `scripts/checkpatch.pl` on all changed C files:
  0 errors, 0 warnings (the missing Signed-off-by notice appears
  only when checking a raw diff). No existing test needed any
  adjustment for this change.
