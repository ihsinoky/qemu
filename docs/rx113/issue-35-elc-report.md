<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 35 — ELC implementation and retained interface audit

Current status: **D-008 implementation delivered with explicit residual
scope**; see "Resumed implementation under D-008" below. The following
blocked report is retained verbatim apart from this contextual heading.

## Historical report from the first attempt

Status: **blocked before implementation**, not an acceptance delivery.
Branch: `rx113/issue-35-elc`.
Inspected base: `b67299dd45b5217714eb8d985acf39fea7bf481b`.
No device, wiring or test source has been changed. ELC remains the existing
stub at 0x0008b100. No push, merge, PR or upstream action was performed.

## Reason for stopping

The complete `.issue35-runtime/instructions.md` was read first, followed by
AGENTS.md, D-006, the supplied manual's ELC chapter and module-stop sections,
and the Issue 29, 31 and 33 reports. The instructions explicitly say:

> 接続に不足がある場合は、宛先側のモジュールに手を入れるのではなく、
> ELC 側と配線で吸収すること。吸収できない場合は報告書に上げて止めること。

Existing modeled peripherals lack signals needed for the required links.
In particular, interrupt outputs are not independent event outputs. Merely
splitting those wires would silently drop valid hardware events whenever
the peripheral interrupt is disabled. ELC cannot recover those missing
events through wiring. This is separate from the explicitly permitted
stored-only treatment of links involving absent peripheral models.

Concrete source evidence at the base above:

* `hw/rx/rx113-mtu.c`, `mtu_match()` (lines 134–161): compare outputs exist
  only inside `if (enabled)`, where `enabled` comes from TIER/TIER2.
  `mtu_finish_events()` similarly gates wrap requests with TIER. The
  zero-period case can also hold an IRQ high instead of pulsing per match.
  `mtu_init()` supplies IRQ outputs and four `mtclk` inputs only; there
  are no ELC count-start, restart or capture inputs. The MTU channel state
  and timer helpers are private to that source file.
* `hw/timer/renesas_cmt.c`, `timer_events()` (lines 244–254): CMT compare
  produces an output only when CMCR.CMIE is set.
* `hw/timer/renesas_tmr.c`, `tmr_match()` (lines 138–150): comparison and
  overflow outputs depend on CMIEA, CMIEB and OVIE respectively.
* `hw/char/renesas_sci.c`, `sci_receive()`, `send_byte()` and `txend()`
  (lines 100–153): receive/error, transmit-empty and transmit-end outputs
  depend on RIE, TIE and TEIE respectively.

These conditions contradict using IRQ wires as Table 17.3 event signals:
section 17.1 and Figure 17.2 explicitly make event generation independent
of interrupt enable settings. CMT/TMR/SCI are shared models, whose changes
are forbidden for this issue. D-007 authorized shared changes for Issue 10;
it does not override Issue 35's explicit prohibition.

Forcing interrupt-enable bits would alter guest-visible registers and
ICU/DTC behavior. Reinterpreting MTCLK as capture/start would change its
existing contract. MMIO adapters could perform some individual actions
such as setting a timer start bit, but cannot obtain absent source pulses.
Polling counters/status cannot reproduce every match, wrap and serial
event, including repeated events between observations. A second peripheral
scheduler in ELC would duplicate the source models and their timing,
rather than connect their events. None of these is a faithful wiring fix.

## Manual findings

References are to local R01UH0448EJ0120 Rev.1.20, converted with
`pdftotext -layout` into `.issue35-runtime/rx113.txt`. No network fetch
was made and no scratch/build data was placed in `/tmp`.

| Reference | Requirement confirmed |
| --- | --- |
| 17.1; 17.3.1, pp.324,336 | Event signals are independent of peripheral interrupt enables |
| 17.2.1–2, pp.325–328 | ELCR reset 0x7f, ELCON bit 7; ELSRn reset zero, zero disconnects, documented event IDs only |
| 17.2.3–6; 17.3.3, pp.329–330,338 | Timer options select start/restart, MTU capture, CMT/TMR event counting, or disabled |
| 17.2.7–10; 17.3.5, pp.331–334,338–342 | PORTB group/single-port operations, capture buffer, overwrite/rotation and CPU/DTC write restrictions |
| 17.2.11, p.335 | ELSEGR software event 0x69, WI write protection and WE gating; SEG reads zero |
| 17.2.2; 17.4.1, pp.326,343 | ELSR18 permits only 0x63–0x6a; ELSR19 selects 0x5d; ELSR24/25 cannot select DOC |
| 17.4.3–5, p.343 | DTC destination restriction, operational-clock requirement and ELC module stop |
| 11.2.3; 11.4, pp.215–216,227 | PRC1-protected MSTPCRB bit 9 (MSTPB9); reset stopped; retained internal state, inaccessible registers while stopped |

## Link inventory

No links were wired. The following distinguishes available endpoints from
missing interfaces; availability is not a claim of implemented routing.

| Table 17.3 source IDs | Current endpoint / finding |
| --- | --- |
| 0x08–0x1a (documented MTU1–4 events) | MTU model exists; independent event outputs missing |
| 0x1f (CMT1) | Shared CMT model exists; independent event output missing |
| 0x22–0x24, 0x28–0x2a (TMR0/2) | Shared TMR models exist; independent event outputs missing |
| 0x3a–0x3d (SCI5) | Shared SCI model exists; independent event outputs missing |
| 0x58 (S12AD scan end) | Existing named `scan-end[0]` output is available |
| 0x61 (DTC transfer end) | Existing named `transfer-end[0]` output is available |
| 0x63, 0x65, 0x66 (PORTB edges) | PORT exists; ELC group/single edge logic and integration would be needed |
| 0x69 (software event) | Must be generated by the new ELC itself |
| 0x4e–0x51, 0x59–0x5b, 0x5d, 0x6a | RIIC, comparators, LVD, LPT and DOC have no real event-producing model |

| Destination register | Current endpoint / finding |
| --- | --- |
| ELSR1–4: MTU1–4 | Missing ELC operation inputs; MTCLK is a different interface |
| ELSR7: CMT1; ELSR10/12: TMR0/2 | Shared models have no ELC operation interface |
| ELSR15: S12AD | Existing named `trigger[9]` rising-edge input is available; selectors/TRGE/busy checks remain in ADC |
| ELSR18/19: ICU | DTC front-end inputs 106/80 exist; documented source-selection restrictions must be respected |
| ELSR20/22/24/25: PORTB | PORT exists; ELC port operations/write restrictions would need integration |
| ELSR14: CTSU; ELSR17: DA0 | Destination models absent |

DTC transfer-end -> S12AD and software event -> S12AD are feasible using
the available ADC interface. S12AD/software/PORT events -> ELSR18 could use
the existing DTC/ICU path subject to the documented source restrictions.
Implementing only these does not satisfy the requirement to wire existing
modeled peripherals, including timer/SCI source events. No subset was
implemented after identifying the explicit stop condition.

In a resumed implementation, links with the absent sources/destinations
listed above must retain configuration only and deliver no signal. That
exception must not be applied to the existing MTU/CMT/TMR/SCI models simply
because their interfaces are insufficient. Currently even register storage
is not implemented: the original ELC stub is unchanged.

## Validation and remaining work

This is a source/manual audit, not a functional validation. No RX build,
ELC qtest, FIT probe or regression suite was run. All functional acceptance
conditions, including module stop, migration/reset, disconnection and
end-to-end links, remain outstanding. Existing tests were not edited or
excluded. Changed-file checkpatch and whitespace/patch checks are recorded
in `.issue35-runtime/`; they apply only to this report.

Resumption needs PO/PMO instructions resolving the interface constraints:
independent event notifications for existing sources, with IRQ behavior
unchanged, and a defined way to perform destination timer operations.
Shared CMT/TMR/SCI changes require a scope decision; any MTU destination
additions must resolve the explicit destination-side restriction. Estimated
interface work with tests is 1–2 days, followed by approximately 2–4 days
for ELC routing, timer/PORT integration and acceptance/regression checks.
These are planning estimates, not implemented features or an authorized
scope change.

## Delivery

Only `docs/rx113/issue-35-elc-report.md` is delivered. Runtime/manual files
and AGENTS.md are excluded. `git add` failed creating `.git/index.lock`
with `Read-only file system`; no commit was created. The report remains
in the working tree and the import patch and exact file list are
`.issue35-runtime/issue35.patch` and
`.issue35-runtime/changed-files.txt`. The patch documents the blocker; it
does not implement Issue 35. Changed-file checkpatch: zero errors and zero
warnings. Reverse applicability and forward application in an isolated
scratch fixture were checked successfully.

## Resumed implementation under D-008 (2026-11-10)

This section supersedes the earlier blocked status and delivery description.
The complete audit above is retained as historical evidence. Work resumed
under `.issue35-runtime/instructions-v2.md` and the supplied D-008 copy;
`docs/rx113/decisions.md` is unchanged and excluded from the import patch.
The base and worker branch remain those recorded above. No push, merge,
PR, mailing-list post or upstream action was performed.

The ELC stub is replaced by `hw/rx/rx113-elc.c` and its header. The model
implements byte-wide ELCR, the sixteen documented ELSRn registers, ELOPA–D,
ELSEGR, reset and migration. Reserved read values follow section 17.2.
Undefined source IDs and destination-specific prohibited selections are
logged and rejected; zero disconnects a link. ELSR18 accepts only the
Table 17.3 sources in 0x63–0x6a, ELSR19 only 0x5d, and ELSR24/25 reject
0x6a. No undocumented source is synthesized.

MSTPB9 uses the existing PRC1-protected SYSTEM path. Reset stops ELC;
stopped registers read zero and writes are discarded. Controls survive
module stop. ELCON=0 and module stop cancel pending delivery; neither
replays stopped events on re-enable. Changing a selection discards that
link's previous pending events. Source levels, queued deliveries, controls,
clock and module-stop state migrate. Timer mode/status state migrates with
the timers. The MTU migration version is extended to include its source
subscription mask; this avoids losing repeated zero-period ELC events.
Old machines containing only the ELC stub are not a migration compatibility
contract.

Source callbacks enqueue destination operations. The first queued event
schedules a dispatch after one nominal PCLKB period, rounded up to a
nanosecond. Events arriving before that dispatch are counted, rather than
collapsed into one notification. Events generated during delivery schedule
a later dispatch. Thus even cyclic timer-event links do not recurse through
host callbacks. This is functional scheduling, not a hardware propagation
latency or bus-cycle model; clock changes affect subsequently scheduled
batches, without rescaling an already scheduled dispatch. Cycle-accurate
timing is outside the project scope in `project.md`.

### Independent peripheral interfaces and IRQ invariance

CMT, TMR and SCI now expose named `event` pulse outputs. These are additive
outputs from compare/overflow or serial event generation, outside the
interrupt-enable tests. Existing IRQ conditions, output indices, wires,
ICU arbitration and DTC arbitration are unchanged. RX62N boards do not
connect the new signals or enable ELC operations. Default modes preserve
the legacy clock paths; RX62N emits no additional migration subsection.

CMT exposes two named `elc` operation inputs and an operation-select API;
RX113 connects CMT1 only. TMR exposes one input per pair, an operation-select
API, and RX113-only `has-elc` opt-in for TCSTR at pair offset 0x0c. Start
sets STR1/TCS and ignores an already-started counter, restart clears the
counter without setting its start bit, and event count replaces the
internal clock. CMT event count still requires STR1; TMR event count is
independent of TCCR, and cascaded operation is excluded as section 22.7.2
requires. TCSTR bit 0 is read-only-set by an event and cleared by writing 0.
Unconnected inputs do nothing, including the disabled operation mode.

MTU exposes Table 17.3 event outputs independently of TIER/TIER2 and twelve
operation inputs (start, restart, TGRA capture for each MTU1–4). Operations
require normal TMDR mode; capture also requires the channel running and
TIOR.IOA configured for capture (20.8.2). Capture honors TGRA counter-clear
selection and the existing capture IRQ enable. MTU's held zero-period IRQ
policy remains intact. Its zero-period scheduler optimization is suspended
only for source events subscribed to by an enabled ELC, so repeated ELC
events continue even while the IRQ stays disabled or asserted. Removing
those links restores the existing idle optimization.

### Wired links

All source numbers and destination register numbers below are those of
Tables 17.2–3, not ICU vector numbers. A listed source can fan out to any
implemented destination permitted by those tables and the ELOP settings.

| Source IDs | RX113 connection |
| --- | --- |
| 0x08–0x1a | MTU1–4 named `event[id]`; compare A/B, C/D where present, overflow and documented underflow signals; inherited MTU mode limitation for 0x1a is stated below |
| 0x1f | CMT unit 0 channel 1 independent compare output |
| 0x22–0x24 | TMR unit 0 channel 0 compare A/B and overflow |
| 0x28–0x2a | TMR unit 1 channel 0 (TMR2) compare A/B and overflow |
| 0x3a–0x3d | SCI5 ERI/RXI/TXI/TEI independent events; existing overrun/break/error paths are used |
| 0x58 | S12AD `scan-end[0]`, already independent of ADIE |
| 0x61 | DTC `transfer-end[0]` after a real transfer request |
| 0x69 | ELSEGR software event, using prior WE and the write's WI protection |

| Destination register | Connection and operation |
| --- | --- |
| ELSR1–4 | MTU1–4 start/restart/TGRA capture, ELOPA/B select |
| ELSR7 | CMT1 start/restart/event count, ELOPC select |
| ELSR10,12 | TMR0/2 start/restart/event count, ELOPD select |
| ELSR15 | S12AD `trigger[9]`; ADC keeps its TRGE, trigger-selector, busy and module-stop checks |
| ELSR18 | DTC front-end input 106, then the existing ICU/CPU or DTC path according to DTCE/IER |
| ELSR19 | DTC front-end input 80, which delegates to ICU for this CPU-only LPT vector; LPT source is absent |

The section 17.4.3 rule prohibiting the same DTC transfer destination and
ELC destination remains a firmware configuration requirement. ELC does not
inspect or rewrite guest DTC descriptors to work around it.

### Explicit stored-only links and D-006 merge-gate items

These are acceptance limitations for PMO/PO review, not completed behavior.
Register readback is not evidence that these links work.

| Remaining behavior | Delivered behavior, reason and estimated follow-up size |
| --- | --- |
| PORTB event sources 0x63/0x65/0x66 and destinations ELSR20/22/24/25 | PGR1, PGC1, PDBF1 and PEL0/1 retain masked register values only. No PORTB link is connected. Writes log `LOG_UNIMP`, as do deliveries to an absent destination. Faithful support needs PORT-side input qualification and notification, output/group ownership, restrictions on CPU/DTC writes to selected PODR/PDBF bits, first-capture/overwrite state and rotation over non-contiguous selected bits. The current `port_update()` and `port_write()` have no ELC ownership or capture contract. Implementing only output toggles would falsely suggest that those combined semantics work. Approximately 2–3 working days including PORT integration, reset/migration and edge/group/single-port regression tests; deferred within the authorized three-hour timebox. |
| Sources 0x4e–0x51 (RIIC), 0x59/0x5a (comparators), 0x5b (LVD), 0x5d (LPT), 0x6a (DOC) | Valid ELSRn values are retained; no source peripheral exists to produce a signal. GPIO injection into ELC would test routing only and is not claimed as peripheral support. Each requires its own real device model and tests; indicative sizes are 2–4 days for RIIC, 1–2 days each for comparator/LVD/LPT/DOC, depending on the approved scope. |
| ELSR14 CTSU and ELSR17 DA0 | Valid selections are retained; destination outputs remain unconnected and log on delivery. CTSU needs a new touch-sensing model (several days); DA0 needs a DAC model with enable/output behavior (approximately one day). |
| MTU4 underflow event 0x1a | Its event interface and ELC wire exist, but the inherited MTU model does not implement the complementary/triangle-counting modes that can generate MTU4 down-count underflow. No fake underflow is generated from the MTU5 IRQ. Existing normal compare/overflow and MTU1/2 phase-count underflow are implemented and tested. Completing the missing PWM/counter modes is approximately 2–4 days with mode, buffer, synchronous-operation and IRQ regressions; see the inherited Issue 29 limitations. |

PORT register overwrite/capture/write restrictions are part of the first
residual item; they are not implemented by the stored-only registers.
No change to RX62N code, shared ICU code, PORT code, S12AD code or DTC code
was needed. Independent CMT/TMR/SCI interfaces are the D-008-approved shared
changes.

### Validation of the resumed implementation

All runs use the final binary identified by
`.issue35-runtime/tested-binary.sha256`. Scratch is under `build/`, `.t/`
and `.issue35-runtime/`; `TMPDIR=$PWD/.t` throughout. Existing pinned FIT
ZIPs and the gdbsim U-Boot cache asset were copied from Issue 33. The
unchanged FIT builder verified the ZIP hash and rebuilt PLL and HOCO
probes with the supplied rx-elf toolchain. No external source, manual or
subproject fetch was necessary.

| Check | Result | Evidence under `.issue35-runtime/` |
| --- | --- | --- |
| rx-softmmu configure/build | PASS; final incremental build has no compiler warning | `configure.log`, `build.log`, `build-final.log` |
| New ELC qtest | 44/44 PASS standalone and in suite | `elc-test.log`, `regression.log` |
| RX qtest + quick functional suites | 27 OK, 0 failures, 4 existing skips | `regression.log` |
| Phase 1, four MCU parts, reset | 8 PASS | `regression.log` |
| RX113/RX62N FPU | 2 PASS | `regression.log` |
| LCDC functional/qtest | 1/6 PASS | `regression.log` |
| RTC / PORT / IWDT functional / IWDT qtest | 20 / 6 / 2 / 49 PASS | `regression.log` |
| MTU / S12AD / DTC / clock / SCI / ICU | 21 / 14 / 125 / 9 / 3 / 5 PASS | `regression.log` |
| Genuine FIT PLL and HOCO, boot and reset | Both PASS | `fit-build-*.log`, `fit-*.log`, `verify-*/result.json` |
| SYSTEM with/without FIT ELF | Both PASS | `system.log`, `system-elf.log` |
| RX62N gdbsim U-Boot | PASS | `gdbsim-functional.log` |
| Both gdbsim machine smoke runs | Listed; running for 3 seconds; exit 0 | `machine-help.log`, `gdbsim-smoke.json` |
| Changed-file checkpatch / whitespace | ERROR 0; diff check PASS | `checkpatch-files.log`, `patch-validation.log` |
| Import patch | Reverse check and isolated forward application PASS | `patch-validation.log` |

The existing skips are cdrom, qos, info_usernet and linters. The ELC matrix
uses real peripheral actions, not injected ELC source pulses: eighteen MTU
sources (0x08–0x19), all six TMR sources, CMT1, all four SCI5 event kinds,
S12AD scan end, DTC transfer end and ELSEGR. Timer→ADC tests disable source
interrupts and ADC ADIE, check no corresponding IR request, then disconnect
the link, change analog voltage and demonstrate that no new conversion
occurs. The chain software→TMR event count→CMT event count→ADC verifies
independent compare events at both stages and stops after disconnection.
Other checks cover timer start/restart/capture, TMR TCSTR stop, repeated
zero-period events, ELSEGR WI/WE, ELCON, PRC1/MSTPB9 retention and reset,
ELC→ICU/DTC, and migration with a delivery pending or ELC stopped.

An initial new-test failure used the fast-ADC completion deadline without
setting ADHSC. The test now explicitly selects fast conversion in that
case; no ADC code or assertion was relaxed. The initial build's Meson
regeneration printed a jobserver descriptor diagnostic, then completed all
2450 build steps successfully; subsequent direct Ninja builds completed
without that diagnostic.

Reproduce from this worktree root:

```sh
export TMPDIR="$PWD/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$HOME/workspace/projects/qemu/.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
mkdir -p "$TMPDIR" build
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
ninja -C build -j8
export QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx"
build/tests/qtest/rx113-elc-test
bash .issue35-runtime/regression.sh
```

### Resumed delivery

The final working-tree changes and import patch implement the links above,
with the explicit residual items requiring PMO/PO review. The import patch
is `.issue35-runtime/issue35.patch`; the exact delivered file manifest is
`.issue35-runtime/changed-files.txt`, and its SHA-256 is recorded alongside
it. Neither `docs/rx113/decisions.md`, AGENTS.md, instructions, PDF, build
outputs nor runtime assets are included. The Git index is read-only in
this environment, so no commit was created; changes remain in this
worktree for import. Patch forward application was checked in an isolated
fixture and its resulting files were compared byte-for-byte with the
working-tree files.
