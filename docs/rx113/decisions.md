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
