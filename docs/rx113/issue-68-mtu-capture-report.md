# Issue #68 report: MTU input capture from MTIOC pin edges

Implements the pin-edge input capture path of section 20
(R01UH0448EJ0120 Rev.1.20). Until now a TIOR setting selecting
input capture only logged "input capture path unavailable"; the
capture core existed for ELC operation inputs and for the MTU5
PWM measurement mode, but no pin edge could ever trigger it.
Design record: D-025 in docs/rx113/decisions.md.

## What was built

- The PORT forwards the resolved level of every MTIOC pad to the
  MTU on a new `mtioc-in` named GPIO set, in the same signal
  index space as the existing `mtioc` outputs. A channel with no
  selected pad reads as high, the convention the SCI forwarding
  established.
- The MTU tracks the previous levels and detects edges. A
  capture fires when the channel is running in normal mode and
  the general register's TIOR field selects input capture
  (1000 rising, 1001 falling, 101x both edges). The timers are
  advanced first, so the captured value is the count at the
  edge; the interrupt and ELC event go through the compare-match
  path (`mtu_match`), and a TCR.CCLR designation of the register
  clears the counter with the same buffer handling the ELC
  operation capture uses (Figure 20.11's shape).
- Capture-capable registers follow Table 1.5: MTU0 TGRA-TGRD,
  MTU1/2/3/4 TGRA/TGRB. A register serving as a buffer register
  (TGRC under BFA, TGRD under BFB) does not capture; a capture
  into TGRA/TGRB writes the register itself.
- TICCR (section 20.2.8, previously not decoded) is implemented:
  with I1AE/I1BE set, an MTIOC1A/1B edge also captures into
  MTU2.TGRA/TGRB under MTU2's own TIOR edge selection and gates.
- Migration state grows by `ticcr` and the tracked input levels
  (vmstate version 12).

## The advance discipline (found during development)

The first version advanced the timers on every forwarded edge.
That broke the complementary-PWM suite
(complementary-tgcr-migration): the MTIOC pins are shared with
the timer outputs, so the model's own output transitions fed
back as input events and re-entered the timing engine in the
middle of transfer bookkeeping. The handler now advances only
when a capture is actually armed for the pin's channel (or for
MTU2 through TICCR); otherwise it just tracks the level. This
rule is recorded as part of D-025.

## Deliberately out of scope

- The MTU0 TIOR 11xx capture source (capture on MTU1.TCNT count
  edges) and capture selection on MTIOC3C/3D and MTIOC4C/4D
  (output-only pins) keep the LOG_UNIMP notice, now narrowed to
  exactly those selections.
- TSR flag storage: the MTU model has never stored TSR flags
  (reads return a constant); that is an MTU-wide pre-existing
  gap affecting compare matches and overflows equally, and is
  left for a separate theme rather than fixed for capture only.
- Capture in PWM / complementary / reset-synchronized modes:
  gated to normal mode, mirroring the ELC operation precedent.

## Tests and verification

- New qtests in rx113-mtu-test: capture-edges (rising, falling
  and both-edge selection on MTIOC0A, values tracking the count,
  TGIA interrupt in the ICU, no capture while stopped),
  capture-clear (Figure 20.11: TGRB capture clearing TCNT),
  capture-ticcr (MTIOC1A capturing into MTU1 only while
  TICCR=0, and into both MTU1 and MTU2 once I1AE is set).
- Full regression and checkpatch results: see the PR comment.
