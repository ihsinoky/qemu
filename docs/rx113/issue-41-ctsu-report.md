<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #41: RX113 CTSU implementation

Date: 2026-11-10. Branch: `rx113/issue-41-ctsu`.
Base: `e5a9a0674ecdf0fa7df916d4aa1e4bafec5d98fb`.
This replaces the previous stop report. Authority: the original, v2 and
v3 briefs in `.issue41-runtime/`, including their transcriptions of
D-010 and D-011, and R01UH0448EJ0120 Rev.1.20 section 35.
The branch's decision log predates D-010/D-011; it is deliberately not
rewritten from their authoritative brief transcriptions.

## Implementation and audit disposition

The RX113 CTSU placeholder is replaced by `hw/rx/rx113-ctsu.c`.
RX62N and the shared peripheral implementations are unchanged.

| Area | Implemented behavior / source |
| --- | --- |
| Registers | Table 5.1 and sections 35.2.1–19: byte/word widths, reset values, reserved-bit masks, read-only fields, CTSUINIT's selective initialization |
| State machine | Figure 35.11 states 0–5; state 1 selects the channel immediately; state 2 waits for SO1; state 3 waits for stabilization and readout; state 4 measures; state 5 lasts two operating clocks |
| Scan order | Single self scan, ascending self multiscan, ascending mutual receive/transmit pairs with receive channel as outer loop; two edges per mutual pair |
| Handshakes | WR on 1→2, RD on 5→1, FN on 1→0; one WR per mutual pair; CTSUPS changes on 5→1; RC read clears DTSR |
| Counters | Integrate sensor/reference ICO clocks during state 4, floor fractional counts, saturate at 0xffff, latch overflow when 65536 clocks are reached; status read followed by zero write clears each overflow independently |
| Timing | Live SYSTEM PCLKB, CR1 operating-clock division, SO1 base-clock division, SDPRS pulse/ratio formula, SO0 repetition count, D-011 stabilization |
| Start/stop | Software start, repeated-start protection, forced stop/init, external rising-edge start and rearming; section 35.4.2's three-PCLKB restart inhibit for operating-clock /4 and section 35.4.3's one-operating-clock external inhibit |
| Interrupt/DTC | Vectors 60/61 through the existing RX113 DTC frontend; vector 62 directly to the ICU; edge pulses, no overflow interrupt |
| ELC | Existing ELC destination 14 connected to the CTSU start input |
| Module stop | New PRC1-protected SYSTEM MSTPCRD register, reset 0xffffffff, writable mask 0x8c00; MSTPD10 gates CTSU; stopped MMIO reads zero and discards writes, with scan state and fractional progress retained |
| Calibration registers | ERRS writable mask 0x008f, read-only voltage-monitor bit; TRMR reset 0x00 and writes only with SPMD=10b; fixed TS output uses TSOD/IOC through PORT and PSEL=25 |
| Migration/reset | Registers, frequencies, overrides, input level, scan position/edge, phase, fractional counts, clock and gate state migrate; pending deadlines are reconstructed; forced stop and MCU reset cancel pending measurements |

The previous audit's missing timing and trim fixtures are resolved by
D-011. Its state-transition, unread-data hold, mutual WR count, scan-order,
DTC and ELC findings are implemented and tested. No required digital
register, counter, scan, interrupt or trigger feature is deferred under
D-006. SYSTEM migration is extended to version 6 for MSTPCRD; PORT is
extended to version 2 for the fixed-output control. Existing field layouts
are preserved, with defaults for earlier incoming device versions. This
is not a claim of whole-machine migration compatibility with a binary
that has no CTSU device.

## D-010: ICO fixture boundary and limits

`/machine/mcu/ctsu` exposes `ref-ico-hz` (default 1000000) and
`ts00-ico-hz` through `ts11-ico-hz`. An unspecified sensor inherits the
configured reference frequency, including a non-default reference. An
explicit zero is a zero-frequency sensor input. Channel properties can
also be changed with QMP `qom-set`; elapsed measurement time is accounted
for before applying a change. Inputs survive MCU reset and migrate.

Frequencies describe the already adjusted ICO output. SO0.CTSUSO,
SO1.CTSURICOA/CTSUICOG, TRMR and the analog calibration fields retain
and read back their values but do not alter these frequencies or counts.
SO0.CTSUSNUM and SO1.CTSUSDPA remain functional digital timing controls.
Both mutual edges use the receive channel's frequency, exactly as D-010
requires. No random noise is synthesized.

The model reproduces digital control and software-visible measurement
behavior. It does not predict physical touch sensitivity, capacitor
charging, analog power stabilization, current/gain transfer functions,
or calibration quality. The adjusted-ICO fixture assumes normal TSCAP
voltage: CTSUICOMP remains zero; analog offsets cannot synthesize a
voltage fault. TSOD/IOC fixed digital pad levels are implemented, but the
analog sensing drive waveform, spectrum diffusion and external electrode
network are not synthesized at PORT. These analog-path limits are part
of using the D-010 input boundary, not alternative counter algorithms.
Firmware must still follow the manual's power, pin and configuration
sequences. Prohibited configurations/accesses have no promised silicon
behavior; invalid MMIO widths/addresses are diagnosed and ignored.

## D-011: stabilization and factory trim

State 3 waits `CTSUSST * 34/16` base-clock periods, hence 34 periods for
the required SST=0x10. The base clock is PCLKB divided by the CR1 and SO1
dividers from sections 35.2.3/35.2.15. **This is the PMO's emulation
convention, not an RX113 timing specification.** Its rationale is the
representative CTSU1 example in R30AN0428EJ0340 section 3.1, as supplied
by D-011. SST=8 is also tested to verify proportional scaling.

TRMR's factory fixture is exactly 0x00. Normal-mode writes are rejected,
calibration-mode writes are accepted, and MCU reset restores 0x00.
It has no count effect under D-010. Timers have QEMU's nanosecond
resolution; fractional PCLKB cycles and fractional ICO counts are retained
across clock changes and module stop, with transition deadlines rounded
up to the next nanosecond.

## Verification and reproducibility

Build uses only `rx-softmmu`, with docs disabled and the already populated
subprojects. PATH starts with `~/workspace/tools/build-venv/bin`.
All scratch is physically under `.issue41-runtime/.t` or `build/`.
For the full qtest run, `.t` is a short symlink to `.issue41-runtime/.t`,
and TMPDIR uses that alias: the original absolute TMPDIR produced a
116-byte Unix socket path in the generic netdev reconnect test, exceeding
Linux's 108-byte limit. No test or networking code was changed to hide
that environment failure; the alias allows the entire suite to pass.

- Configure and full `make -j$(nproc)` passed; final incremental build
  passed. Logs: `configure.log`, `build.log`, `build-final.log`.
- Full qtests: 22 suites passed, 2 unsupported suites skipped, no failures
  or timeouts. The CTSU suite contains 20 passing tests, including live
  overflow status, D-011 timing/trim, D-010 inputs, analog-field count
  invariance, both scan modes, unread holds, DTC transfers, ELC, module
  stop/clock changes, restart/forced-stop/reset, and five migration cases
  (states 2–5 and stopped state 4).
- `-M help` lists both RX62N gdbsim machines and all four RX113 machines.
- Existing FIT PLL and HOCO ELFs both reach `main` and print the expected
  SCI0 message before and after MCU reset. Inputs were reused from the
  existing `.issue6-worktree/.issue6-runtime/probe{,-hoco}/boot.elf`.
- Both RX62N gdbsim machines run for three seconds and exit cleanly on
  SIGTERM using the existing Issue #6 smoke script.
- `git diff --check` passes. Checkpatch reports zero errors/warnings on
  changed C/header files; the unsigned working-tree patch is checked with
  `--no-signoff` and has zero errors plus the generic new-file MAINTAINERS
  reminder. PMO supplies any commit attribution/sign-off when packaging.

Logs and FIT results are retained in `.issue41-runtime/`; the final
qtest log is `qtest-all.log`, with a standalone `ctsu-qtest.log`.
The final patch is `issue-41-ctsu-v3.patch`, with a SHA-256 sidecar and
`delivery-validation.log`. Validation checks a forward patch dry run
against the exact base files in an isolated directory, applies it,
compares every delivered file byte-for-byte, and checks reverse dry run.
Runtime inputs, vendor manuals, build products, the scratch alias,
`probe.txt`, and AGENTS.md are excluded. No commit, push, PR, merge or
upstream submission was made; committing and PR delivery remain with PMO
because this sandbox cannot write `.git`.

## Follow-ups and estimates

No implementation blocker remains for the agreed digital model. PMO
review and packaging/commit are estimated at half a day. Firmware-level
CTSU/FIT driver exercises beyond the register/DTC/ELC acceptance tests
would add approximately one worker day, depending on the available
application harness. Electrical pad-waveform or sensitivity/calibration
prediction would require a separate analog fixture contract and validated
vendor/board data; an initial waveform-only exploration is approximately
2–4 worker days, while calibrated physical accuracy cannot be estimated
from this manual alone. Those are extensions beyond D-010's adjusted-ICO
boundary, not dependencies of the delivered digital implementation.
