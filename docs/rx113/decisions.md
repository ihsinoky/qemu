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
