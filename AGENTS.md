# AGENTS.md — fork-local working rules (NOT part of upstream QEMU)

This file exists only on the `rx113/dev` branch of the ihsinoky/qemu fork.
It governs AI worker (Codex) and PMO activity in this fork. It is not an
upstream QEMU document and must not be included in any patch series ever
prepared for upstream.

## Project

Add Renesas RX113 MCU support to QEMU, based on the existing RX62N
implementation (`hw/rx/rx62n.c`, `hw/rx/rx-gdbsim.c`), for model-driven
development / virtual-product testing. Project overview and decisions:
`docs/rx113/project.md`, `docs/rx113/decisions.md`.

## Roles

- PO (repository owner): final decisions — merges to `rx113/dev`, scope
  changes, any upstream-submission decision.
- PMO: issue decomposition, instruction documents, worker assignment,
  time-boxes, deliverable review, progress records in `docs/rx113/`.
- Worker (Codex): implementation on feature branches, delivered as draft
  PRs against `rx113/dev`. Workers never merge.

## Hard rules

- Never submit anything upstream: no mailing-list posts, no merge/pull
  requests to qemu-project. Upstream contribution is out of scope until
  the PO explicitly decides otherwise (see decisions.md D-001).
- Do not modify the RX62N implementation's behavior. RX113 support is
  added as new code alongside it.
- Follow upstream QEMU coding style (`docs/devel/style.rst`) and run
  `scripts/checkpatch.pl` on changed files before delivery, so the code
  stays reviewable by humans later.
- Commit messages: upstream conventions — subsystem prefix
  (e.g. `hw/rx: ...`), one logical change per commit, imperative mood.
- This VM's /tmp is a 512MB tmpfs. Put build directories and scratch in
  the worktree (e.g. `build/`, `.issueXXX-runtime/`) or under
  `~/workspace`, never /tmp.
- Build only what is needed: `--target-list=rx-softmmu`.

## Baseline

- Base version: tag `v11.1.3` (branch point of `rx113/dev`).
- Subprojects are not vendored in this clone. Fetch them first, from the
  wrap-defined official URLs at their pinned revisions:
  `meson subprojects download dtc berkeley-softfloat-3 berkeley-testfloat-3 keycodemapdb`
  (meson/ninja live in `~/workspace/tools/build-venv`; put it on PATH).
  Note: rx-softmmu requires FDT in QEMU 11 — `-Dfdt=disabled` fails.
- Build: `mkdir build && cd build && ../configure --target-list=rx-softmmu --disable-docs && make -j$(nproc)`
- Smoke: `./build/qemu-system-rx -M help` must list the gdbsim machines.
  Verified green on the unmodified v11.1.3 base (2026-11-05).
