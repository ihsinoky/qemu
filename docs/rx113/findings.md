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
