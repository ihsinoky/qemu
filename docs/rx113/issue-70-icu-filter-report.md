# Issue #70 report: ICU digital filter data path

Implements the section 14.4.7 digital filters left register-only
by Issue #58 (D-021): the NMI pin filter (NMIFLTE/NMIFLTC) and
the IRQ0-7 pin filters (IRQFLTE0/IRQFLTC0). D-026 records the
design decisions.

## What was built

- **Filter engine** (shared ICU model, behind the existing
  RX113 opt-in): nine channels (IRQ0-7 plus the NMI pin). A
  channel's output level - the level the IRQ sense logic and
  the NMI edge detector see - changes only after the input has
  been sampled at the new level three times in a row; pulses
  shorter than three sampling cycles never reach detection.
  Sampling periods are the ICU clock (ICLK) divided by
  1/8/32/64 per FCLKSEL/NFCLKSEL. One QEMUTimer serves all
  channels, armed at the earliest pending sample, so a flip is
  delivered at its deadline without further guest activity.
  The sampling phase is anchored at the moment the input
  diverges from the output (D-026).
- **Decode**: IRQFLTE0 (0x87510) and IRQFLTC0 (0x87514,
  16-bit, byte-accessible) are new. The IRQCR decode window
  narrows from 0x500-0x51f to the eight real registers
  0x500-0x507 (it previously shadowed the filter registers).
  NMIFLTC moves from offset 0x591 to the manual's 0x594 -
  the old decode was simply the wrong address, and the
  Issue #58 storage test pointed at it; the test define is
  corrected in this PR.
- **Clock delivery**: SYSTEM now delivers ICLK (source clock
  divided by SCKCR.ICK) to the ICU through a new
  `rx_icu_set_clock()` API, alongside the other per-device
  deliveries. The SCKCR field mask is corrected from
  0xff000f0f to 0x0f0f0f0f so the ICK field (and the other
  divider fields) is actually stored; bits 31:28 are reserved
  on hardware and no longer stick. An ICK value above 6 is
  rejected like the existing PCLKB divider check.
- **Configuration changes**: disabling a filter makes its
  output follow the input immediately (the figure 14.7
  behaviour); enabling one starts sampling from the current
  level with a clean count; changing a sampling-clock select
  or ICLK re-anchors an in-flight sequence under the new
  period. The internal NMI sources (OSTDI, WUNI) are not
  filtered - the filter sits on the pin only, per the manual.
- **Migration**: ICU VMState goes to version 4 with the new
  registers, the delivered clock and the raw/output levels.
  In-flight sample counts are not migrated; post-load
  restarts a pending sequence from the migrated levels.

## The TSR question that started this issue

The theme originally proposed for this slot was "MTU TSR flag
storage", carried as a gap since Issue #68. Checking the
manual first refuted it: section 20.2.6 gives the RX113 TSR
no match flags at all (bits 5-0 reserved/undefined, bit 7 is
TCFD). The constant read is conforming, no code change was
needed or made, and findings entry F-006 records the result
(with an erratum in the Issue #68 report) so the item is not
re-proposed.

## Boundaries kept

- Everything sits behind the RX113 opt-in; the gdbsim
  (RX62N) ICU sees none of it. rx-icu-test and the gdbsim
  functional tests pass unmodified.
- With every filter disabled, IRQ and NMI behaviour is
  bit-identical: the nine pre-existing rx113-nmi tests pass;
  one test define changed (the NMIFLTC address above).
- Standby entry/exit (section 14.4.7 asks software to disable
  the filters before software standby) is not modelled, as
  standby itself is not modelled.

## Verification

- New qtests in rx113-nmi-test (suite now 13): filter
  register decode (including NMIFLTC at 0x594 and the old
  slot reading back 0xff), NMI filter removal/pass-through
  at ICLK sampling, the /64 divider widening the removable
  pulse, IRQ0 falling-edge filtering, and level-mode
  follow-through with the three-sample delay.
- Full regression (20 suites), functional tests and
  checkpatch: see the PR body for the measured results.
