# RX113 support for QEMU — project overview (fork-local)

Status: setup phase (2026-11-05). This document lives only in the
ihsinoky/qemu fork and describes a private development project.

## Purpose

Provide a QEMU machine that runs firmware built for the Renesas RX113
group, so the firmware can be exercised in a virtual-product test
environment (model-driven development: the machine/peripheral models
are the design artifact; fidelity is added where the firmware actually
touches hardware). The machine exists so that an AI can run firmware on
the simulator and verify behavior; per D-006, documented hardware
behavior is implemented by default, and leaving behavior unimplemented
is justified only when a faithful implementation is genuinely heavy or
complex.

## Starting point

Upstream QEMU already emulates the RX62N group (R5F562N7 / R5F562N8):

- CPU: RXv1 core in `target/rx/` — shared with RX113, no work needed.
- MCU definition: `hw/rx/rx62n.c`, `include/hw/rx/rx62n.h`
- Machine definition: `hw/rx/rx-gdbsim.c` (`gdbsim-r5f562n7`,
  `gdbsim-r5f562n8`)
- Reusable peripheral models: `hw/char/renesas_sci.c`,
  `hw/timer/renesas_cmt.c`, `hw/timer/renesas_tmr.c`,
  `hw/intc/rx_icu.c`

RX113 differs in memory map, interrupt assignments, and peripheral set
(notably a segment LCD controller that upstream QEMU does not model).
The RX113 hardware manual is the source of truth for all constants.

## Scope

In scope:
- New MCU/machine definitions for RX113 alongside the RX62N ones.
- Peripheral models only to the fidelity the target firmware needs;
  unimplemented regions start as logged stubs (a starting point, not an
  end state — see D-006).
- An LCD controller model that exposes segment/display-RAM state for
  assertions (display correctness is checked from state, not images).

Out of scope:
- Cycle-accurate timing.
- Upstream submission (see decisions.md D-001).
- Changing RX62N behavior.

## Working regime

PO / PMO / Worker, the same as the owner's other projects:

- PO: final decisions (merges, scope, upstream question).
- PMO: decomposition into issues, instruction documents, assignment
  and time-boxing of workers, review of draft PRs, records here.
- Workers (Codex): implementation on feature branches, draft PRs
  against `rx113/dev`, never merge.

## Branch strategy

- `master` tracks the upstream mirror untouched.
- `rx113/dev` (branched from tag `v11.1.3`) is the integration branch.
- Feature branches per issue, merged into `rx113/dev` via PRs inside
  this fork after PO approval.

## Contribution policy note

Per PO decision (2026-11-05), work stays inside this fork for now.
QEMU upstream declines contributions that include or derive from
AI-generated content, and requires a Signed-off-by (DCO) certification
from the submitter. If upstreaming is ever pursued, the code to be
submitted must first be personally reviewed (and where necessary
rewritten) by the PO, who alone can make that certification.

### Known fork-local surface to rework before upstreaming

- **LCDC QOM properties** (`hw/rx/rx113-lcdc.c`: `display-ram`,
  `segments`, `pattern`, `driving`, `time-slices`, `blink-phase`).
  Added in Issue #14 so tests can assert the displayed segment state
  without image inspection; the scope was adopted by the PO at the
  PR #15 merge gate (2026-11-09). The concern: once published, QOM
  properties become a de-facto stable API, so a wide observability
  surface raises the maintenance cost of the model and would likely
  need to be reduced or replaced with an upstream-acceptable
  mechanism before submission. A matching note sits at the property
  registration site in the source.
