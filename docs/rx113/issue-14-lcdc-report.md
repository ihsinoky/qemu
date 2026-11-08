<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 14: RX113 LCD controller/driver (LCDC) model

Source: R01UH0448EJ0120 Rev.1.20, section 34 (Table 5.1 for addresses,
Table 14.3 for interrupts). No new network access was needed.

## Implemented

New `hw/rx/rx113-lcdc.c` / `include/hw/rx/rx113_lcdc.h` (`rx113-lcdc`),
mapped at 0x000a0800 by `rx113.c`; it replaces the `rx113-lcdc` unimplemented
stub (same 0x100-byte window). Only RX113-specific files, build files, tests
and docs changed; shared models and RX62N are untouched.

| Item | Behaviour | Manual |
|---|---|---|
| LCDM0 (+0x00) | 8-bit R/W, reset 00h | 34.2.1 |
| LCDM1 (+0x01) | reset 00h; b2, b1 reserved, read 0 | 34.2.2 |
| LCDC0 (+0x02) | reset 00h; b7, b6 reserved, read 0 | 34.2.3 |
| VLCD (+0x03) | reset 04h; b7-b5 reserved, read 0 | 34.2.4 |
| SEG00-SEG39 (+0x40..+0x67) | 8-bit R/W display RAM, reset 00h | 34.3, Tables 34.6/34.7 |
| Other offsets | read 0, write ignored, `LOG_GUEST_ERROR` | - |

* All 40 display data registers exist (100-pin superset); 64-pin
  "N/A" segments are not distinguished.
* Display data area (34.4): static/2/3/4-slice uses the A-pattern (low
  nibble, COM0-3) or B-pattern (high nibble) selected by LCDM1.LCDSEL;
  8-slice uses all 8 bits as COM0-7 and ignores BLON/LCDSEL (Table 34.7).
* Blinking (34.4.2, Fig. 34.3): with LCDM1.BLON = 1 the pattern flips on each
  rising edge of the `prd` GPIO input (stand-in for the RTC periodic
  interrupt); blinking starts from the A-pattern.
* Driving (34.5): pixels are lit only when LCDM1.SCOC = LCDON = 1 (other
  combinations output ground level or deselect everything). The lit COM
  bits are masked to the selected time slice count (LDTY); prohibited LDTY
  lights nothing.
* Guest-error logging (`-d guest_errors`) for prohibited LCDM0 settings and
  for rewriting LCDM0/LCDC0/VLCON/LCDVLM while SCOC = 1 or VLCD while
  VLCON = 1 (the manual's "do not rewrite" notes). The writes still take
  effect.
* Interrupts: Table 14.3 defines no LCDC interrupt vector, so none is wired
  (blink timing comes from the RTC PRD, which is not modeled).
* Reset and migration (vmstate v1) are implemented.

## Reading the segment state from outside the guest

QOM properties on `/machine/mcu/lcdc` (`qom-get` over QMP, or `qom-get` in
the HMP; `qtest_set_irq_in(..., "prd", ...)` pulses the blink input):

* `display-ram`: 80 hex digits, raw SEG00..SEG39 registers.
* `segments`: 80 hex digits, state driven to the panel per SEGnn; bit n set
  means the SEGnn/COMn pixel is lit (0 when not driven).
* `driving` (bool), `pattern` ("A"/"B"), `time-slices` (uint8),
  `blink-phase` (bool).

## Out of scope

Waveform A/B, bias and boost voltages (LCDM0 fields, VLCD, LCDVLM, VLCON
are readback only; boost/capacitor-split need not be enabled for pixels to
show), LCD clock divider/frame timing (LCDC0), module stop gating (MSTPCRD
b11), LCDSCLKCR/LCDSCLKCR2 source clock registers in SYSTEM, port function
selection (PmnPFS/PMR), and the RTC PRD source itself. LCDC rewrite
restrictions are logged, not enforced.

## Verification

* `tests/qtest/rx113-lcdc-test.c` (6 cases): reset values and system reset,
  reserved-bit masking, word access, reserved offsets, display RAM
  read/write, segment state (A/B pattern, enable bits, static/4/8-slice),
  blinking via PRD. All pass.
* `tests/functional/rx/test_rx113_lcdc.py`: a guest program sets up the LCDC
  and writes display RAM; the test reads `display-ram`/`segments` via QMP
  and checks they match. Passes.
* Regressions: `make check-functional-rx` (rx113, rx113_fpu, gdbsim: OK),
  `rx-icu-test` (OK), `-M help` lists gdbsim and rx113 machines, FIT probes
  `verify.py` (PLL and HOCO) and `system.py` (flash and ELF) all pass.
* `scripts/checkpatch.pl` on all changed/new files: 0 errors, 0 warnings.

## Remaining

* Connect `prd` to the RTC once it is modeled.
* SYSTEM LCDSCLKCR/LCDSCLKCR2 and MSTPCRD, pin-function routing.
* Frame-rate/voltage behaviour if ever needed.
