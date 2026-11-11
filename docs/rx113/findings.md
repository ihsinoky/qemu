# RX113 emulation findings ledger

Empirical findings from external verification of emulation premises
(Issue #51). Format per entry: conclusion, confidence, conditions,
evidence, validity boundary, promoted-to. Newest first.

## F-001: ICSR3.OSTSTE is write-once; the D-018 "clear always accepted" reading is probably wrong (2026-11-11)

- **Conclusion:** On real hardware, POE2a's configuration registers —
  including ICSR3, which holds OSTSTE — can be written only once
  after a reset. The vendor's own recovery flow for an
  oscillation-stop trip clears OSTSTF (and OSTDSR.OSTDF), never
  OSTSTE, and the RX24U application note directs recovery by system
  reset. D-018's composition (set once, clear always accepted,
  re-set barred) therefore most likely does not match hardware: the
  current model accepts a release path (clear OSTSTE to drop the
  high-impedance request) that hardware probably ignores, i.e. the
  model is looser than hardware in exactly the way flagged when
  D-018 was adopted.
- **Confidence:** High-probable, not proven. Four independent vendor
  sources converge; no document states the silicon behaviour of an
  OSTSTE clear directly, and no errata/technical-update/KB item on
  the point was found.
- **Evidence:**
  1. RX113 manual R01UH0448EJ0120 §21.2.7: OSTSTE is R/W*1 with
     Note 1 "Can be modified only once after a reset." The same
     note marks the configuration fields of ICSR1, OCSR1, ICSR2,
     POECR1 and POECR2, while SPOER (the runtime control register)
     carries no such note and documents free read-1/write-0
     clearing for its bits — a deliberate contrast.
  2. Renesas migration guide R01AN3669EJ0100 (RX23T, comparing
     POE2a/POE3 generations): "Among the registers used by the POE
     function, the following can be written to only once after a
     reset… ICSR1, ICSR3, ICSR4, ICSR6, OCSR1, ALR1, POECR1,
     POECR2, POECR4, POECR5, POECMPSEL."
  3. Renesas application note R01AN4140JJ0100 (RX24U POE3A +
     MTU3d, "PWM output stop and recovery"): §1 and §4.1 state
     that after an oscillation-stop trip the system is recovered
     by a system reset; §4.1.5's recovery procedure clears
     OSTDSR.OSTDF, reconfigures the clocks and clears
     ICSR6.OSTSTF — OSTSTE is never written after arming. The
     sample's oscillation-stop NMI handler waits for a reset.
  4. RZ/T1 POE3 sample program note (R01AN… GPTA/POE3): the POE3
     configuration commands "can be used only once; once issued,
     they cannot be changed until the RZ/T1 is reset" — the family
     treats POE configuration as set-once by design.
  The only support for D-018's reading is the RX113 §21.3.5
  sentence "clearing the ICSR3.OSTSTF or ICSR3.OSTSTE bit releases
  the pins", which reads coherently as a statement of the logical
  release condition, not an operable procedure.
- **Conditions / open sub-question:** The write-once composition
  still has one unresolved point: whether the latch is consumed by
  the first write to ICSR3 at all (which would make an early
  OSTSTF-clear write lock OSTSTE at 0), or only by the first write
  that presents a new OSTSTE value. The flag-clear path (OSTSTF,
  R/(W) with its own note) must remain usable repeatedly under
  either reading, since recovery depends on it.
- **Validity boundary:** POE2a on RX113 as documented at Rev.1.20;
  corroborated across POE3/POE3a/POE3b generations. Not a
  substitute for a vendor answer or silicon test.
- **Promoted-to:** Issue #51; supersede of D-018 raised as a PO
  gate on 2026-11-11 (decision pending).

## F-002: D-015 (complementary-PWM duty interrupts only on own-counter matches) is supported; no contradicting evidence (2026-11-11)

- **Conclusion:** The current reading stands. Every source defines
  TGIB3/TGIA4/TGIB4 as their own channel's TCNT-vs-TGR compare
  match, including in complementary PWM mode; nothing ties their
  generation to the partner channel's counter or to buffer-transfer
  timing.
- **Confidence:** Moderate-high. The manual's definition is direct;
  external corroboration is consistent but comes from the sibling
  MTU3a IP rather than RX113 silicon reports.
- **Evidence:**
  1. RX113 manual Table 20.57 (MTU Interrupt Sources): TGIB3 =
     "MTU3.TGRB input capture/compare match", TGIA4/TGIB4 =
     "MTU4.TGRA/TGRB input capture/compare match" — own-channel
     definitions with no complementary-mode exception.
  2. RX113 manual §20.5 (dead time compensation procedure,
     Figure 20.81): in complementary PWM mode the interrupts used
     are "generated at the crest (TGIA3) or trough (TCIV4)" — the
     crest interrupt is MTU3's own TGRA match, the trough is
     MTU4's own overflow/underflow.
  3. RX113 manual §20.6.14 (buffer operation in complementary PWM
     mode) constrains buffer settings and says nothing about
     interrupt generation moving to the partner channel.
  4. Linux kernel devicetree bindings for the sibling Renesas
     MTU3a (RZ/G2L family; reviewed with Renesas maintainers):
     tgib3 is documented as "MTU3.TGRB input capture/compare
     match", tgib4 as "MTU4.TGRB input capture/compare match".
- **Conditions:** Applies to the TGI requests gated by each
  channel's TIER and the TITCR skipping machinery, which are
  orthogonal and unchanged.
- **Validity boundary:** Documentary evidence only; no silicon
  measurement. The residual scenario — hardware additionally
  raising TGIB3 at MTU4-timed transfer points — has no supporting
  text anywhere found and would contradict Table 20.57.
- **Promoted-to:** Issue #51. No supersede proposed; D-015 stands.

## F-003: Cascade TMO — the manual is explicit (§22.5.1(2)), and the model drops TMO1 in 16-bit count mode (2026-11-11)

- **Conclusion:** The Issue #50 stage-1 report recorded the cascade
  output behaviour as an unverified reading because "the manual
  does not state" it. That premise was an oversight: §22.5.1(2)
  states it directly. In 16-bit count mode, TMO0 follows the
  16-bit compare match conditions and TMO1 follows the lower
  8-bit compare match conditions, each under its own channel's
  TCSR OSA/OSB. In compare match count mode (§22.5.2) the
  channels are independent. The model matches TMO0 and the count
  mode, but in 16-bit count mode channel 1 never receives match
  events (tmr_div() returns 0 for channel 1 when tmr_wide(), so
  its timer is never scheduled), hence TMO1 is never
  match-driven. The same missing events also suppress CMIA1/
  CMIB1 in that mode; §22.6 lists those sources unconditionally.
- **Confidence:** Proven from the manual text for the pin
  behaviour; the interrupt side follows from the same event
  mechanism and the unconditional §22.6 table.
- **Evidence:**
  1. RX113 manual §22.5.1(2) "Pin Output" (16-bit count mode):
     TMO0 control "is in accordance with the 16-bit compare match
     conditions"; TMO1 control "is in accordance with the lower
     8-bit compare match conditions".
  2. RX113 manual §22.5.2: in compare match count mode,
     interrupts, TMOn output and counter clear follow each
     channel's own settings (the model conforms here).
  3. Code: hw/timer/renesas_tmr.c — update_events() computes
     channel 0 matches on the concatenated 16-bit counter in
     cascade mode and returns early for channel 1; tmr_div()
     returns 0 for channel 1 under tmr_wide(); the cascaded
     channel-1 match generation in tmr_clock_event() runs only
     when !tmr_wide() (compare match count mode).
- **Direction:** The model is narrower than hardware (a driven
  pin stays idle), so mismatches surface as missing activity
  rather than passing silently — not the dangerous direction,
  but a fidelity gap against explicit text in code shipped by
  Issue #50 stage 1.
- **Validity boundary:** TMR units 0 and 1 (the text covers both;
  §22.5 describes unit 0 and states unit 1 is the same).
- **Promoted-to:** Issue #51; a fix (generate channel-1 low-byte
  match events in wide mode) raised as a PO gate on 2026-11-11.
  The shared-model change also affects the RX62N TMR, whose
  manual carries the same cascade text; D-007 regression rules
  apply.

## F-004: D-016 (TWCR.CCE clear moves counters only) is consistent with the manual's operation example (2026-11-11)

- **Conclusion:** D-016 stands. Figure 20.62 (Example of Counter
  Clearing Operation by MTU3.TGRA Compare Match) shows the
  counters cleared at the TGRA match and the output continuing
  with the same TCDR / MTU3.TGRB / TDDR levels across the clear;
  no buffer or temporary-register transfer is depicted at the
  clear, and no text anywhere assigns the clear a transfer
  effect. Transfers therefore happen only at their natural
  boundary, as D-016 composes the two documented rules.
- **Confidence:** Moderate. Based on the manual's own timing
  figure plus the absence of contrary text; no external source
  (errata, app note, forum report) addresses the interaction.
- **Evidence:**
  1. RX113 manual §20.2.30 (TWCR): CCE "specifies whether to
     clear counters at the MTU3.TGRA compare match" — counters
     only.
  2. RX113 manual §20.3 (o) and Figure 20.62: the counter is
     cleared at the TGRA match; TCDR, TGRB and TDDR levels are
     drawn unchanged across the event and the second cycle
     repeats identically.
  3. TWCR.WRE text ties waveform behaviour at synchronous
     clearing to the Tb interval, again with no transfer effect.
- **Validity boundary:** Complementary PWM mode 1 (the only mode
  CCE may be used in, per the manual's notes).
- **Promoted-to:** Issue #51. No supersede proposed; D-016 stands.

## F-005: Remaining premises — batch verification results (2026-11-11)

Per-item verdicts for the rest of the Issue #51 list. "Unconfirmed"
means no source beyond the manual's own text was found either way;
those items close as documented premises, per the issue protocol.

- **D-019 (POE OSF1 observes all candidate pads): supported.**
  Rests directly on the OCSR1 footnote in the manual (§21.2.2:
  the output-level comparison depends on the pin level
  regardless of the PmnPFS setting). No external source
  contradicts it.
- **Module-stop access rule (Issue #10): supported in substance.**
  Manual §11.4: "the internal states of modules are retained in
  the module stop state" and "basically the registers in the
  module stop state cannot be read or written". The model's
  read-as-0 is the usual concrete form of "cannot be read". The
  manual's race note (a write immediately after setting MSTP may
  still land) is a hardware timing window the model does not
  reproduce; §11.4 itself instructs software not to rely on it,
  so the simplification is on the safe side.
- **D-009 (CAC writes during CFME=1 ignored): unconfirmed.**
  The manual prohibits the writes ("Set the CACR1 register when
  the CACR0.CFME bit is 0", likewise CACR2 and the limit
  registers) but nowhere states the hardware effect of violating
  the prohibition; no errata, app note or forum source found.
  The model's ignore-plus-guest-error choice is the stricter
  direction (a violating program's settings cannot silently take
  effect), so the premise is retained as documented.
- **D-011 (CTSU stabilization wait, CTSUTRMR factory value):
  unconfirmed beyond the sanctioned setting.** The RX113 manual
  fixes CTSUSST to 00010000b with no cycle formula, so the
  modelled 34-cycle wait at 0x10 (from the CTSU1 QE parameter
  guide) and the proportional behaviour for other values only
  affect non-sanctioned settings; the wait has no
  software-observable contract beyond measurement starting
  afterwards. No source states a CTSUTRMR factory trim value.
- **D-014 (ELC duplicate destination arbitration): unconfirmed.**
  The manual is silent on two ELSR entries selecting the same
  destination pin, and no external source addresses it. The
  model's accept-and-apply-in-ELSR-order rule is observable only
  when two sources drive the same pin in the same instant.
- **RTCDV hold-only (Issue #20): not a hardware premise.** The
  register sets sub-clock drive capability, which has no
  emulation-observable effect; storing and reading it back is a
  modelling scope choice, not a behavioural claim. No
  verification applies.

- **Promoted-to:** Issue #51. With F-001 to F-005 the issue's
  verification list is exhausted: D-015 and D-016 stand, D-019
  and the module-stop rule are supported, D-009 / D-011 / D-014
  close unconfirmed, RTCDV is out of scope, and two items await
  PO decisions (D-018 supersede, F-001; cascade TMO1 fix, F-003).
