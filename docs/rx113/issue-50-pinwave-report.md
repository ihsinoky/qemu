# Issue #50 Report — Pin waveforms (cross-cutting): TMR / RTCOUT real pin output

Stage 1 of Issue #50 (TMR waveform + RTCOUT), executed as the Wonder
comparison experiment: the worker was Muse Wonder (this chat's agent),
not Codex. Manual: R01UH0448EJ0120 Rev.1.20, sections 19, 22, 24.

## What was built

### TMR compare-match output (TMO0–TMO3), manual 22.2.6 / 22.3.1

- Shared model `hw/timer/renesas_tmr.c` gains an opt-in pad output,
  gated by the new `has-output` property (default off). With it off,
  no new code path executes and the RX62N behaviour is unchanged
  (D-007 condition). RX113 sets it on both units.
- Per channel, two named GPIO outputs: `tmo` (level) and `tmo-oe`
  (output enable). Compare A applies TCSR.OSA, compare B applies
  TCSR.OSB: 00 no change, 01 low, 10 high, 11 toggle. Both actions
  converge in `tmr_match()` (clock-managed and ELC event-count paths)
  and in the legacy `issue_event()` path.
- Output enable follows the manual: OSA and OSB both zero negates the
  enable (Hi-Z request to the port); enabling from all-zero drives the
  pin low until the first compare match (22.3.1). A module stop
  (MSTP) drops the enable; the level state is retained and re-driven
  on resume. Reset clears the level.
- Migration: the level array travels in a new `rx-tmr/output`
  VMState subsection, `needed` only when `has-output` is set, so the
  RX62N stream is unchanged.

### PORT mux (hw/rx/rx113-port.c)

- Tables 19.3–19.16: TMO0–3 all use PSEL=5 — TMO0: P22/PB3,
  TMO1: P17/P26, TMO2: P16/PC7, TMO3: P13/P32/P55. RTCOUT uses
  PSEL=7 on P16/P32/PA1/PB0.
- PORT gains GPIO inputs `tmo` (4 levels + 4 enables) and `rtcout`
  (level + enable), resolved in `port_update()` alongside MTIOC, and
  both functions count as supported in the PMR "unavailable" check.
  POE cut-off does not apply (it targets MTU outputs only, sec. 21).
- VMState version 5 adds the `tmo`/`tmo_oe` masks and the RTCOUT
  level/enable bits (`VMSTATE_*_V(..., 5)`).

### RTCOUT pin output (hw/rx/rx113-rtc.c)

- The level was previously observable only through the `rtcout` QOM
  property. The RTC now also pushes `rtcout` (level) and `rtcout-oe`
  (RTCOE) GPIO outputs to the PORT mux. The level remains a pure
  function of (RCR1.RTCOS, RCR2.RTCOE, sub-counter), so no VMState
  change is needed: post-load recomputes it via `rtc_update_timer()`.
- `rtc_update_timer()` takes the next RTCOUT edge (sub-counter bit 7
  for 1 Hz, bit 1 for 64 Hz) as a timer deadline candidate, so the
  pad tracks edges even when no PRD/alarm deadline exists.

### Tests

New `tests/qtest/rx113-tmr-test.c`, 6 subtests: Figure 22.3 pulse
output on TMR0/P22 (initial low, A high at tick 100, B low at 140,
period repeats), toggle + no-change selects on TMR1/P26, Hi-Z on
all-zero selects and module-stop drop/resume, second unit (TMR3/P55),
RTCOUT on P32 (1 Hz) and PB0 (64 Hz) including RTCOE-off Hi-Z, and a
migration subtest (driven-high pad and waveform continue on the
destination). All pass; full rx qtest regression is green
(17 suites, 460 subtests, 0 failures), checkpatch 0 errors /
0 warnings on all touched files.

## Model readings and limits (not claims about hardware)

- Cascade (16-bit) mode: the manual does not state which TMO pin a
  cascaded unit drives. The model applies each channel's own TCSR
  actions to its own pin from the events that channel's match logic
  raises (channel 0 from the wide compares, channel 1 from the
  cascaded low-byte events). Unverified; a candidate for the
  Issue #51-style external cross-check.
- When compare A and B coincide on the same tick, B is applied after
  A (the order the event bits are processed in `tmr_match()`).
- External clock / cascade-input modes keep their existing limits
  (Issue #10 report); output actions follow whatever compare events
  the counter logic raises, including ELC event-count mode.
- TMR input pins (TMRI/TMCI) remain unimplemented, as before.

## Experiment record (Wonder comparison)

- Worker: Muse Wonder, one continuous run, 2026-11-11 14:35–14:57 JST
  (investigation, implementation, tests, regression). No Codex or
  Copilot credits consumed.
- Runs to completion: 1. PMO send-backs: 0 (worker and PMO are the
  same agent in this experiment; independence is provided by the
  main side re-running the qtests on the submitted branch).
- Audit stops: 0. No undocumented premise needed a new decision; the
  one judgement call (cascade pin behaviour, above) is recorded here
  as an unverified reading instead of being silently baked in.
- Defects found and fixed during the run, all before the first green
  build of the full suite:
  1. RTC: new RTCOUT helpers initially placed after their first user
     (`rtc_update_timer`); moved before it (would not have compiled).
  2. qtest setup: SYSTEM registers are PRCR-protected; the test
     omitted the PRCR unlock, so MSTPCRA writes were dropped and the
     TMR stayed stopped (all register writes dropped with it).
  3. qtest setup: SCKCR/SCKCR3 are likewise PRCR-protected; writing
     them before the unlock left PCLKB at its slow boot default, so
     the waveform timing asserts missed by orders of magnitude.
  No device-code defect was found after the first compile; defects 2
  and 3 were in the new test's setup, found by probing the running
  model (register readbacks) rather than by re-reading code.
- qtests added: 6 (one new suite). Codex baseline for comparison is
  in the experiment plan (Issue #49: 8+ runs, 92 MTU/POE subtests,
  weekly quota 19%→3% plus a reset credit).

## Remaining (later)

- Out of scope, unchanged: LCDC pin waveforms, NMI delivery, RXD
  input sampling, the shared SCI model's known quirks.

## Stage 2: SCI TXD waveform on the shared model

Worker: Muse (Wonder) under the experiment, on
`rx113/issue-50-stage2-scitxd` from `rx113/dev` tip 977a047ba7.
Implemented 2026-11-11 in one run.

### Design: the waveform rides the model's transfer timing

- The shared SCI model (hw/char/renesas_sci.c) gains an opt-in
  `has-output` bool property (default off). With it off, no new code
  path executes and no VMState subsection is emitted, so RX62N
  behaviour is unchanged (the D-007 condition, same pattern as the
  TMR output in Stage 1). RX113 sets it for all eight channels.
- A bit engine serializes each transmitted byte onto new GPIO outputs
  `txd` (level) and `txd-oe` (driven while SCR.TE=1 and the module is
  running). A frame starts exactly when the model's existing
  send_byte() runs: on a TDR write while idle, or for a queued byte
  when the previous transfer deadline expires. The backend delivery
  instant, TDRE/TEND timing and the known quirks (the update_trtime
  baud formula, immediate delivery) are untouched.
- Bit time: update_trtime()'s slot count (data + parity + STOP + 1)
  does not include the start bit, so one slot is exactly one bit time
  of the model's baud generator, while a serialized frame has one
  more bit than the slot count. Consequence: TEND asserts at trtime,
  one bit before the pad frame ends. A byte handed over while the
  previous frame is still on the pad waits in a pending slot and
  follows with no idle gap (the TSR-behind-TDR relationship), so
  back-to-back frames show a full stop bit. This reading is forced by
  the model's own arithmetic and is recorded here rather than
  silently harmonized.
- Frame content (Section 28): start bit (low), data bits LSB first
  (MSB first with SCMR.SDIR), parity when SMR.PE (even/odd per SMR.PM,
  computed over the data bits), the MPBT bit in multiprocessor mode,
  1-2 stop bits (SMR.STOP). SCMR.SINV inverts the data bits only,
  per the manual's SCMR note. 7-bit data with SMR.CHR.
- Bit deadlines chain from the previous deadline rather than the
  callback time, so a stepped (qtest) clock cannot stretch the frame.
- TE falling mid-frame aborts the frame and releases the pad; module
  stop releases the pad and freezes the engine via the existing
  clock bookkeeping (tx_bit_remaining in input-clock cycles, the
  same mechanism as the frame timer), resuming on restart.
- Clock-synchronous mode (SMR.CM) has no start/stop framing: the pad
  is driven (TE) at idle level but the data is not serialized.
  Recorded as a limitation of this stage.
- VMState: new `renesas-sci/output` subsection (needed only when
  has-output) with the shift byte, bit position, frame/slot counts,
  the pending byte, the current bit's remaining cycles, the current
  deadline and the bit timer. The level itself is re-pushed on
  post-load.

### PORT and board

- rx113-port learns the TXD function map from Tables 19.3-19.16
  (20 pad assignments over PSEL 10/11/12/23 for TXD0/1/2/5/6/8/9/12),
  a `txd` GPIO input bank (8 levels + 8 output-enables) resolved
  after TMO in the peripheral mux, and the PMR "unavailable" log
  exemption. VMState version 6 adds the two TXD masks.
- rx113.c sets `has-output` on every SCI and wires each channel to
  its PORT signal (unit order SCI0, 1, 2, 5, 6, 12, 8, 9 maps to
  signals TXD0, 1, 2, 5, 6, 12, 8, 9).

### Verification

- New suite tests/qtest/rx113-sci-txd-test.c, 5 subtests, all
  passing: a bit-by-bit 8N1 frame of 0x55 on SCI1/P16 sampled at
  mid-bit; TE and module-stop release on both TXD0 pads (P10,
  PB7); 7-bit + even parity and MSB-first order on SCI12/P14; a
  queued byte starting its frame at the previous frame's end with
  no idle gap; migration mid-frame (bit position, driven level and
  completion all continue on the destination).
- Full RX regression (author side): 18 suites, 465 subtests,
  0 failures, including rx113-sci-test (backend order, MSTP and
  clock behaviour of the shared model) and the Stage 1 suites.
  `-M help` lists all machines; checkpatch reports 0 errors and
  0 warnings on every touched file.
- Independent re-run requested from the main side for: the new
  rx113-sci-txd suite, rx113-sci-test, rx113-port-test,
  rx113-tmr-test and rx113-clock-test (RX62N invariance evidence:
  the gdbsim machine is covered by the unchanged-code-path argument
  plus the full regression; no RX62N-specific SCI test exists).

### Experiment record (Stage 2)

- Wall time 2026-11-11 16:27-18:05 JST (including two environment
  restarts). Runs to completion: 1. PMO send-backs: 0. Audit stops:
  0; no new decision was needed - the one judgement call (the
  start-bit slot reading above) follows the model's arithmetic and
  is documented rather than decided.
- Defects found and fixed during the run:
  1. First build: the PORT TXD input handler was inserted before
     port_update()'s definition (implicit declaration), and
     qdev_prop_set_bool does not exist in this tree (Stage 1 used
     qdev_prop_set_bit). Both fixed before any test ran.
  2. First waveform sampling was compressed by 10%: the bit
     duration divided trtime by the serialized frame's bit count,
     but the model's slot count omits the start bit. Found by
     dumping the pad at 1 us resolution and matching the transition
     times against both candidate periods. Fixed by deriving the
     bit time from the model's slot count and adding the pending
     slot for queued bytes.
  3. The migration test's wait helper used a dotted QMP key
     (qdict_get_str does not resolve paths), segfaulting the test
     driver. Replaced with the Stage 1 helper's nested lookup.
  4. One verification loop was accidentally run against the Stage 1
     worktree's binaries (a copied path) and reported Stage 1
     numbers; the full regression was re-run in this worktree.
- qtests added: 5 (one new suite). Codex quota consumed: none.
