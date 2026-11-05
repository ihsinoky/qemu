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
