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

## Remaining (Stage 2 and later)

- Stage 2 (separate go): SCI TXD waveform on the shared SCI model
  (opt-in output under the D-007 condition), including MPC PSEL
  entries for TXD pads and PORT mux support.
- Out of scope, unchanged: LCDC pin waveforms, NMI delivery, RXD
  input sampling, the shared SCI model's known quirks.
