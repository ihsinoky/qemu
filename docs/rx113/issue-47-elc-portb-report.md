# Issue #47: RX113 ELC PORTB linkage implementation

Date: 2026-11-11. Branch: `rx113/issue-47-elc-portb`.
Implementation resumed after the previous worker runtime was interrupted
at approximately 05:43 JST. The continuation first inspected the diff,
the previous audit report, both runtime briefs, and the supplied manual.
The partial RX113 PORT interface, headers and machine wiring were retained
and completed; the ELC still had placeholder port-register handling.
The earlier audit-only patch is superseded by this implementation delivery.

Authoritative requirements: `.issue47-runtime/issue-47-elc-portb.md` and
`issue-47-elc-portb-v2.md`, including PMO decision D-014. Hardware reference:
R01UH0448EJ0120 Rev.1.20, sections 17.2.7–10, 17.3.5 and 17.4.

## Design and behavior

ELC owns the PGR1, PGC1, PDBF1 and PEL0/1 configuration, edge selection,
unread-capture protection and rotation phase. PORT provides RX113-only
access to qualified PORTB GPIO membership, direction, physical levels and
PODR, plus masked event writes and a CPU/DTC write-permission callback.
The MCU connects the two devices directly. Shared devices and RX62N code
are unchanged.

PORT level changes generate independent group (0x63) and single-port
(0x65/0x66) sources, with rising, falling and both-edge selection. Output
pins do not generate input events. Sources enter the existing ELC queue,
which preserves Issue #35's nominal one-PCLKB delivery convention.
Destinations 20, 22, 24 and 25 perform group output, group capture and the
two single-port output operations. Output-group membership takes priority
over single-port output; overlapping input functions operate independently.
Package availability, PMR and the 64-pin PSRA pad selection qualify linkage.

Group output supports low, high, toggle, buffer transfer and compressed
MSB-to-LSB rotation within the selected output members, preserving all
other PODR bits. The first rotation event transfers PDBF1; later events
rotate PODR. Rewriting ELSR20 starts a fresh transfer sequence. As required
by section 17.4.2, firmware must rewrite ELSR20 after changing rotation
buffer data and must space rotation sources by at least one PCLKB cycle.
No silicon behavior is claimed for firmware violating these usage notes;
the existing event-count queue is retained.

Capture replaces only input-member bits of PDBF1, preserving other bits.
An unread capture blocks further capture unless PGCOVE enables overwrite.
CPU and DTC reads release the protection. ELCON gates CPU/DTC write
restrictions: output-group PODR bits are protected regardless of ELSR20;
single-output PODR bits require an active corresponding link; input-group
PDBF bits require an active ELSR22 link, with the documented DOC exception.
The existing prohibition of DOC on ELSR24/25 remains in force. PEL PSP
encodings other than disabled or PORTB are rejected as prohibited settings.
Reserved register bits and reset values follow the manual.

D-014 is an explicit **emulation convention**, not a statement about
silicon: duplicate PEL0/PEL1 output selections are accepted silently.
Coincident operations compose in destination order, ELSR24 then ELSR25;
the latter operation determines the final PODR value, including mixed
toggle/set operations. This resolves the ambiguity documented by the
previous audit. No further unresolved design decision was encountered.

MSTPB9 retains configuration, unread protection and rotation phase, returns
zero for stopped ELC MMIO, ignores stopped writes and cancels queued
delivery. Reset clears capture and rotation state. VMState version 2 adds
both state flags and accepts version 1 streams with their default false
values. PORT reset/post-load restores GPIO without notifying ELC of input
edges, preventing migration restoration from fabricating port events.

## Validation

All required checks pass. Logs are under `.issue47-runtime/`.

| Check | Result / evidence |
| --- | --- |
| RX-only build | PASS; `build.log`, `build-final.log` (final Ninja invocation up to date) |
| ELC qtest | PASS, 55 cases including 11 new port cases; `elc-qtest.log` |
| All configured RX qtests and quick functional suites | PASS, 32 programs, zero failures, four expected skips; `regression-final.log` |
| FIT PLL and HOCO startup, including reset | PASS; `fit-pll.log`, `fit-hoco.log` and respective `result.json` files |
| SYSTEM timing/protection, with and without FIT ELF | PASS; `system.log`, `system-elf.log` |
| Machine list | PASS, both gdbsim machines listed; `machine-help.log` |
| Both gdbsim machines, running for three seconds | PASS; `gdbsim-smoke.log` |
| RX62N gdbsim U-Boot | PASS; `gdbsim-functional-verified.log`; existing Linux/sash flaky exclusion retained |
| Checkpatch, all six changed C/header files | PASS, zero errors/warnings; `checkpatch-files.log` |
| Checkpatch, complete patch | PASS, zero errors; two documentation metadata warnings (SPDX and MAINTAINERS for the new report); `delivery-validation.log` (`--no-signoff` for an uncommitted patch) |
| Whitespace and patch applicability | PASS; `delivery-validation.log` |

New qtests cover all edge encodings, the nominal PCLKB delivery boundary,
D-014 ordering/composition, output priority, all group modes, non-member
preservation, capture protection/PGCOVE, CPU/DTC access rules, independent
overlapping inputs, package selection, CMT-to-port linkage, reset, pending
and stopped migration, retained state and suppression of restoration edges.
The migration cases verify unread protection and rotation phase together;
the DTC case performs actual transfers through the normal address space.

Builds used Meson 1.12.1/Ninja from `~/workspace/tools/build-venv` and the
already populated subprojects; nothing was re-fetched. Scratch remained
inside the supplied `.issue47-runtime/.t`. FIT used the existing Issue #6
PLL/HOCO probe ELFs; the pinned U-Boot cache asset was copied from Issue #45.
The regression commands were:

```sh
meson test -C build --no-rebuild --suite qtest --suite func-quick \
  --num-processes 4 --print-errorlogs \
  --wrapper "$PWD/.issue47-runtime/regression-wrapper.py"
meson test -C build --no-rebuild --setup thorough --print-errorlogs \
  --wrapper "$PWD/.issue47-runtime/regression-wrapper.py" func-rx-gdbsim
```

The first broader run exposed only test-environment path problems: generic
monitor hotplug and netdev tests exceeded UNIX socket path limits. A
runtime-only wrapper shortens functional socket-directory prefixes and
uses a relative spelling of the same TMPDIR for netdev. It also redirects
QEMUMachine's hard-coded `/var/tmp` default to the supplied worktree scratch
for generic helpers and gdbsim's machine probe. No repository test behavior
or product code was changed to resolve these environment problems. Initial
logs are retained; the final full run passes in a single invocation.

## Delivery and remaining scope

No commit, merge, PR or upstream submission is made. The sandbox makes
Git metadata read-only; PMO will commit the working-tree delivery. Runtime
briefs, the manual, scratch, build products, AGENTS.md and the pre-existing
untracked `probe.txt` are excluded from the implementation patch.
The complete delivery is
`.issue47-runtime/issue-47-elc-portb-complete.patch`, with a companion
`.sha256` file. `make-delivery.py` records the exact changed-file allowlist,
checks reverse application against this tree, runs forward dry-run checks
against files reconstructed from HEAD, applies the patch in that isolated
scratch tree using Git, compares all seven results byte-for-byte, and
checks reverse dry-run application and complete-patch style. GNU patch's
actual application cannot preserve ownership in this sandbox, so Git is
used for the real isolated application; GNU patch dry-run checks pass in
both directions. `delivery-validation.log` records the successful checks.

No functional deferrals are introduced. Hardware analog pad timing and
behavior under prohibited firmware sequences are outside this digital
model, consistent with the existing PORT and Issue #35 event models.
