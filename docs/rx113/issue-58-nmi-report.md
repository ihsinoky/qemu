# Issue #58 Report — NMI delivery (OSTDI / WUNI to CPU exception 30)

Cross-cutting theme promised by D-017 and the Issue #27 IWDT work:
the OSTDI and WUNI requests were generated but connected nowhere.
Worker: Muse Wonder (this chat's agent), under the standing audit
and PO-gate regime. Manual: R01UH0448EJ0120 Rev.1.20, sections
2.2.2, 13.5.4, 14.2.10–14.2.15, 19 (P35), 24 (PORT3.PIDR).

## What was built

### ICU NMI block (hw/intc/rx_icu.c, opt-in)

- New bool property `nmi-rx113`, default off. With it off, no new
  code path executes: the NMI register block keeps the RX62N
  decorative behaviour (NMISR reads 0, UNIMP guest-error log) and
  the RX62N layout is untouched. RX113 sets the property before
  realize.
- RX113 layout (sec. 14.2.10–14.2.15), register block at
  ICU+0x580: NMISR b0 NMIST, b1 OSTST, b3 IWDTST, b4 LVD1ST,
  b5 LVD2ST (mask 0x3B — note this differs from the RX62N bit
  positions). NMIER accumulates set-once (`|= val & 0x3B`); a
  write of 0 never clears. NMICLR is write-1-to-clear per flag,
  reads 0. NMICR keeps the shared NMIEN write gate. NMIFLTE
  (+0x590) / NMIFLTC (+0x591) are storage-only (see leftovers).
- Source inputs are named GPIO inputs `nmi-pin`, `nmi-ostdi`,
  `nmi-iwdt`. Per sec. 14.2.10, NMIER does not gate the NMISR
  flags: a source event latches its flag regardless of the enable
  bits; source-side gating stays upstream (SYSTEM gates OSTDI by
  OSTDCR, the IWDT raises WUNI only in its NMI-output OFS0 mode).
  The pin input is edge-detected per NMICR.NMIMD (0 falling,
  1 rising); the other two latch on the rising edge.
- Delivery output is sysbus IRQ index 3, level-driven while
  `(NMISR & NMIER & 0x3B) != 0`, re-evaluated on every flag,
  enable, and clear write. Input levels are kept in state so the
  edge detectors survive migration; VMState version 3
  (minimum 2) carries `nmiflte`/`nmifltc`/`nmi_in_level` under a
  version guard, and post-load re-drives the delivery line.
- LVD1/LVD2 have flags and enables in the register map but no
  input: no LVD device exists in this machine, so the flags can
  never set. Register decode only (D-006 note in the report of
  Issue #51 follow-ups; wiring them becomes possible if an LVD
  model is ever built, cf. the ELC leftover list).

### CPU (target/rx)

- New GPIO input `RX_CPU_NMI` (index 2, via
  `CPU_INTERRUPT_TGT_INT_2`). `rx_cpu_set_irq` maps it straight
  to `cpu_interrupt`/`cpu_reset_interrupt(CPU_INTERRUPT_NMI)` —
  a level, with no vector/IPL interpretation in the setter.
- `rx_cpu_exec_interrupt` accepts a pending NMI before FIR and
  maskable interrupts, unconditionally (PSW.I and IPL do not gate
  it). `rx_cpu_do_interrupt` routes it through the existing
  exception branch with exception index 30, so entry uses the
  fixed vector at 0xFFFFFFF8, saves PSW/PC on the ISP stack, and
  returns with RTE; per sec. 2.2.2 it additionally sets
  PSW IPL to 15 on entry (I=0/U=0/PM=0 come from the existing
  exception path). `rx_cpu_has_work` includes the NMI bit, so a
  WAIT/SLEEP guest wakes for an NMI. No new env fields: the
  pending state lives in the CPU interrupt bit and the ICU.
- RX62N impact: the gdbsim ICU never drives the new output (its
  `nmi-rx113` property stays off) and no board connects CPU
  input 2, so no NMI can pend there; the do_interrupt restructure
  takes the NMI branch only when the NMI bit is set.

### IWDT lazy evaluation vs the new connection (hw/rx/rx113-iwdt.c)

Connecting WUNI exposed a load-bearing assumption of Issue #27:
the IWDT skips repeated automatic errors arithmetically ("lazy"
mode, catch-up on MMIO access) only while its output is
unconnected, so wiring it to the ICU silently disabled the skip
and a one-year qtest clock step iterated billions of per-cycle
timer callbacks (the pre-existing `unconnected` qtest caught
this as a hang). Two design facts shape the fix: qtest clock
warping advances the virtual clock deadline by deadline, so a
"wake at the next boundary" timer still iterates once per
boundary (billions per year); and the manual's WUNI request is a
one-cycle pulse per error (26.3.1.2, the counter reloads after
one cycle), so the observable content of skipped cycles is one
latched flag plus at most one live pulse cycle.

The fix therefore keeps the arithmetic skip with the output
connected and materializes the request level in the MMIO
catch-up: raised when a boundary lands exactly on the catch-up
target cycle, expired once time has advanced past it, and
collapsed into a single pulse when boundaries were left behind
by a jump. The stale UNIMP log claiming NMI delivery is
unavailable is removed from the event path. Known limitation,
accepted for this issue: in a lazy stretch (automatic start,
NMI output, sleep-stop off, an error flag already latched), a
consumer that clears its ICU latch without touching the IWDT
sees a repeated request only at the next IWDT access, where
hardware would deliver it at the next underflow. Every other
configuration (first error, refresh-pending, register start,
sleep-stop on, reset output) keeps per-cycle delivery.

### Machine wiring (hw/rx/rx113.c, hw/rx/rx113-port.c)

- ICU sysbus IRQ 3 → CPU `RX_CPU_NMI`; SYSTEM named output
  `ostdi` → ICU `nmi-ostdi`; IWDT sysbus IRQ 0 (WUNI) → ICU
  `nmi-iwdt`.
- NMI pin: P35 is a fixed-function input (no PFS gate); the
  manual notes its level is reflected in PORT3.PIDR.B5. PORT
  therefore drives its P35 pad level on a new named GPIO output
  `nmi` from `port_update()`, wired to ICU `nmi-pin`. The level
  is re-driven after load by the existing post-load update, so
  PORT VMState is unchanged.

## Decision recorded

D-021 (decisions.md): the opt-in ICU layout switch, the
ungated-flag / gated-delivery split, the TGT_INT_2 CPU hookup,
the fixed P35 wiring, and the storage-only filter registers.

## Tests

New `tests/qtest/rx113-nmi-test.c`, 9 subtests. The six register
tests run under the qtest accelerator (dummy vCPUs): reset
values, NMISR write ignored, filter-register storage, NMIER
set-once, the NMICR NMIEN gate, source flag set/clear semantics
driven through the ICU's named inputs, NMI pin edges in both
NMIMD modes, and the pin path through PORT (P35).

The three delivery tests run a small RX assembly guest (source
in the PMO instruction file for this issue; assembled with the
project's rx-elf toolchain, text embedded in the test) under
`-accel tcg`, because the qtest accelerator never executes guest
code. The image carries the vectors, an OFS0 word, and a config
word at flash offset 0x100; the guest sets ISP/USP, unlocks
PRCR, selects HOCO, builds NMIER from the config word, enables
OSTDCR, and spins incrementing a RAM counter. PSW.I stays 0 for
the whole run, so acceptance itself proves non-maskability. The
handler records NMISR, clears the flags it saw via NMICLR, and
returns with RTE.

- delivery-ost: the SYSTEM `main-clock-failed` input raises
  OSTDI after the model's 1 ms detection; the handler runs with
  snapshot 0x02, clears OSTST itself, and the main loop resumes.
- delivery-ost-gated: with OSTEN=0 the flag latches (NMISR=0x02)
  but nothing is delivered; a host MMIO write of NMIER delivers
  the latched request.
- delivery-iwdt: OFS0 selects automatic start + NMI output; the
  ~8.53 ms underflow delivers with snapshot 0x08.

Full rx qtest regression (19 suites incl. the new one): see the
PR comment for the recorded result.

## Leftovers

- NMIFLTE/NMIFLTC digital filter data path (PCLKB sampling with
  3-match filtering, sec. 14.2.14–14.2.15): registers only. The
  ICU has no clock input today, so this needs a clock wiring
  design first; estimate 1–2 days once the clock path exists.
- LVD1ST/LVD2ST sources: no LVD model in the machine (see
  above). If the ELC-leftover LVD work is ever done, connecting
  these two inputs is a small addition.
- Repeated WUNI requests inside an IWDT lazy stretch are
  materialized at the next IWDT MMIO access rather than
  autonomously at each underflow (see the IWDT section above).
  Removing the delay means giving the lazy skip a wake-up that
  does not iterate per boundary under clock warping, which no
  current timer mechanism provides.
