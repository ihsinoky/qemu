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
