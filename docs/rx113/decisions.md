# Decision log — RX113 QEMU project (fork-local)

## D-001: Develop in a private fork; no upstream submission for now
Date: 2026-11-05. Decided by: PO.
Development happens in the ihsinoky/qemu fork only. Submitting to
upstream QEMU is out of scope until the PO decides otherwise; any
future submission requires the PO's own review of the code first
(QEMU declines AI-generated contributions and requires DCO sign-off).

## D-002: Base version is upstream tag v11.1.3
Date: 2026-11-05. Decided by: PMO under PO's setup mandate.
`rx113/dev` branches from `v11.1.3`, the latest stable tag in the
mirror at setup time. Master's later movement is not tracked until a
rebase is explicitly scheduled.

## D-003: RX62N code stays untouched; RX113 is added alongside
Date: 2026-11-05. Decided by: PMO under PO's setup mandate.
New files follow the existing pattern (`hw/rx/rx62n.c` + header, and
the gdbsim machine file). Machine naming follows the existing
convention (e.g. `gdbsim-r5f51138` for the 512KB/64KB part); the exact
target part number is to be confirmed against the product's BOM.

## D-004: Working regime is PO / PMO / Worker
Date: 2026-11-05. Decided by: PO.
Same regime as the owner's other PMO projects: PO decides, PMO
decomposes and reviews, Codex workers implement via draft PRs and
never merge. Worker rules are in the repository-root `AGENTS.md`.

## D-005: Split Issue #8; defer clock propagation and module gating
Date: 2026-11-06. Decided by: PO; implemented under PMO instructions.
Issue #8 now implements oscillator stabilization delays only, confined to
RX113 SYSTEM. Clock-frequency propagation and module gating move to
a separate issue. The shared CMT/TMR/SCI models expose fixed `input-freq`
properties that reject runtime changes and provide no retained-state gate
interface. Updating those properties alone would not reschedule timers or
refresh cached SCI timing; zero frequency is unsupported. Separate design
and shared-model interface work are therefore required, preserving RX62N
behavior. See `issue-8-clock-tree-report.md` for the blocking evidence.

## D-006: Implement by default; unimplemented only when genuinely heavy
Date: 2026-11-09. Decided by: PO.
This machine is infrastructure for model-driven development: its purpose
is that an AI can run firmware on the simulator and verify behavior.
Casually leaving behavior unimplemented (stubs, stored-only registers)
defeats that purpose, because tests against silent no-ops pass for the
wrong reason. The default is therefore to implement documented hardware
behavior. Leaving something unimplemented is acceptable only when a
faithful implementation is genuinely heavy or complex; in that case the
worker records the reason and the expected cost in the issue report, and
the PMO raises it at the merge gate as an explicit decision, not a
footnote. Existing recorded simplifications are revisited under this
rule (e.g. Issue #20 implements the RTC registers that Issue #18 stored
only).

## D-007: Shared-model changes approved for Issue #10 scope
Date: 2026-11-09. Decided by: PO.
The PO approves modifying the shared CMT/TMR/SCI models for Issue #10
(runtime input-frequency updates and a module-stop gate interface),
which D-005 had deferred pending this decision. The binding condition
is behavioral invariance: unless the new interfaces are used, existing
behavior — RX62N above all — must remain completely unchanged, and the
RX62N regression suite takes priority over the RX113-side wiring.
D-003's "RX62N code stays untouched" is therefore read as "RX62N
behavior stays unchanged" for this scope; RX113 remains alongside.

## D-008: Independent event interfaces for ELC (extends D-007)
Date: 2026-11-10. Decided by: PMO under the D-007 pattern; reported to
the PO, whose merge gate remains the final decision.
The Issue #35 audit showed that the shared CMT/TMR/SCI models and the
RX113 MTU expose only interrupt outputs, which are gated by the
peripheral interrupt enables, while ELC event signals are independent
of those enables (17.1, Figure 17.2). Routing IRQ wires into the ELC
would silently drop events whenever an interrupt is disabled, so a
faithful ELC requires independent event interfaces on the sources.
Under the same conditions as D-007, the following are permitted:
additive, opt-in event outputs on the shared CMT/TMR/SCI models that
pulse on the documented event regardless of interrupt enables and
change nothing when unconnected; event outputs and ELC operation
inputs on the RX113 MTU; and the ELC device plus its wiring on
RX113. IRQ behavior and RX62N behavior must remain completely
unchanged, with the existing regression suites as the gate.

## D-009: CAC active-configuration write policy (emulation convention)
Date: 2026-11-10. Decided by: PMO as an emulation convention, in the
same manner as the Issue #10 stopped-MMIO convention; reported to the
PO, whose merge gate remains the final decision.
The manual requires CACR1, CACR2, CAULVR and CALLVR to be set while
CACR0.CFME=0 (sections 10.2.2, 10.2.3, 10.2.6, 10.2.7) but does not
specify the hardware response to writes with CFME=1. The Issue #39
audit stopped on this gap. Policy: while CFME=1, writes to those four
registers are ignored (values retained) and a guest error is logged.
Rationale: accepting a mid-measurement change to the target/reference
selection, dividers or limits has no defined meaning for the interval
already being counted, so inventing partial-acceptance semantics would
create more undocumented behaviour than ignoring the write; firmware
following the documented sequence is unaffected. Related choices for
Issue #39: the CFME reflection delay (section 10.2.1 note, no duration
or clock domain specified) is not modelled — writes reflect
immediately; the digital filter (section 10.3.2) is documented
behaviour and is implemented as specified under D-006.

## D-010: CTSU input fixture contract (emulation convention)
Date: 2026-11-10. Decided by: PMO as an emulation convention; reported
to the PO, whose merge gate remains the final decision.
The manual defines the CTSU analog chain only qualitatively
(i = f C V, section 35.3.1): no numerical transfer from capacitance
or from the CTSUSO / CTSURICOA adjustment codes to ICO frequency
exists in the supplied sources, and the Issue #41 audit stopped on
this gap. Contract: the fixture boundary is the ICO input. QOM
properties supply ICO frequencies directly — a device-level
reference ICO base frequency (`ref-ico-hz`, default 1 MHz) and a
per-channel sensor ICO frequency (`tsNN-ico-hz`, defaulting to the
reference base, i.e. the deterministic no-touch baseline). The
digital controller — measurement timing from PCLKB, counting,
overflow, the states of Figure 35.11, scan order, interrupts, DTC
and ELC integration — is implemented as documented. Register fields
whose documented effect is purely in the analog domain and has no
numerical definition (CTSUSO sensor offset, CTSURICOA reference
current, CTSUICOG gain) are stored and read back but do not alter
counts; the fixture frequencies are defined as post-adjustment
values. This follows the Issue #20 RTCDV precedent (an
electrical-characteristic-only setting is held, not effected). In
mutual mode, a pair's measurements derive from the receive channel's
sensor ICO frequency for both edge measurements. This contract
tests the digital controller and the software-visible behaviour; it
does not predict physical touch sensitivity or calibration behaviour
against silicon, and the report must say so.

## D-011: CTSU stabilization timing and CTSUTRMR factory value (emulation conventions)
Date: 2026-11-10. Decided by: PMO as emulation conventions under the
D-010 contract; reported to the PO, whose merge gate remains the
final decision.
The RX113 manual requires CTSUSST to be programmed to 0x10 but does
not specify the state-3 stabilization duration that value represents
(Issue #41 resumed audit). Convention: the state-3 wait is 34
base-clock periods at CTSUSST=0x10 — anchored on Renesas's QE
parameter guide (R30AN0428EJ0340, section 3.1), which gives a
34-base-clock-period stabilization interval for a representative
CTSU1 example, CTSU1 being the group the vendor's introduction guide
assigns to both RX113 and RX130 — scaled proportionally for other
CTSUSST values (CTSUSST x 34/16 base-clock periods). The base clock
is the CTSU base clock of sections 35.2.3/35.2.15. This is an
emulation convention, not an RX113 timing specification.
CTSUTRMR (section 35.2.19) resets to a "unique value for each chip"
that the manual does not give numerically. Convention: the model
holds a fixed factory value of 0x00, restored on reset; writes are
accepted only in calibration mode (CTSUERRS.CTSUSPMD=10b), as the
manual specifies, and the value has no effect on counts under D-010.

## D-012: USB0 register inventory follows the manual

The RX113 manual's Table 5.1 and section 27.2 define the USBc register
inventory (62 registers). BUSWAIT, TESTMODE and USBADDR belong to other
Renesas USB variants and do not exist on RX113; the Issue #45 brief's
inclusion of them was a PMO error, corrected at the audit stop.
Registers not in the manual's inventory are not implemented.

## D-013: USB0 SIE transaction fixture contract

BRDY/NRDY/BEMP and the FIFO receive side are driven by bus transaction
outcomes (section 27.3), so the core never invents bus results. The
model takes transaction completion as an explicit input at the SIE
boundary (received packet data and length, transmit completion,
NAK/STALL/error outcomes) — the same boundary the future host and
function bus adapters will drive. FIFO port reads return only data
delivered by completed receive transactions; BEMP is raised only on
completed transmissions, never by BCLR/ACLRM. Tests drive this fixture
boundary. Production host/function adapters remain follow-ups with
their own estimates; enumeration and live bus transfers are not
claimed. Decided by the PMO at the Issue #45 audit stop.

## D-014: ELC duplicate single-port selection on one output pin

Section 17.2.10 lets PEL0 and PEL1 each select any PORTB bit as a
single port, but the manual gives no arbitration when both select the
same output pin and coincident events request conflicting operations,
and no prohibition either. Emulation policy: duplicate selection is
accepted; coincident output operations on the same pin are applied in
ELSR destination order (24 before 25), so the later operation is the
final PODR state and mixed operations compose in that order. This
matches the Issue #35 delivery model (destination-number order) and is
deterministic. It is an emulation convention for an undocumented
configuration, not a claim about hardware behaviour. Decided by the
PMO at the Issue #47 audit stop.

## D-015: Complementary-PWM duty interrupt sources

The manual does not uniquely determine which counter supplies the
complementary-mode duty interrupt requests (TGIB3, TGIA4, TGIB4), nor
whether waveform-suppressed matches request interrupts. Emulation
policy, chosen by the PO at the Issue #49 stage 2b5 audit stop from
the PMO's options: a duty request is raised only when the owning
channel's own counter matches its active compare value — TGIB3 only
on MTU3.TCNT matches, TGIA4/TGIB4 only on MTU4.TCNT matches. Pending
temporary-bank values never request. A match whose waveform turn-on
is suppressed by the duty arbitration still requests, because the
active-value match occurred. Buffered C/D compares selected by
MTU3.BFA/BFB follow the same per-channel rule. The crest (TGIA3) and
trough (TCIV4) paths, TITCR skipping and the reset-synchronized PWM
suppression of section 20.6.15 are unchanged. This is an emulation
convention, not a claim about hardware behaviour.

## D-016: CCE synchronous clear does not transfer the temporary bank

In complementary mode 1, TWCR.CCE clears MTU3.TCNT, MTU4.TCNT and
TCNTS at the TGRA3 match (20.3.8(o)), while the temporary bank
transfers at the end of Tb1 (20.3.8(b)), a boundary the clear can
prevent from ever being reached. The manual does not specify the
interaction. Emulation policy, chosen by the PO at the Issue #49
stage 2b6 audit stop from the PMO's options: compose the two
documented rules literally — CCE performs only the counter clear,
and the temporary bank transfers only at the documented natural
boundary. With CCE active the bank can therefore remain pending
indefinitely; no early transfer and no hidden completion mechanism
is invented. Pending transactions keep their existing snapshot
ordering. This follows the implement-by-default principle (D-006)
and is an emulation convention, not a claim about hardware
behaviour.

## D-017: Oscillator-stop detection follows the IWDT NMI precedent

Section 9.5.2 couples oscillator-stop detection (SYSTEM OSTDCR/
OSTDSR) to two outputs: notification to POE and the non-maskable
OSTDI interrupt. The shared ICU and RX CPU have no NMI input or
delivery path, the same gap recorded for the IWDT underflow (WUNI)
in Issue #27. Scope decision, chosen by the PO at the Issue #49
stage 3 audit stop from the PMO's options: implement POE completely
on the RX113 side, including OSTDCR/OSTDSR, detection, the POE
notification, cutoff and release. The OSTDI request is generated at
the SYSTEM/ICU boundary but not delivered to the CPU, exactly as
with WUNI, and this limitation is stated in the report. Full NMI
delivery (shared ICU/CPU work, estimated 2-4 days with RX62N
regression review) remains a separate cross-cutting theme and is
not folded into Issue #49.

## D-018: OSTSTE write-once is composed with the documented release

Superseded by D-020 (2026-11-11).

Section 21.2.7 marks ICSR3.OSTSTE as modifiable only once after a
reset, while section 21.3.5 documents clearing OSTSTE as a release
path from the oscillator-stop high-impedance state; the manual
states no precedence. Emulation policy, chosen by the PO at the
Issue #49 stage 3b audit stop from the PMO's options: compose both
statements with the smallest deviation. Setting OSTSTE (0 to 1) is
allowed once after a reset. Clearing OSTSTE (1 to 0) is always
accepted as the documented release operation. After a clear,
OSTSTE cannot be set again until a reset. The OSTSTF flag-clear
release path is unaffected. This is an emulation convention for a
manual inconsistency, not a claim about hardware behaviour.

## D-019: POE output comparison observes all alternate pads

Table 21.3 names the POE output-comparison inputs by MTIOC function,
but each function has several alternate pads (Table 19.1) and the
manual gives no selection or reduction rule; the OCSR1 footnote
states only that detection depends on the pin level irrespective of
MPC.PmnPFS. Emulation policy, chosen by the PO at the Issue #49
stage 3c audit stop from the PMO's options: a comparator input is
active when any of the function's alternate pads for the current
package is at the active level, regardless of PFS assignment. The
OSF1 clear qualification (section 21.3.5, outputs inactive) uses
the same observation point. High-impedance output gating remains
limited to PMR/PFS-selected MTU pads, as settled in the stage 3
audit. This is an emulation convention, not a claim about hardware
behaviour, and is a candidate for the Issue #51 hardware-behaviour
cross-check.

## D-020: OSTSTE is strictly write-once (supersedes D-018)

External verification under Issue #51 (findings F-001) found four
vendor sources converging against the D-018 composition: the
manual's own contrast between the write-once configuration
registers and the freely clearable SPOER, migration guide
R01AN3669 listing ICSR3 among the registers that "can be written
to only once after a reset", application note R01AN4140 whose
oscillation-stop recovery clears OSTSTF and never touches OSTSTE,
and the POE family's set-once configuration convention. The PO
accepted the supersede on 2026-11-11. OSTSTE now follows the same
field latch as the other section-21 configuration fields: the
first write that presents the field after a reset determines its
value (a write of 0 consumes the modification as well), and later
writes cannot change it until a reset. The OSTSTF flag clear
remains effective on every write, so the documented recovery path
(OSTDF clear, clock reconfiguration, OSTSTF clear) is unchanged;
only the OSTSTE-clear release path is removed. Whether the latch
is consumed by a write that does not present the field (a low-byte
write to ICSR3 touches only reserved bits) is not documented; the
model follows the ICSR2 precedent of latching per presented field.

## D-021: NMI delivery design (Issue #58)

The OSTDI (D-017) and WUNI (Issue #27) requests were generated but
connected nowhere; this issue delivers them to the CPU as
non-maskable interrupts, closing the last generated-but-undelivered
request in the model. Design points settled by the PMO under the
issue instruction, following the manual (R01UH0448EJ0120
14.2.10–14.2.15) wherever it speaks:

- The shared ICU gains an opt-in bool property `nmi-rx113`
  (default off). The RX62N NMI register block keeps its existing
  decorative behaviour and bit layout; enabling the property
  switches the block to the RX113 layout (NMISR mask 0x3B:
  NMIST/OSTST/IWDTST/LVD1ST/LVD2ST) and real semantics. This
  follows the D-007/D-008 opt-in pattern for shared models.
- NMISR flags are NOT gated by NMIER (14.2.10 note); delivery is
  gated instead: the ICU drives its NMI output while
  `(NMISR & NMIER) != 0`. Source-side enable stays upstream
  (SYSTEM's OSTDCR gate for OSTDI, the IWDT's OFS0 NMI-output
  selection for WUNI).
- The CPU hookup uses the generic `CPU_INTERRUPT_TGT_INT_2` bit
  behind a new `RX_CPU_NMI` GPIO input. Acceptance bypasses
  PSW.I/IPL, takes priority over FIR and maskable interrupts,
  enters through the existing exception-30 path, and sets PSW
  IPL to 15 on entry (2.2.2). No new CPU env state.
- The NMI pin is P35, a fixed-function input with no PFS gate
  whose level the manual reflects in PORT3.PIDR.B5; PORT drives
  the pad level to the ICU at all times.
- NMIFLTE/NMIFLTC are storage-only: the digital filter data path
  (PCLKB sampling, 3-match) is not implemented, because the ICU
  has no clock input to sample with. Recorded as a leftover in
  the issue report, not silently dropped.
- LVD1ST/LVD2ST decode in the register map but have no source
  input: no LVD device exists in this machine. This is the one
  place where a documented flag cannot be raised; it becomes
  connectable if an LVD model is ever built.
- The IWDT's lazy arithmetic skip (Issue #27) previously applied
  only while its WUNI output was unconnected. It now applies
  with the output connected as well, and the MMIO catch-up
  materializes the one-cycle request level the skipped cycles
  would have produced (raised on an exact-boundary target,
  collapsed to a single pulse after jumps). A wake-up timer was
  rejected: qtest clock warping fires timers deadline by
  deadline, so per-boundary wakes iterate billions of times over
  long steps -- the exact cost the skip exists to avoid. The
  accepted deviation is that a repeated request inside a lazy
  stretch surfaces at the next IWDT access instead of at its
  underflow; see the issue report for the exact conditions.

## D-022: LCDC frame timing model (Issue #61)

Issue #14 modelled the LCDC registers and the logical segment state
but left the frame timing, the LCD source clock selection and the
module stop unimplemented. This issue models them, under the
instruction's requirements, on these PMO-settled points:

- The SYSTEM register file gains LCDSCLKCR/LCDSCLKCR2 (9.2.15-16,
  PRC0-protected, appended to the table so the migration layout is
  preserved; SYSTEM VMState version 8). The clock update computes
  the selected source frequency with the same availability rules as
  the CAC sources and delivers it to the LCDC; LCDSCLKSTP is folded
  into the delivered frequency (0 Hz), while MSTPCRD bit 11 is
  delivered separately as the module-stop flag, which gates the
  LCDC's MMIO (reads 0, writes dropped, state held, section 11.4)
  like the other gated peripherals.
- The scan position is a count of half-LCDCL steps, projected
  lazily from the virtual clock on observation (no timer). The
  scan runs while LCDM1.SCOC = 1: section 34.5 starts the common
  select scan at SCOC, and LCDON only gates whether the display
  data is applied. Reconfigurations materialize the position
  under the old settings first.
- The drive phase follows Figures 34.15 to 34.17: waveform A
  inverts every half LCDCL period, waveform B every full period.
  Bias/boost voltages stay analog and unmodelled; the phase and
  the scan slice are exposed through the fork-local QOM surface
  alongside `frame-frequency`.
- The LCDC0 divider tables are per source kind (34.2.3), so the
  delivery carries the source kind next to the frequency. A code
  outside the selected table is a prohibited setting: guest-error
  log and no LCD clock.
- Consequence for existing tests: the LCDC is module-stopped at
  reset like every other gated peripheral, so the qtest and the
  functional guest program now release MSTPCRD bit 11 before use,
  matching the CTSU precedent and real guest software flow.

## D-023: LCDC pad routing abstraction (Issue #63)

Issue #14 modelled the LCDC's logical segment state and Issue #61
its frame timing; this issue routes the COM/SEG outputs to the
PORT pads (Tables 19.3 to 19.16, all LCDC functions at PSEL = 24).
Points settled by the PMO under the issue instruction:

- The PORT pulls the pad state from the LCDC through a QOM link
  on every pad update, and the `levels`/`driving` QOM getters
  refresh before reading. The LCDC stays timer-less (D-022): the
  scan position is projected lazily at observation time.
- Boolean abstraction of the Table 34.11 voltages: the selected
  common is driven at the drive-phase level; a segment with data
  bit 1 (select voltage) is driven opposite to it, a 0 bit
  (deselect) at the same level, so a lit pixel is exactly the
  anti-phase pair. Non-selected commons are left undriven: their
  hardware deselect level is a bias voltage with no boolean
  equivalent, and driving them at either rail would fabricate a
  full-swing differential across unselected pixels.
- Static mode drives COM0 to COM3 with the same selected signal
  (34.8 (1)). With SCOC = 0 all LCDC pins drive the ground level
  (34.2.2); under module stop the LCDC drives nothing.
- The dual-function Port B pads (COM4/SEG11, COM5/SEG12,
  COM6/SEG13, COM7/SEG15) follow the LCDC mode: segment outputs
  except in 8-time-slice mode, where they are commons and the
  four segment numbers are not outputs at all (34.8 (2)).
- VL1 to VL4 and CAPH/CAPL share PSEL = 24 but are analog power
  pins; they stay unmodelled, as before.

## D-024: SCI synchronous transmission design (Issue #65 stage 1)

Points settled by the PMO under the issue instruction; stage 2
(RXD sampling) will extend this record.

- A synchronous frame rides the model's existing transfer timing,
  as the asynchronous waveform does (Issue #50): one synchronous
  bit lasts one model slot (trtime / slots, the Issue #10 baud
  formula included), and the pending-byte slot chains frames with
  no idle gap.
- SCK is generated from the same bit timer in half-bit phases:
  low for the first half of a bit (TXD changes on the falling
  edge), high for the second half (receivers sample on the rising
  edge), idle high outside frames, and TXD holds the last bit
  after a frame (section 28.5).
- The clock source follows section 28.2.6 literally: SCR.CKE
  bit 1 = 0 is the internal clock (SCK is an output); bit 1 = 1
  is an external clock. A synchronous frame on an external clock
  is not serialized in stage 1, because it advances on SCK input
  edges; the input path arrives with RXD sampling in stage 2.
- The PORT SCK map (Tables 19.3 to 19.16, PSEL 10/11/12) uses
  the TXD channel order, so one signal-index table in rx113.c
  serves both.

Stage 2 (RXD sampling) adds:

- Pad input is a second opt-in, `has-input`: the PORT forwards
  the resolved RXD and SCK pad levels (idle high when unselected)
  and the SCI samples them. The chardev receive path is not
  replaced; both sources deliver through the same RDR logic, and
  whichever completes a byte first wins.
- Asynchronous sampling verifies the start bit at its centre
  and samples at bit centres, so a sub-half-bit low pulse is a
  false start, not a frame. FER/PER latch with the byte; an
  overrun drops it, matching the chardev path.
- Reception-only clock generation (section 28.5.1) runs on its
  own half-phase engine so it can stop on overrun and resume
  after a transmission without disturbing the TX engine; while
  transmitting, the receiver samples on the TX engine's rising
  edges (one clock, full duplex).
- The external-clock direction reuses the same `has-input`
  wiring: the forwarded SCK level is the clock, rising edges
  sample RXD, falling edges present TXD bits.

## D-025: MTIOC input capture discipline (Issue #68)

- The PORT forwards the resolved MTIOC pad levels to the MTU on
  a new `mtioc-in` GPIO set that reuses the mtioc signal index;
  the MTU keeps the previous levels and detects edges itself.
- The input handler advances the timers only when a capture is
  actually armed (channel running, normal mode, the general
  register's TIOR field selecting input capture). The MTIOC pins
  are shared with the timer outputs, so an output pad toggling
  back into the model must never disturb the timing state; an
  unconditional advance on every forwarded edge broke the
  complementary-PWM transfer timing during development and is
  therefore ruled out.
- Capture scope follows Table 1.5 and Figure 20.11: MTU0 A-D,
  MTU1/MTU2/MTU3/MTU4 A/B. Capture selection on the output-only
  MTIOC3C/3D and MTIOC4C/4D pins and the MTU0 TIOR 11xx source
  (MTU1.TCNT count clock) stay unimplemented and keep the
  LOG_UNIMP notice. A general register serving as a buffer
  register does not capture (TMDR BFA/BFB notes); a capture into
  TGRA/TGRB writes the register itself, as the ELC operation
  capture already did.
- TICCR cross-capture applies MTU2's own TIOR edge selection and
  mode gates to the MTIOC1A/1B edge; it is not a raw copy of the
  MTU1 capture.
- TSR flag storage is a pre-existing, MTU-wide gap (TSR reads
  return a constant) and is deliberately not fixed here; capture
  raises its interrupt and ELC event through the same path as a
  compare match, like every other MTU event source.

## D-026: ICU digital filter model (Issue #70)

- The filters of section 14.4.7 (NMI pin via NMIFLTE/NMIFLTC,
  IRQ0-7 via IRQFLTE0/IRQFLTC0) sample on ICLK, which SYSTEM
  delivers to the ICU through rx_icu_set_clock(); the manual's
  "PCLK" for the ICU is its own operating clock. Dividers are
  1/8/32/64 per the select fields.
- Sampling phase: the three-match rule is evaluated on a
  sampling cadence anchored at the moment the input diverges
  from the filtered output, not on a free-running phase the
  guest could never observe anyway. A flip is therefore
  delivered between two and three sampling periods after the
  input settles, and any pulse shorter than three periods is
  removed.
- Disabling a filter passes the input through immediately
  (figure 14.7); enabling starts from the current level.
  Changing a select field or ICLK re-anchors an in-flight
  sequence. Sample counts in flight are not migrated; a load
  restarts a pending sequence from the migrated levels.

## D-027: USB0 production host adapter design (Issue #72)

- The adapter lives inside the rx113-usb device: it creates a
  USBBus with one root port and bridges at the SIE boundary.
  Pending host packets are submitted as USBPackets on the
  core's own frame tick, at most one per pipe per frame;
  results return through the same completion entry the D-013
  fixture uses. Guest-visible core behavior is unchanged, and
  with no device attached the fixture behavior is bit-identical.
- Attach/detach drive the core's existing line inputs by
  device speed; while the guest asserts the bus reset, the
  attached device is reset once per assertion.
- Addresses resolve through QEMU's `usb_find_device()`:
  DEVSEL is the device address (DEVADDn is indexed by it).
  Addresses with no reset device leave packets pending.
  Hubs - including the one QEMU chains automatically onto a
  one-free-port bus - are traversed by that lookup; hub
  enumeration itself is guest firmware's work.
- Function mode gets no adapter in this issue and stays
  fixture-driven: there is no gadget-side bus in QEMU to bridge
  to. Premise corrected by the PO on 2026-11-12: the firmware
  this model serves is predominantly device-side, so the
  function adapter is the main remaining theme, split out as
  Issue #74 with its own topology decision (a USB/IP bridge to
  host-PC tools is the leading candidate).
- Adapter state is not migrated; an in-flight packet is
  abandoned on detach, reset or migration (generation
  counter) and left for the guest to retry.

## D-028: USB0 function adapter topology (Issue #74)

- The function adapter is a USB/IP server inside the rx113-usb
  device (the D-027 placement argument applies a fortiori: the
  endpoint-to-pipe map is core-internal state an external bridge
  cannot see). It listens on a QIOChannel socket when the
  `usbip-listen` property is set (e.g. "127.0.0.1:3240"); with
  the property unset the server does not exist and guest-visible
  behavior is bit-identical. The bound port is readable through
  the `usbip-port` property (enables port 0 in tests).
- One session at a time. OP_REQ_IMPORT drives VBUS; when the
  guest raises DPRPU the bridge injects one bus reset and kicks
  SOF interpolation so the frame tick runs - the sequence a real
  host produces on attach. A pull-up drop re-arms the reset.
  TCP disconnect, device reset and migration drop the session
  and all pending URBs (adapter state is not migrated, as in
  D-027).
- URBs translate onto the D-013 SIE boundary: control transfers
  run the setup/data/status stage machine (setup completion,
  per-packet rx/ack completions, stage tokens, completion gated
  on the firmware's CCPL exactly as the fixture scripts it);
  bulk/interrupt URBs resolve their pipe by PIPECFG EPNUM plus
  the DIR bit (set = SIE sends = host IN) and complete through
  rx/ack completions. One URB per endpoint queue is driven at a
  time, on the frame tick, on submit and on SYSCFG writes.
- Recorded divergences: SET_ADDRESS is never delivered (vhci
  completes it locally, as with real USB/IP devices; the model's
  device state therefore skips the Address state on this path).
  The protocol has no bus-reset signal mid-session, so host-side
  port resets are invisible until a re-import. Isochronous URBs
  are refused with -EOPNOTSUPP. The device record reports
  zero VID/PID/descriptor class (the firmware's descriptors are
  only exchanged during enumeration itself).
- Stage 2 (real vhci under Linux/WSL plus the PO's own CDC/HID
  communication tool, self-made) is a separate verification
  step; the protocol-level behavior is pinned by qtest with an
  in-test USB/IP client.
