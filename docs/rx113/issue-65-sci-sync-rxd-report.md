# Issue #65 report: SCI clock-synchronous transmission and RXD pad reception

HW manual: R01UH0448EJ0120 Rev.1.20, sections 19 and 28.

Issue #50 stage 2 gave the shared SCI an opt-in TXD bit engine for
asynchronous frames and left two gaps: clock-synchronous frames were
not serialized, and reception existed only through the chardev
backend (no RXD pad input). This issue closes both, in two stages.
Stage 1 (this report section) covers synchronous transmission and
the SCK pad; stage 2 adds RXD pad sampling.

## Stage 1: synchronous TXD serialization and SCK output

### Model changes (shared SCI, opt-in via the existing `has-output`)

- A synchronous frame (SMR.CM = 1) on the internal clock
  (SCR.CKE bit 1 = 0, section 28.2.6) is serialized onto TXD by the
  same bit timer as the asynchronous engine, stepped in half-bit
  phases: eight data bits, no start/parity/stop, bit order from
  SCMR.SDIR, SINV applied to the data bits as on the async path.
  TXD changes on the falling edges of SCK and holds the last bit
  after the frame (section 28.5).
- The SCI gains `sck` / `sck-oe` GPIO outputs next to `txd` /
  `txd-oe`. SCK is driven while TE = 1 in synchronous mode on the
  internal clock: it idles high and pulses once per bit (low for
  the first half of the bit, high for the second). The VMState
  `renesas-sci/output` subsection moves to version 2 with the
  synchronous frame state.
- The frame timing follows the Issue #50 rule: the waveform rides
  the model's existing transfer timing, and one synchronous bit
  lasts one model slot (trtime / slots), the Issue #10 baud formula
  included. The pending-byte slot behaves as on the async path, so
  back-to-back synchronous frames have no idle gap.
- A synchronous frame on an external clock is not serialized: it
  advances on SCK input edges, which stage 2 adds. The model's
  register-level timing (TDRE/TEND/interrupts) is unaffected.

### PORT and machine wiring

- The PORT gains an SCK signal map (Tables 19.3 to 19.16, PSEL 10,
  11 and 12) in the TXD channel order, `sck` / `sck-oe` state fed
  by 16 named GPIO inputs mirroring the TXD plumbing, and a
  VMState bump to version 7. The PMR-write "unsupported function"
  log now also accepts SCK selections.
- rx113.c connects each channel's `sck` / `sck-oe` outputs to the
  PORT with the same per-unit signal indices as TXD.

### Tests and verification

- tests/qtest/rx113-sci-txd-test grows two cases: `sync-tx` checks
  an LSB-first and an MSB-first frame half-bit by half-bit (SCK
  low/high phases, TXD bit sequence, idle-high SCK, last-bit hold,
  TEND), and `sync-external-clock` checks that an external clock
  selection leaves SCK undriven and TXD at its idle level.
- Existing suite: 7/7 (5 previous + 2 new). Full regression and
  checkpatch results: see the PR comment.

### Left for stage 2 / later

- RXD pad sampling (async and sync), including the reception-only
  clock generation of section 28.5.1 and the SCK input path for
  external-clock operation (both directions).
- The asynchronous-mode SCK bit-clock output (SCR.CKE = 01 in
  async mode) is not modelled.
