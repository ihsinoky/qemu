<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 49: RX113 MTU advanced output — stage 2b6 TDER and TWCR audit

## Current delivery (2026-11-11)

**Stage 2b and Issue #49 remain incomplete.** The first item of
`.issue49-runtime/issue-49-stage2b6.md` is implemented: TDER
read-before-clear and no-dead-time waveforms. Work stops during the
second item's TWCR audit at the CCE/buffer-transfer evidence gap below,
in accordance with the instruction to stop instead of guessing.
TWCR/WRE/CCE and complementary synchronous clearing are not implemented.
The third item, TGCR gating in reset-synchronized PWM, was not started
because the brief requires ordered execution. Stage 3 (POE) was not
started. Earlier delivery sections are historical.

Base: `37c2523fd0b90e8ea7b312d35dc30478ddb6aed4`.
Branch: `rx113/issue-49-mtu-advanced`.
Deliverables: working tree and
`.issue49-runtime/issue49-stage2b6-tder-audit.patch`.
No commit, merge, push, PR or upstream submission was performed.

### TDER implementation and evidence

Authority: supplied R01UH0448EJ0120 Rev.1.20, printed manual pages.

* 20.2.29, p.445; 20.3.8(f), p.486: TDER resets to 1. Clearing
  requires a preceding read of 1; writing 1 is unconditional. Writes
  consume the read qualification. Reserved bits read as zero. TDER is
  absent from the protected-register list in 20.2.16, p.432, so TRWER
  does not protect it. Firmware must change it while counters are stopped,
  as the manual requires; changing it during counting is not validated.
* Table 20.56 and Figure 20.41, pp.486-487: firmware sets TDDR=1,
  TGRA3/TGRC3=TCDR+1. The coupled carrier and TCNTS retain their
  existing count offset and transfer chronology. MTU4 compare matches
  switch the positive waveform with no pad dead time; coincident turn-on
  and turn-off retain its latch, as in the existing waveform arbitration.
* 20.2.18-19, pp.434-437; 20.6.20, pp.543-544: negative output is
  the inverse of positive output with no dead time, including initial
  output. Only OLSP/OLSiP selects polarity. Existing TGCR gating remains
  downstream of the waveform and TOER still controls pad ownership.
* Migration version 10 saves TDER and its read qualification. Older
  streams default to dead time enabled, matching their implemented
  behavior. Existing same-build migration tests pass with TDER=1;
  migration with TDER=0 or a qualified TDER read has not been tested.

Only RX113 MTU implementation, its qtest and this report change. RX62N,
PORT and shared SCI/CMT/TMR sources are untouched. The existing partial
complementary diagnostic remains appropriate. One new qtest covers
modes 13/14/15 over two carrier cycles, three duties, global/per-phase
polarity, ignored N polarity, carrier count checks one nanosecond before
edges, read-before-clear qualification, reserved bits, stop and reset.
Extreme duties and pending-buffer changes with TDER=0 are not newly
validated by this delivery.

### TWCR audit: CCE versus the crest transfer boundary

The manual provides two individually clear rules whose interaction is
not specified:

1. 20.3.8(b), p.484, Figure 20.40, p.485: the temporary bank transfers
   to active registers **at the end of Tb1**, when upward-counting TCNTS
   reaches TGRA3. The MTU3.TGRA crest match occurs earlier in Tb1.
   Writes committed during Tb wait for its end before entering the
   temporary bank. The current implementation follows these rules.
2. 20.2.30, p.446; 20.3.8(o), p.502, Figure 20.62: in complementary
   mode 1, CCE clears MTU3.TCNT, MTU4.TCNT and TCNTS at the TGRA3
   match. TSYR synchronization, zero duty and PSYE are prohibited for
   this operation. Neither this text nor the figure specifies the
   disposition of the interrupted Tb1 transfer or its temporary bank.

Distinguishing setup using the existing test carrier: TDDR=2, TCDR=12,
TGRA3=14, mode 13 with BFA/BFB enabled, PSYE=0, TSYR=0, old duty 4.
Commit a new duty 8 into the temporary bank during Ta. In the ordinary
carrier, the first crest match is count edge 13 and the end-Tb1 transfer
is edge 15. With CCE, the documented reset at edge 13 prevents TCNTS
from naturally reaching that later boundary.

A literal restart with no special transfer leaves active duty 4; repeated
CCE clears can prevent the normal crest transfer indefinitely. Treating
CCE as an early end-Tb1 transfer changes active duty to 8 at edge 13,
but adds a transfer condition and timing not stated by the manual.
Continuing a separate hidden Tb1 completion after clearing TCNTS would
also require an undocumented mechanism. The alternatives affect active
TGR readback and the next waveform. A commit write during Tb1 adds the
question of whether the old temporary bank is consumed before the newly
pending transaction. Figure 20.62 shows neither bank nor an update.
Section 20.7's restart procedures do not resolve this CCE case.

**Next step requires PO/PMO resolution:** provide authoritative vendor
clarification or an RX113 trace for the above setup, including active
TGR readback around edges 13/15 and a commit during Tb1. Otherwise,
explicitly choose and record a CCE transfer convention and its ordering
relative to pending buffer snapshots. No TWCR implementation or tests
encoding one of these choices remain in this patch. Then complete TWCR
read-before-set, WRE's noninitial-trough-only retention, CCE and
synchronous restart tests, followed by reset-synchronized PWM TGCR.

The supplied text does establish that WRE retains output only for a
later trough Tb2 clear, excludes the initial startup Tb2, and otherwise
restores TOCR initial output (20.3.8(m)-(n), pp.497-501). Counter stop
and restart uses the initial counter state (20.6.13, pp.538-539;
20.7, Figures 20.149-150, pp.569-570). These findings do not resolve
the separate CCE bank-transfer question.

### Duty-request decision remains PENDING

The historical Duty-request audit section below remains unchanged and
PENDING with PMO/PO. This run neither wires nor suppresses speculative
duty/C/D interrupt sources. Existing TGIA3/TCIV4 ICU paths, skipping and
raw ELC sources are unchanged. This TWCR audit is a separate evidence
gap; stage 2b cannot yet be described as waiting only for the duty
interrupt decision.

### Validation

Evidence uses `stage2b6-*` names under `.issue49-runtime/`. The existing
build and populated subprojects were reused without downloads. All scratch
uses `.issue49-runtime/.t` or its existing `.s49` socket-path alias.

| Check | Result and runtime evidence |
| --- | --- |
| RX-only rebuild | PASS: `stage2b6-build-final.log` |
| Full MTU qtest | 67/67 PASS, 146.44 seconds: `stage2b6-regression.log` |
| Configured RX qtests and quick functional tests | 32 suites OK, four existing skips, zero failures: `stage2b6-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2b6-gdbsim-functional.log` |
| Cached FIT PLL/HOCO before and after reset | Both PASS: `stage2b6-fit-pll.log`, `stage2b6-fit-hoco.log` |
| SYSTEM protection/timing without/with FIT ELF | Both PASS: `stage2b6-system.log`, `stage2b6-system-elf.log` |
| Both gdbsim machines listed/run three seconds | Both PASS: `stage2b6-machine-help.log`, `stage2b6-gdbsim-smoke.log` |
| Final focused TDER waveform test | PASS: `stage2b6-tder-final.log` |
| Changed-file and raw-diff checkpatch | Zero errors and warnings: `stage2b6-checkpatch-files.log`, `stage2b6-checkpatch-report.log`, `stage2b6-checkpatch-patch.log` |
| Forward/reverse patch dry runs and byte reconstruction | PASS: `stage2b6-patch-validation.log` |

All original 66 MTU cases remain. After full regression the only device
source change was wrapping a long validation condition for style; no
operation changed. The final rebuild and focused TDER case check the
resulting tree. `stage2b6-regression-binary.sha256` identifies the binary
used for full regression; `stage2b6-tested-binary.sha256` identifies the
final rebuilt binary. Early focused runs exposed two test-oracle setup
errors (the descending match edge and enabling PSYE for the final cycle
pad assertion); these were corrected before the passing full regression.

Reproduce from the worktree root:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
bash .issue49-runtime/stage2b6-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c \
  tests/qtest/rx113-mtu-test.c docs/rx113/issue-49-mtu-advanced-report.md
python3 .issue49-runtime/stage2b6-validate-patch.py
```

The patch contains three tracked files relative to the stated HEAD and
excludes AGENTS.md, manuals, runtime/build assets, the scratch alias and
repository metadata. The verifier reconstructs the HEAD files in worktree
scratch, checks/applies forward and reverse patches, and compares all
bytes with the working tree and base respectively, without writing `.git`.
Raw-diff checkpatch uses `--no-signoff` for the uncommitted deliverable.
Suggested commit subject:
`hw/rx: Honor RX113 MTU dead-time suppression`.

# Issue 49: RX113 MTU advanced output — stage 2b5 compensation delivery

## Current delivery (2026-11-11)

**Stage 2b and Issue #49 remain incomplete.** Item 1 of
`.issue49-runtime/issue-49-stage2b5.md` is implemented: MTU5 external
pulse measurement and crest/trough capture for firmware-driven dead-time
compensation. Item 2 was audited, but further implementation stops at the
unresolved duty-request source mapping described below, as required by
the brief and PO's instruction not to guess. This is a design-evidence
boundary, not a usage-window cutoff. Stage 3 (POE) was not started.
Earlier delivery sections and next-step ordering below are historical.

Base: `5a68775ab8df67dff956067f66f3131c63cfa800`.
Branch: `rx113/issue-49-mtu-advanced`.
Deliverables: working tree and
`.issue49-runtime/issue49-stage2b5-compensation.patch`.
No commit, merge, push, PR or upstream submission was performed; repository
metadata is read-only.

### Implemented boundary and manual evidence

Authority: supplied R01UH0448EJ0120 Rev.1.20, printed manual pages.

* Table 20.28, p.418; 20.3.10, p.515: TIORU/V/W settings 19h, 1Ah,
  1Bh measure low pulses; 1Dh, 1Eh, 1Fh measure high pulses. Each MTU5
  counter counts **up** only while its selected level is present, using
  its own TCR prescaler and start bit. The up/down signal in Figure 20.82
  is the complementary carrier direction, not signed delay accumulation.
  Pin edges themselves do not capture, and pulse measurement alone does
  not transfer to TGR. Counting wraps modulo 16 bits without inventing an
  MTU5 overflow vector. Normal MTU5 compare operation remains available.
* 20.3.11, pp.516-518, Figures 20.79-82: raw complementary crest and
  trough sources capture TCNTU/V/W into TGRU/V/W according to the low two
  TIOR bits. These decisions are independent of TIER3/4 and TITCR skipping.
  Captures use the pre-edge value (20.5.1, Figure 20.93, p.526), including
  elapsed faster MTU5 clocks before the carrier edge. TCNTCMPCLR clears
  the selected counter at capture. Existing MTU5 TIER bits gate capture
  requests on vectors 139/140/141. The manual advises firmware to leave
  these enables clear and service TGIA3/TCIV4 instead; tests deliberately
  enable selected MTU5 requests to verify the documented capture source.
* Figure 20.81 specifies firmware calculation and writes to MTU3.TGRD
  and MTU4.TGRC/D. No automatic duty correction is implemented. A qtest
  supplies a measured two-clock delay, calculates the correction in the
  test's firmware role, writes all three duty buffers with MTU4.TGRD last,
  and verifies the old temporary bank is consumed before the corrected
  transaction becomes active. This pending transaction and a live input
  with fractional counter time survive migration.
* Table 19.1 and Tables 19.3/9/11-14: qualified MTIC5U input routes are
  P11/PA4, V routes P10/PA6, and W routes P56/PB0, all with PSEL=1.
  PORT supplies resolved levels after bonding, PMR, PSEL and ASEL checks.
  Alternative routes feed the same signal; MTCLK and MTIOC0 feedback
  remain separate interfaces. General edge capture and noise filtering
  are outside this compensation delivery and are not claimed.
* MTU migration version 9 saves the three qualified measurement levels;
  counters, captures, control bits and fractional clocks use the existing
  migrated channel state. PORT derives routing from its existing state.
  Same-build migration is tested; older streams did not implement pulse
  measurement, and cross-version compensation acceptance is not claimed.

Implementation changes are confined to RX113 MTU, PORT, their RX113 SoC
wiring and the PORT header. RX62N behavior and sources are untouched.
Tests and this report also change. The pre-existing uncommitted increase
of the MTU qtest timeout from 60 to 180 seconds is retained in the patch;
no tests are excluded. TDER, TWCR and reset-PWM TGCR are not implemented
here, and the partial complementary diagnostic remains appropriate.

### Duty-request audit: unresolved decision and precise next step

The existing crest TGIA3 vector 129 and trough TCIV4 vector 138 paths,
including independent skipping and raw ELC sources, remain in place and
pass regression. Trough alone never requests vector 139; that vector now
also has its correct MTU5U capture use. No new complementary duty IRQs or
C/D compare requests have been wired speculatively.

The supplied manual establishes these facts, but does not establish a
unique request source for the remaining complementary duty cases:

1. Section 20.4.1 and Table 20.57, pp.520-521 list the TGR compare/input
   capture interrupts and their TIER enables without specifying which
   complementary counter/temporary comparator supplies each request.
2. Figure 20.37, p.481 explicitly draws TGRA3 and MTU4 underflow interrupt
   connections. The duty comparators' drawn connections go to the output
   controller; the figure does not identify duty IRQ connections. Absence
   of a drawn connection alone is insufficient evidence of suppression.
3. Section 20.3.8(b)/(j), pp.484,492-493 defines active versus temporary
   banks, three counters and turn-off/turn-on priority **for waveforms**.
   It does not say whether ignored waveform matches still request IRQs,
   or whether temporary-bank matches are IRQ sources at all.
4. Section 20.6.14, p.539 specifies complementary buffer grouping through
   MTU3.BFA/BFB. Section 20.6.15, p.540 explicitly suppresses buffered
   C/D requests in reset-synchronized PWM. Extending its mode-specific
   wording to a different complementary comparator path needs evidence.

Concrete distinguishing case: with TDDR=2 and steady U duty=4 in the
current 12-count half-cycle setup, MTU3.TCNT equals the duty on carrier
edges 3 and 23; the complementary waveform also uses MTU4.TCNT matches
on edges 5 and 21. A per-channel-TCNT interpretation requests TGIB3 on
3/23; an interpretation exposing every waveform compare adds 5/21; an
interpretation exposing only Figure 20.37's two interrupt connections
requests neither duty pair. A pending temporary-bank update introduces
further differences. The manual/brief does not select one of these
policies explicitly enough to add observable ICU/ELC edges confidently.

**Next step requires PO/PMO resolution:** supply authoritative clarification
or a hardware trace for TGIB3/TGIA4/TGIB4 in that distinguishing case,
including a selected Tb update and whether ignored turn-on matches request
interrupts; resolve C/D behavior when MTU3.BFA/BFB select buffering. If no
hardware evidence is available, explicitly choose and record an emulation
convention for those sources before implementation. Add count-edge ICU/ELC
and pending-bank tests for the approved mapping, then implement it. This
run stops implementation here rather than choosing that convention.

After that decision, the older unfinished stage 2b controls still require
implementation and tests: TDER read-before-clear/no-dead-time waveforms
with TDDR=1 setup; TWCR read-before-set, WRE/CCE and complementary
synchronous clearing/restart with retained versus initial outputs
(20.3.8(m)-(o), pp.497-502, and 20.7); and TGCR gating in reset-synchronized
PWM. Do not begin stage 3 (POE) or declare stage 2b complete before those
requirements are addressed.

### Validation

Evidence uses `stage2b5-*` names under `.issue49-runtime/`. The existing
build and populated subprojects were reused without downloads. All scratch
uses `.issue49-runtime/.t` or its `.s49` socket-path alias.

| Check | Result and runtime evidence |
| --- | --- |
| RX-only rebuild | PASS: `stage2b5-build-final.log` |
| Full MTU qtest | 66/66 PASS, 140.76 seconds: `stage2b5-regression.log` |
| Configured RX qtests and quick functional tests | 32 suites OK, four existing skips, zero failures: `stage2b5-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2b5-gdbsim-functional.log` |
| Cached FIT PLL/HOCO before and after reset | Both PASS: `stage2b5-fit-pll.log`, `stage2b5-fit-hoco.log` |
| SYSTEM protection/timing without/with FIT ELF | Both PASS: `stage2b5-system.log`, `stage2b5-system-elf.log` |
| Both gdbsim machines listed/run three seconds | Both PASS: `stage2b5-machine-help.log`, `stage2b5-gdbsim-smoke.log` |
| Changed-file and raw-diff checkpatch | Zero errors and warnings: `stage2b5-checkpatch-files.log`, `stage2b5-checkpatch-patch.log` |
| Forward/reverse patch dry runs and byte reconstruction | PASS: `stage2b5-patch-validation.log` |

`stage2b5-tested-binary.sha256` identifies the tested binary. Ten new MTU
cases cover all six TIOR pulse-measurement settings and both pad routes,
all four MTU5 prescalers, low/high gating, wraparound, capture selection
and clearing, raw-source capture despite ICU skipping, selective MTU5
capture requests, all three complementary modes, bonding/PMR/PSEL/ASEL,
MTCLK isolation, module-stop fractional retention, counter stop, reset,
and live measurement/corrected-buffer migration. All original 56 cases
are retained. The final change after regression only wraps a test source
line to satisfy checkpatch; it changes no test operation or device source.

The initial route-test setup incorrectly assumed the default package was
64 pins, then attempted reset-register assertions while MTU was module
stopped. The test now explicitly selects the 64-pin package and ungates
MTU before asserting its reset state, matching existing reset tests. No
device behavior was changed to satisfy those setup errors. Initial filter
invocations without the `/rx` prefix executed zero tests; the final full
66-case regression is the acceptance evidence.

Reproduce:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
bash .issue49-runtime/stage2b5-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c hw/rx/rx113-port.c \
  hw/rx/rx113.c include/hw/rx/rx113_port.h tests/qtest/rx113-mtu-test.c
python3 .issue49-runtime/stage2b5-validate-patch.py
```

The patch contains seven tracked files relative to the stated HEAD. It
excludes AGENTS.md, manuals, runtime/build assets, the scratch alias and
repository metadata. The verifier reconstructs the HEAD files in worktree
scratch, checks/applies forward and reverse patches, and compares all
bytes with the working tree and base respectively, without writing `.git`.
Raw-diff checkpatch uses `--no-signoff` for the uncommitted deliverable.
Suggested commit subject:
`hw/rx: Measure RX113 inverter delays for dead-time compensation`.

# Issue 49: RX113 MTU advanced output — stage 2b4 TGCR delivery

## Stage 2b4 TGCR delivery (historical)

**Stage 2b and Issue #49 remain incomplete.** This continuation implements
item 1 of `.issue49-runtime/issue-49-stage2b4.md`: TGCR brushless gating
within complementary modes. It stops at this tested boundary under the
requested limited usage window. Item 2, dead-time compensation with MTU5
measurement/capture, is next and has not been implemented. Item 3, remaining
duty interrupt wiring, follows it. Stage 3 (POE) was not started. There is
no unresolved manual/design decision requiring PO judgment at this boundary.
All earlier delivery sections and their next-step ordering are historical.

Base: `11666c4f740f499860f8ff353837ff3d1eeb8e21`.
Branch: `rx113/issue-49-mtu-advanced`.
Deliverables are the working tree and
`.issue49-runtime/issue49-stage2b4-tgcr.patch`. No commit, merge, push, PR,
or upstream submission was performed; repository metadata is read-only.

### Implemented boundary and manual evidence

Authority: supplied R01UH0448EJ0120 Rev.1.20, printed manual pages.

* 20.2.21, pp.439-440, Table 20.39; 20.3.8(p), pp.503-505,
  Figures 20.63-66: BDC enables the six-arm truth table. Feedback 000/111
  disables all arms; the other six combinations select one positive and
  one negative arm. P and N independently select fixed active level or
  the existing complementary waveform. Gating operates after waveform
  arbitration; it does not change the carrier, duty bank or interlocks.
  BDC=0 bypasses gating. TOCR1/TOCR2 polarity and TOER retain their roles.
* FB=1 selects UF/VF/WF. FB=0 uses resolved MTIOC0A/B/C pad levels from
  PORT/MPC, with package bonding, PMR, PSEL and analog exclusion applied.
  Existing MTIOC alternative routes supply the same feedback signals.
  MTCLKA/B/C/D are independent count inputs and are not used as feedback.
  No general MTU0 input capture or noise-filter implementation is claimed.
* TGCR resets to 80h, reads reserved bit 7 as one, and stores only bits
  0..6. TRWER protects its reads/writes (20.2.16, p.432 and
  20.3.8(4)(a), p.510), using the existing read-before-clear RWE rule.
* Migration version 8 saves TGCR and the three feedback levels. PORT
  routing is derived from its existing migrated state. Same-build tests
  exercise external and software feedback in modes 13 and 14, and switch
  from fixed-level gating to chopping after load to check retained carrier
  state. The full two-cycle waveform matrix exercises mode 15. Older
  streams default TGCR to disabled; cross-version waveform acceptance
  is not claimed.

Changed implementation files are `hw/rx/rx113-mtu.c`,
`hw/rx/rx113-port.c`, `hw/rx/rx113.c`, and `include/hw/rx/rx113_port.h`.
Tests and this report also change. The MTU qtest timeout in
`tests/qtest/meson.build` increases from 60 to 180 seconds because the new
physical-pad matrix alone takes about 65 seconds; no tests are excluded.
RX62N behavior and device sources are untouched. TGCR gating in
reset-synchronized PWM is outside this complementary-only delivery and
remains unavailable. The existing partial complementary-mode diagnostic
remains because restart, compensation and duty interrupt work is unfinished.

### Remaining stage 2b work and precise next step

Continue in the stage2b4 brief's order:

1. **Dead-time compensation next:** read 20.3.10 external pulse width
   measurement and 20.3.11, pp.516-518, Figures 20.79-82, together with
   MTU5 TIORU/V/W and TCNTCMPCLR definitions. Add qualified MTIC5U/V/W
   PORT inputs and the specified MTU5 up/down measurement, crest/trough
   capture, optional count clear and capture request behavior. Make capture
   decisions on raw crest/trough sources independently of TITCR ICU
   skipping. Figure 20.81 specifies firmware calculation of corrected
   duty and writes to MTU3.TGRD/MTU4.TGRC/D; do not invent automatic duty
   correction. Test measured delays and the resulting firmware-driven
   buffer transactions. General input capture remains out of scope.
2. Audit and implement remaining duty and buffered C/D compare requests
   using the documented up/down and temporary-bank arbitration. Existing
   crest TGIA3, trough TCIV4 and interrupt skipping remain unchanged here.
3. Earlier unfinished stage 2b requirements still apply: TDER
   read-before-clear and no-dead-time outputs (TDDR=1 setup), TWCR
   read-before-set/WRE/CCE and complementary synchronous clearing/restart,
   retaining versus restoring initial outputs (20.3.8(m)-(o), pp.497-502,
   and 20.7). TGCR in reset-synchronized PWM also needs a separate
   implementation and tests before full advanced-mode acceptance.

Do not start stage 3 (POE). No MTU5 compensation, new duty IRQ, TDER or
TWCR behavior is included in this patch.

### Validation

Final evidence uses `stage2b4-*` names under `.issue49-runtime/`.
The existing build and populated subprojects were reused without downloads.
Scratch uses `.issue49-runtime/.t` or its `.s49` alias for socket limits.

| Check | Result and runtime evidence |
| --- | --- |
| RX-only rebuild | PASS: `stage2b4-build-final.log` |
| Full MTU qtest | 56/56 PASS, 140.26 seconds: `stage2b4-regression.log` |
| Configured RX qtests and quick functional tests | 32 suites OK, four existing skips, zero failures: `stage2b4-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2b4-gdbsim-functional.log` |
| Cached FIT PLL/HOCO before and after reset | Both PASS: `stage2b4-fit-pll.log`, `stage2b4-fit-hoco.log` |
| SYSTEM protection/timing without/with FIT ELF | Both PASS: `stage2b4-system.log`, `stage2b4-system-elf.log` |
| Both gdbsim machines listed/run three seconds | Both PASS: `stage2b4-machine-help.log`, `stage2b4-gdbsim-smoke.log` |
| Changed-file and raw-diff checkpatch | Zero errors and warnings: `stage2b4-checkpatch-files.log`, `stage2b4-checkpatch-patch.log` |
| Forward/reverse patch dry runs and byte reconstruction | PASS: `stage2b4-patch-validation.log` |

`stage2b4-tested-binary.sha256` identifies the final tested binary. Two new
MTU cases cover the Table 20.39 matrix at every count edge over two carrier
cycles, both feedback sources, all four P/N combinations, both global
polarities and mixed per-arm polarity, BDC bypass, reserved-bit readback,
TRWER, reset, TOER, alternate routing, PMR/PSEL/ASEL qualification and
MTCLK isolation; and both feedback sources across migration in modes
13/14. The original 54 cases are retained. Standalone final-feature tests
also passed (`stage2b4-tgcr.log`, `stage2b4-tgcr-migration.log`); full
regression above uses the final rebuilt binary. The initial filtered
invocation selected zero tests because it omitted the `/rx` prefix; its
`stage2b4-tgcr-first.log` is not passing evidence.

The build's Meson regeneration printed a ninja jobserver descriptor error
from a nested build invocation, then completed successfully; the final
compile/link and all validation used the resulting binary. No source or
assertion workaround was needed.

Reproduce:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
bash .issue49-runtime/stage2b4-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c hw/rx/rx113-port.c \
  hw/rx/rx113.c include/hw/rx/rx113_port.h tests/qtest/rx113-mtu-test.c
python3 .issue49-runtime/stage2b4-validate-patch.py
```

The patch excludes AGENTS.md, manuals, runtime/build assets and repository
metadata. The verifier reconstructs the seven HEAD files in worktree
scratch, checks/applies forward and reverse patches, and compares all bytes
with the working tree and base respectively. It never writes `.git`.
Raw-diff checkpatch uses `--no-signoff` for this uncommitted deliverable.
Suggested commit subject:
`hw/rx: Gate RX113 complementary outputs with brushless feedback`.

# Issue 49: RX113 MTU advanced output — stage 2b3 skipping delivery

## Stage 2b3 skipping delivery (historical)

**Stage 2b and Issue #49 remain incomplete.** This continuation completes
only the first item in `.issue49-runtime/issue-49-stage2b3.md`:
TITCR/TITCNT interrupt skipping and TBTER buffer-snapshot control in
complementary modes. It stops at this independently tested boundary under
the requested limited usage window. TGCR is the next item; it has not been
started. Stage 3 (POE) was not started. No unresolved hardware decision is
being delegated to the PO, and this is not full issue acceptance.

Base: `6397ad4a2fd29d3db70533f9eeb055f51945edba`.
Branch: `rx113/issue-49-mtu-advanced`.
The working tree and `.issue49-runtime/issue49-stage2b3-skipping.patch`
are the deliverables. No commit, merge, push, PR or upstream submission
was performed; repository metadata is read-only. All delivery sections
below are historical and their next-step lists are superseded here.

### Implemented boundary and manual evidence

Authority: supplied R01UH0448EJ0120 Rev.1.20, printed manual pages.

* 20.2.26-27, pp.442-443; 20.3.8(3)(a)-(b), pp.506-507,
  Figure 20.69: each crest/trough source independently skips COR
  occurrences, then passes the next occurrence and clears its counter.
  TITCNT advances on sources even with TIER disabled. COR=0 bypasses
  skipping. Disabling the corresponding enable or setting COR=0 clears
  that counter. TITCNT is read-only, with reserved bits read as zero.
  CST stop and module stop retain skipping progress; module stop also
  retains the existing fractional carrier timing. Ordinary timer modes
  do not consume these complementary skipping counters.
* Crest TGIA3 and trough TCIV4 requests are filtered independently.
  Carrier direction, PSYE, waveform generation, polarity transfers and
  raw ELC compare/trough events remain driven by every source occurrence.
  The trough still uses vector 138 and ELC event 0x1a, with no vector 139
  request. Skipping does not acknowledge an existing ICU.IR request.
* 20.2.28, p.444; 20.3.8(3)(c), pp.508-510, Figures 20.70-72:
  BTE=01 blocks buffer-to-temporary snapshots while temporary-to-active
  transfers continue. The MTU4.TGRD commit write retains a pending
  transaction. Re-enabling transfers accepts it in an eligible Ta window;
  a Tb write cannot bypass the old temporary bank.
* BTE=10 links snapshots to source intervals opened by a non-skipped
  crest or trough occurrence. It excludes both Tb windows and follows
  the selected crest/trough update mode. Either enabled source can open
  the interval when both are selected (Figure 20.72). The initial zero
  count does not fabricate an interval before a source has passed.
  A write outside the interval waits for the next eligible opening;
  a write inside snapshots immediately. When skipping is disabled, linked
  snapshots never occur. BTE=11 remains a prohibited hardware setting;
  this model opens no snapshot window for it.
* MTU migration version 7 adds TITCR, TITCNT, TBTER and the two source
  interval latches. Existing temporary-bank and pending-transaction state
  is retained. Same-build tests cover migration inside crest Tb with a
  pending transaction, and inside the following open Ta interval with
  a new write after loading. Neither loading nor an initial zero TITCNT
  creates an interrupt edge. Older streams default these new controls
  to disabled; no cross-version advanced-waveform acceptance is claimed.

Only `hw/rx/rx113-mtu.c`, `tests/qtest/rx113-mtu-test.c` and this report
change. RX62N and all other device sources are untouched. The partial-mode
diagnostic remains because TGCR, restart and duty IRQ controls are still
unavailable. Delayed A/D start requests and their skipping links are not
implemented or claimed by this delivery.

### Remaining stage 2b work and precise next step

Continue in the supplied brief's order:

1. **TGCR gating next**: derive the six-arm gating table from 20.2.21,
   p.439 and 20.3.8(p), pp.503-505, Figures 20.63-66. Implement BDC,
   P/N, software UF/VF/WF feedback and FB=0 feedback from qualified
   MTIOC0A/B/C inputs. Include TRWER protection and physical PORT/PIDR
   assertions for both polarities and both feedback sources. Do not
   substitute externally supplied clock signals for the documented
   MTIOC0 inputs. No TGCR register storage or gating is added here.
2. Wire remaining duty and buffered C/D compare interrupts using the
   manual's up/down and temporary-bank arbitration. Only the existing
   crest TGIA3 and trough TCIV4 paths, now with skipping, are delivered.
3. Audit/finish output restart: TDER read-before-clear and no-dead-time
   waveforms (retaining the required TDDR=1 setup), TWCR read-before-set,
   WRE/CCE, complementary synchronous clearing and retained/initial-output
   restart (20.3.8(m)-(o), pp.497-502; 20.7 restart procedures). The
   generic synchronous-clear path still skips the complementary pair.
4. Implement 20.3.11, pp.516-519, dead-time compensation with the MTU5
   measurement/capture it requires. General input capture stays out of
   scope. No compensation or MTU5 capture behavior is added here.

Do not begin stage 3 (POE). The earlier waveform report's proposed first
step (TDER/restart) is historical; this run follows the new brief's order.

### Current validation

Evidence uses `stage2b3-*` names under `.issue49-runtime/`. The existing
build and populated subprojects were reused, with no downloads. Scratch
uses `.issue49-runtime/.t`, or its `.s49` alias for Unix socket limits.

| Check | Result and runtime evidence |
| --- | --- |
| RX-only rebuild | PASS: `stage2b3-build-final.log` |
| Full MTU qtest | 54/54 PASS within `stage2b3-regression.log` |
| All configured RX qtests and quick functional tests | 32 suites OK, four existing skips, zero failures: `stage2b3-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2b3-gdbsim-functional.log` |
| Cached FIT PLL/HOCO before and after reset | Both PASS: `stage2b3-fit-pll.log`, `stage2b3-fit-hoco.log` |
| SYSTEM protection/timing without/with FIT ELF | Both PASS: `stage2b3-system.log`, `stage2b3-system-elf.log` |
| Both gdbsim machines listed/run three seconds | Both PASS: `stage2b3-machine-help.log`, `stage2b3-gdbsim-smoke.log` |
| Changed-file and raw-diff checkpatch | Zero errors and warnings: `stage2b3-checkpatch-files.log`, `stage2b3-checkpatch-patch.log` |
| Forward/reverse patch dry runs and byte reconstruction | PASS: `stage2b3-patch-validation.log` |

`stage2b3-tested-binary.sha256` identifies the final tested QEMU binary.
The earlier standalone `stage2b3-mtu-first.log` ran 52 cases before the
migration/gating additions; final acceptance evidence is the 54-case
regression above.

Four new MTU test cases cover the 0..7 independent skipping-count matrix,
TITCNT read-only/reset/clear/retention behavior, source progress with TIER
clear, fractional module stop, ordinary-mode isolation, all update modes
with crest-only/trough-only/both skip sources, blocked snapshots versus
continued active transfers, immediate and delayed linked snapshots,
link-without-skipping suppression, and two migration timings. The existing
trough ELC test now also exercises a skipped ICU request. All prior MTU
cases are retained. An initial filtered invocation selected zero tests
because it omitted the `/rx` prefix; it is not passing evidence. The
corrected filtered tests each report one executed test.

Reproduce:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
build/tests/qtest/rx113-mtu-test
bash .issue49-runtime/stage2b3-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c \
  tests/qtest/rx113-mtu-test.c
python3 .issue49-runtime/stage2b3-validate-patch.py
```

The patch excludes AGENTS.md, manuals, runtime assets, build products and
repository metadata. The verifier reconstructs the three HEAD files in
worktree scratch, checks/applies the forward patch and compares all bytes
with the working tree, then checks/applies the reverse patch and compares
with HEAD. It never writes `.git`. Raw-diff checkpatch uses `--no-signoff`
for this uncommitted deliverable. Suggested commit subject:
`hw/rx: Implement RX113 complementary interrupt skipping`.

# Issue 49: RX113 MTU advanced output — stage 2b waveform delivery

## Waveform delivery (historical)

**Stage 2b and Issue #49 remain incomplete.** This continuation delivers
complementary PWM duty arbitration and physical PORT pad waveforms on top
of the committed carrier/subcounter and staged-buffer implementation.
It stops at a tested waveform-core boundary under the requested usage
limit. Extended controls and dead-time compensation remain below; stage 3
(POE) was not started. No unresolved hardware decision is being delegated
to the PO, and this is not a full acceptance claim.

Base: `d8e52c0a8ac7e90b5301c31d17665c7c2a8a9259`.
Branch: `rx113/issue-49-mtu-advanced`.
No commit, merge, push, PR or upstream submission was performed. Repository
metadata is read-only. Deliverables are the working-tree changes and
`.issue49-runtime/issue49-stage2b2-waveform.patch`, relative to that base.
All prior delivery sections below are historical.

### Implemented boundary and manual evidence

Authority: supplied R01UH0448EJ0120 Rev.1.20, printed manual pages.

* 20.3.8(j), Figures 20.46–48, pp.492–493: each phase separately evaluates
  four events (positive on/off and negative on/off) before advancing any
  counter or transferring buffers. The main up/down branches and both
  active TCNTS branches supply matches. Outgoing branches use active duty
  registers; incoming branches use the existing temporary bank when the
  mode and buffer-enable group select a transfer at that extremum. In
  unselected windows they use the active bank. A pending buffer write
  cannot bypass the existing temporary bank.
* Turn-off cancels the opposite pending turn-on. The incoming turn-off
  takes priority when the two turn-offs coincide. A turn-on also requires
  the opposite phase to be off. This suppresses short positive/negative
  pulses when the new turn-off overtakes the old delayed turn-on, as in
  Figures 20.47/48. Six logical on-state bits and six interlock bits are
  independent of physical polarity.
* 20.3.8(k), Figures 20.49–53, pp.494–496: simultaneous on/off for the same
  phase retains its latch while the off timing still arbitrates pending
  turn-ons. The tested duty spectrum includes zero, TDDR, TCDR and TGRA3,
  including positive 100% and 0%, and suppressed pulses close to each end.
* 20.3.8(i), Figures 20.44/45, pp.490–491, and Tables 20.31–37:
  complementary mode initially drives all six pads inactive through the
  initial dead time. The initial negative-phase state is qualified against
  the initial duty. Subsequent turn-off/on compares separate the arms by
  TDDR count edges. TOER enables its six documented pads. MTIOC3C remains
  outside the PWM outputs.
* 20.2.18–20, pp.434–438: complementary outputs use both positive and
  negative TOCR1 polarity bits, or all six TOCR2 polarity bits when TOCS
  selects TOCR2. BF selects TOLBR transfer at the crest, trough, or both,
  preserving BF. These transfers occur at the main carrier extremum,
  separately from the end-of-Tb duty transfer.
* 20.3.8(l), Figure 20.54, p.496: PSYE enables MTIOC3A, initially high,
  toggling at **both** the crest and trough. This differs from the existing
  reset-synchronized path, which retains its previous cycle behavior.
* Stopping both counters restores the complementary initial output and
  counter state. Module stop retains the waveform, interlock, count region
  and fractional clock. Matching external MTCLKA/B edges use the same
  coupled waveform path. Partial/mismatched clock configurations retain
  the existing unsupported-configuration diagnostic.
* Migration version 6 saves on-state, interlock and initial-output hold,
  alongside version 5's counters, temporary bank and pending snapshot.
  The same-build migration test observes PORT pads inside crest Tb with a
  second transaction pending, then through the crest and trough transfers.
  Old streams did not implement complementary pads; compatibility of an
  advanced waveform from such a stream is not claimed.

The complementary-mode diagnostic now names the remaining **extended
controls and duty interrupts unavailable**. It no longer says that all
outputs are unavailable. There is no claim of full complementary-mode
support. RX62N and all other device source files are untouched.

### Explicitly unfinished behavior and next step

The next implementation should add **TDER read-before-clear and the
no-dead-time waveform path**, retaining the required TDDR=1 carrier setup
while generating inverse arms without a dead-time gap. Add physical PORT
checks for both polarity selections and zero/full duty before changing
that diagnostic. Then implement TWCR read-before-set, synchronous
restart/WRE/CCE (20.3.8(m)–(o), pp.497–502, and 20.7 restart procedures),
including retained-output and initial-output restart cases.

Still required in stage 2b, and not implemented by register storage:

* Duty and buffered C/D compare interrupt arbitration; only the existing
  crest TGIA3 and trough TCIV4/ELC sources are implemented on this path.
* TITCR/TITCNT interrupt skipping and TBTER-linked buffer transfers,
  including temporary-bank snapshot and transfer gating.
* TGCR brushless gating, software feedback and the documented external
  feedback source, within the complementary modes.
* 20.3.11, pp.516–519: dead-time compensation and the MTU5 measurement/
  capture range it requires. General input capture remains out of scope.
* Dedicated complementary synchronous clear/restart behavior. The generic
  synchronous-clear path still skips the coupled complementary pair.

The basic CST stop/restart implemented here does not substitute for TWCR.
No no-dead-time, skipping, TGCR, compensation, or POE result is claimed.
Do not begin stage 3 before completing stage 2.

### Current validation

Validation evidence is under `.issue49-runtime/`, with `stage2b2-*` names.
No subprojects were fetched. TMPDIR stays inside the worktree; the existing
`.s49` alias resolves to the same scratch directory for socket path limits.

| Check | Result and runtime evidence |
| --- | --- |
| RX-only rebuild | PASS: `stage2b2-build.log`, `stage2b2-build-final.log` |
| MTU qtest | 50/50 PASS in the final regression; earlier equal-duty run also 50/50: `stage2b2-regression.log`, `stage2b2-mtu-second.log` |
| All configured RX qtests and quick functional tests | 32 suites OK, four existing skips, zero failures: `stage2b2-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2b2-gdbsim-functional.log` |
| Cached FIT PLL/HOCO before and after reset | Both PASS: `stage2b2-fit-pll.log`, `stage2b2-fit-hoco.log` |
| SYSTEM protection/timing without/with FIT ELF | Both PASS: `stage2b2-system.log`, `stage2b2-system-elf.log` |
| Both gdbsim machines listed/run three seconds | Both PASS: `stage2b2-machine-help.log`, `stage2b2-gdbsim-smoke.log` |
| Changed-file and raw-diff checkpatch | Zero errors and warnings: `stage2b2-checkpatch-files.log`, `stage2b2-checkpatch-patch.log` |
| Forward/reverse patch dry runs and byte reconstruction | PASS: `stage2b2-patch-validation.log` |

`stage2b2-tested-binary.sha256` identifies the final tested QEMU binary.
The full final regression uses the distinct-duty waveform test and the
extended clock/pad assertions; the earlier standalone 50-test run used
equal phase duties and predates those test enhancements.

The three new test cases cover all six pads and PIDR: four carrier cycles
for every duty from 0 through TGRA3, with distinct duties on the three
phases and checks one ns before each edge; overtaken turn-on suppression
at crest and trough with both the single-extremum and dual-extremum update
modes; and per-arm polarity, TOLBR transfers, TOER and PSYE. Existing clock
and migration tests now also assert live complementary pads. The original
47 MTU tests and their counter/register/IRQ assertions remain intact.

The initial waveform test exposed a missing interlock effect when duty
was exactly TDDR; the off timing now retains its arbitration effect even
when coincident on/off retains the latch. Early attempt logs also record
a test invoked during relinking, an incorrect GLib path selecting zero
tests, and a run interrupted by a binary relink. Those are not passing
validation evidence. All final tests use the completed build.

Reproduce:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
build/tests/qtest/rx113-mtu-test
bash .issue49-runtime/stage2b2-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c \
  tests/qtest/rx113-mtu-test.c
python3 .issue49-runtime/stage2b2-validate-patch.py
```

The patch contains exactly `hw/rx/rx113-mtu.c`,
`tests/qtest/rx113-mtu-test.c` and this report. It excludes AGENTS.md,
manuals, runtime assets, build products and repository metadata.
The verifier reconstructs these three base files in worktree scratch,
checks/applies the forward patch and compares every byte with this tree,
then checks/applies the reverse patch and compares with the base again.
It does not write `.git`. Raw-diff checkpatch uses `--no-signoff` for the
uncommitted deliverable. Suggested commit subject:
`hw/rx: Generate RX113 complementary PWM pad waveforms`.

# Issue 49: RX113 MTU advanced output — stage 2b partial delivery

## Stage 2b carrier delivery (historical)

**Stage 2b and Issue #49 remain incomplete.** This run delivers the
complementary **carrier/subcounter and staged-buffer foundation**, before
the waveform arbitration boundary. It does not yet deliver the complete
working PWM output core requested in the continuation brief. The existing
stage 1 and stage 2a implementations remain in place. Stage 3 (POE) was
not started. This is a partial run boundary, not a claim of full acceptance
or an unresolved decision delegated to the PO.

Base: `31296f4d8a3790b7c83a1ed4fd9930ca05f818f5` (stage 2a).
Branch: `rx113/issue-49-mtu-advanced`. No commit, merge, push, PR or
upstream submission was performed; repository metadata is read-only.
The deliverable is the working-tree diff and
`.issue49-runtime/issue49-stage2b-carrier.patch`.

| Stage | Current state |
| --- | --- |
| 1 | Committed as `3f31fe9583`; retained |
| 2a | Committed as `31296f4d8a`; retained |
| 2b carrier and staged buffers | Implemented and tested as described below |
| 2b duty arbitration, PWM pads and extended controls | Not implemented; explicit diagnostic retained |
| 3 POE | Not started |

### Implemented boundary and manual evidence

Authority: supplied R01UH0448EJ0120 Rev.1.20, printed manual pages.

* 20.3.8(a), Figure 20.39, p.483: one coupled count-edge function advances
  MTU3/4 with the TDDR offset and shared direction. MTU3 runs from TDDR to
  TGRA, MTU4 from zero to TCDR. TCNTS has separate crest down/up and
  trough up/down regions, and a dormant Ta region. Initial operation begins
  in the trough window. This is not two ordinary up-counters.
* Figure 20.38, p.482: the implemented clock domain is the documented
  simultaneous CST3/CST4 start with matching count source and edge.
  Both internal and external MTCLKA/B sources use the same coupled edge.
  Partial starts or mismatched clocks freeze this foundation and produce
  an unsupported-configuration diagnostic; no policy for those setups is
  inferred. Scheduling currently visits every complementary count edge;
  event-distance optimization remains future work.
* 20.3.8(b),(c),(g),(h), pp.484,486,488-489, Figure 20.43:
  all five hidden temporary registers are separate from both buffers and
  active registers. Mode selection initializes all five active registers.
  Thereafter, MTU4.TGRD is the transaction commit write. In Ta it snapshots
  all five buffers; in Tb it arms a deferred snapshot. At an end-of-Tb
  boundary the selected transfer consumes the existing temporary bank
  **before** a pending snapshot replaces it. Thus a Tb write cannot bypass
  the temporary bank. Modes 13/14/15 select crest/trough/both transfers.
* 20.6.14, p.539: MTU3 BFA controls TGRA3, TGRA4 and TCDR transfers;
  BFB controls TGRB3 and TGRB4 transfers. MTU4's mode does not independently
  run another counter engine.
* 20.2.22-25, pp.440-441, and 20.2.16, p.432: TCNTS is read-only;
  TDDR/TCDR/TCBR are 16-bit registers with FFFFh reset values. TRWER
  protects TDDR/TCDR, while TCBR remains accessible. TCNTS and the five
  hidden registers, region and pending-transfer state are migrated in
  device migration version 5. Same-build migration is tested inside Tb1
  with a pending second transaction and a fractional count clock.
* 20.6.13, p.539: stopping both counters restores TDDR/zero and the initial
  carrier/subcounter direction. Module stop instead retains region, count
  and fractional clock state. Starting again continues from the relevant
  state. Output restart semantics are still pending with the waveform core.
* Table 20.57, p.520, and 20.3.8(3), p.506: crest requests use TGIA3;
  the MTU4 trough emits ELC 0x1a and TCIV4 vector 138 together. The generic
  wrap delivery now shares MTU4's overflow/underflow IRQ, instead of
  incorrectly indexing the MTU5 U interrupt at vector 139. ELC delivery
  remains independent of the ICU interrupt enable. No ICU vector was added.

Compare decisions sample the stored count before the edge advances it,
matching this model's existing count-edge convention. Direction changes
and crest/trough requests occur on that sampled terminal count. The
TCNTS transition table implemented by the new edge function is:

| Region | Sampled transition | Action |
| --- | --- | --- |
| Ta, main up | MTU3.TCNT = TCDR | Start Tb1 at TGRA3, counting down |
| Tb1 down | TCNTS = TCDR | Count up |
| Tb1 up | TCNTS = TGRA3 | Halt at zero; selected crest transfer, then pending snapshot |
| Ta, main down | MTU4.TCNT = TDDR | Start Tb2 at zero, counting up |
| Tb2 up | TCNTS = TDDR | Count down |
| Tb2 down | TCNTS = zero | Halt at TGRA3; selected trough transfer, then pending snapshot |

### Explicitly unfinished behavior and next step

Complementary mode selection now diagnoses **duty arbitration, outputs and
extended controls unavailable**. All complementary pad enables stay off.
The counter and buffer implementation is observable, but is not advertised
as usable complementary PWM. TOER cannot enable an unimplemented waveform.
Existing reset PWM and normal/PWM1/PWM2 pads retain their behavior.

The very next change must implement 20.3.8(j),(k), pp.492-496: compare the
old active and new temporary duty banks against MTU3/4 and the active
TCNTS branches, arbitrate the four turn-on/turn-off events for each phase,
apply turn-off precedence/interlocks, and preserve simultaneous on/off
matches and the 0%/100% cases. Add physical PORT/PIDR tests for all six
pads through crest and trough, including duty changes across both dead-time
windows, before enabling complementary pad output. The present hidden-bank
separation and region state are the inputs for that implementation; there
is no immediate-copy waveform substitute. Duty and buffered C/D compare
interrupt arbitration is also pending; only crest/trough sources are
implemented on this path.

Then complete TDER read-before-clear and no-dead-time output behavior;
TWCR read-before-set, synchronous clearing/WRE/CCE and initial output
restart; complementary TOCR/PSYE and TOLBR transfer; TITCR/TITCNT/TBTER
skipping and linked transfers; TGCR brushless gating; and 20.3.11 MTU5
measurement/capture compensation. None is silently replaced by register
storage. TDDR currently provides the carrier offset, **not verified output
dead-time separation**. Generic synchronous-clear processing deliberately
skips the complementary pair until its dedicated restart path is added.
No complementary duty/temporary-match IRQ or output acceptance result is
claimed. Retain the unavailable diagnostic until these paths have tests.
Do not begin POE before completing stage 2.

### Current validation

Evidence uses new `stage2b-*` runtime logs; earlier delivery logs are
historical. No subprojects were fetched. All scratch uses the worktree
runtime directory, with the existing `.s49` alias for socket path limits.

| Check | Result and evidence under `.issue49-runtime/` |
| --- | --- |
| RX-only rebuild | PASS: `stage2b-build-final.log` |
| MTU qtest | 47/47 PASS (37 retained, ten new): `stage2b-mtu.log` |
| All configured RX qtests and quick functional tests | 32 suites OK, four existing skips, zero failures: `stage2b-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2b-gdbsim-functional.log` |
| Cached FIT PLL and HOCO, before/after reset | Both PASS: `stage2b-fit-pll.log`, `stage2b-fit-hoco.log` |
| SYSTEM protection/timing, without/with FIT ELF | Both PASS: `stage2b-system.log`, `stage2b-system-elf.log` |
| Both gdbsim machines listed/run three seconds | Both PASS: `stage2b-machine-help.log`, `stage2b-gdbsim-smoke.log` |
| Changed-file/raw-diff checkpatch | Zero errors and warnings: `stage2b-checkpatch-files.log`, `stage2b-checkpatch-patch.log` |
| Forward/reverse patch dry runs and reconstructed bytes | PASS: `stage2b-patch-validation.log` |

The ten new tests cover three carrier cycles with observations one ns
before each edge; all TCNTS regions; all three update modes; commit-last
ordering; deferred writes during Tb; transfer of the old temporary bank
before a new pending bank; both buffer-enable groups; cycle increases and
decreases across the crest while retaining the outgoing down-ramp;
register resets, read-only and protection behavior; counter stop/restart;
module stop at a fractional clock; external clock coupling; independent
ELC/ICU trough delivery and absence of a spurious vector 139 request; and
migration with both old temporary data and a pending new transaction.
These are carrier/register/IRQ tests, not complementary pad tests.

The first test attempts are retained in `stage2b-mtu-first.log` through
`stage2b-mtu-third.log`. They exposed mistakes in the test's repeating
subcounter lookup and the ELC setup (module-stop bit and operation
selection), which were corrected before the passing run. No original
assertion was removed or weakened. `stage2b-tested-binary.sha256` identifies
the tested device binary; the final additional cycle test rebuilt only the
qtest executable. The final regression uses all 47 MTU cases.

Reproduce:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
build/tests/qtest/rx113-mtu-test
bash .issue49-runtime/stage2b-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c \
  tests/qtest/rx113-mtu-test.c
python3 .issue49-runtime/stage2b-validate-patch.py
```

Current patch files, relative to the stage-2a commit:

* `hw/rx/rx113-mtu.c`
* `tests/qtest/rx113-mtu-test.c`
* `docs/rx113/issue-49-mtu-advanced-report.md`

The raw diff excludes AGENTS.md, manuals, runtime assets, build products
and repository metadata. It is checked with `--no-signoff` because no
commit was authorized or possible. The verifier reconstructs the three
base files in runtime scratch, checks/applies the forward patch and
compares every byte with the working tree, then checks/applies the reverse
patch and compares every byte with the base again. It does not write
`.git`. Suggested commit subject:
`hw/rx: Add RX113 complementary carrier and staged buffer core`.

## Stage 2a delivery record (historical; superseded by current status above)

## Stage 2a delivery status (historical)

Stage 1 is committed as `3f31fe9583a0ab265880b46299d2acf84afb5a24`.
This run delivers the **reset-synchronized PWM portion of stage 2**, at
an independently working boundary permitted by
`.issue49-runtime/issue-49-stage2.md` and the continuation instruction.
**Stage 2 and Issue #49 are not complete.** Branch:
`rx113/issue-49-mtu-advanced`; this patch is relative to the stage-1 commit.

| Stage | State |
| --- | --- |
| 1: normal/PWM1/PWM2 MTIOC pad waveforms | PASS: committed; existing tests retained |
| 2a: reset-synchronized PWM | Implemented: shared MTU3/4 count edges, six pads, cycle buffers, polarity/PSYE, migration and seven new qtests |
| 2b: complementary PWM and compensation | Not implemented: modes 13–15 retain unsupported-mode diagnostics; detailed next step below |
| 3: POE | Not implemented; not started |

The complementary engine, its five temporary registers and old/new compare
arbitration do not fit in the remainder of this run with adequate validation.
This delivery stops after reset-synchronized PWM; it introduces no partial
complementary counter engine or immediate-copy substitute. This is a run
size boundary, not an unresolved hardware design decision. The D-006 merge
gate remains with the PO/PMO; this report does not claim full acceptance.
No commit, merge, push, PR or upstream submission was performed. Repository
metadata is read-only. The working-tree changes and
`.issue49-runtime/issue49-stage2-reset-pwm.patch` are the deliverables.

## Stage 2 implemented subset

Authority remains the supplied R01UH0448EJ0120 Rev.1.20 manual. References
below use its printed page numbers.

* Section 20.3.7, Tables 20.52/53 and Figures 20.35/36 (pp.477–479), and
  20.6.16 (p.541): MTU3 controls both counters with CST3 and its selected
  clock/edge. CST4 and MTU4's clock/clear settings do not independently
  drive the pair. Internal clocks use a shared deadline and edge;
  external MTCLKA/B edges use the same shared-edge function. The cycle
  clear clears both counters and suppresses both overflow requests.
* Each of the three duty matches toggles its positive/negative pair;
  cycle clear toggles all three pairs. A simultaneous duty/cycle match
  toggles once, including the cycle=duty example in Figure 20.35. The
  match decisions for both channels precede count updates and transfers.
* Sections 20.2.18–20, Tables 20.30–38 (pp.434–438): TOCR1 or TOCR2
  selects output polarity. With no dead time, negative outputs are exact
  inverses and only positive polarity bits take effect. PSYE enables
  MTIOC3A, initially high, toggling at the cycle match. TOER controls its
  six documented PWM pads; it does not gate counting or the waveform
  latch. BF=01 transfers TOLBR to TOCR2 at the cycle clear, preserving BF.
* Section 20.6.15 and Figure 20.118 (p.540): MTU3 BFA/BFB control the
  buffer pairs on both channels, while MTU4 TMDR stays at its normal-mode
  initial value. All enabled cycle/duty buffers transfer at MTU3.TGRA
  compare match, after the old compare decisions. Buffered C/D registers
  generate no compare requests on either channel. Incorrectly setting
  MTU4's BFA/BFB inhibits its corresponding C/D output as documented.
* Section 20.6.26 (p.547): PCLK/1 zero-cycle compare requests stay
  asserted for both channels, even with CST4 clear and MTU4's stored TCR
  selecting neither that clock nor that clear source. Clearing ICU.IR
  cannot manufacture another request edge while the source stays flat.
* Section 20.7.3, Figure 20.152 step (10) (p.572): counter stop restores
  the initial reset-PWM output. Module stop retains the waveform and
  fractional clock state. Section 20.8.2 normal-mode ELC operation is
  rejected for the controlled MTU4 as well as MTU3.

Migration version 4 saves the three waveform phases independently of
polarity, in addition to the existing count, fractional clock, output and
control state. Same-build migration is checked between duty and cycle
matches, including a pending cycle-buffer change. Old streams never
implemented reset PWM; no advanced-waveform compatibility claim is made
for those streams. RX62N and all other device source files are untouched.
Brushless motor gating (TGCR) is not added by this subset.

## Stage 2 validation

Validation is recorded in the `stage2-*` runtime logs; the stage-1 logs
and results below are historical and have not been overwritten.
| Check | Result and evidence in `.issue49-runtime/` |
| --- | --- |
| RX-only rebuild with supplied tools | PASS: `stage2-build.log` |
| MTU qtest | 37/37 PASS, including seven new cases: `stage2-mtu.log` |
| All configured RX qtests and quick functional tests | 32 suites OK, 4 existing skips, 0 failures: `stage2-regression.log` |
| Existing gdbsim U-Boot functional | PASS: `stage2-gdbsim-functional.log` |
| Genuine cached FIT PLL/HOCO, before and after reset | Both PASS: `stage2-fit-pll.log`, `stage2-fit-hoco.log` |
| SYSTEM protection/timing, without/with FIT ELF | Both PASS: `stage2-system.log`, `stage2-system-elf.log` |
| Both gdbsim machines listed and run three seconds | Both PASS: `stage2-machine-help.log`, `stage2-gdbsim-smoke.log` |
| Changed C files and raw-diff checkpatch | PASS, zero errors and warnings: `stage2-checkpatch-files.log`, `stage2-checkpatch-patch.log` |
| Patch forward/reverse dry runs and reconstructed-byte comparison | PASS: `stage2-patch-validation.log` |

`stage2-tested-binary.sha256` identifies the tested binary. Reproduce with:

```sh
. .issue49-runtime/env.sh
make -C build -j$(nproc)
build/tests/qtest/rx113-mtu-test
bash .issue49-runtime/stage2-regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c \
  tests/qtest/rx113-mtu-test.c
```

The raw-diff check uses `--no-signoff` for the uncommitted deliverable.
The patch contains exactly the three files listed below, relative to the
stage-1 commit. Validation reconstructs those base files in scratch,
checks forward application, applies and compares all reconstructed files
byte-for-byte with the working tree, then checks/applies the reverse patch
and compares with the original base files, using `git apply` with an
explicit scratch directory. This does not write `.git`. GNU `patch` passed
its forward dry run but could not preserve ownership during reconstruction
in this sandbox (`stage2-patch-validation-ownership.log`); `git apply`
avoids that unrelated metadata operation.

The seven new tests observe physical PORT pads and PIDR. They cover three
successive cycles, all six phases and PSYE, one nanosecond before edges,
CST4-only inactivity, CST3-only paired counting with a conflicting MTU4
clock, shared cycle clear, all four buffers and buffered-IRQ suppression,
per-phase polarity and timed TOLBR transfer, TOER/PSYE gates, cycle=duty
coincidence at FFFFh with overflow suppression, module stop at a fractional
count, prohibited ELC restart, external clock edges, reset, zero-cycle
flat requests, and live buffered-waveform migration. The original 30 MTU
cases and their assertions remain in place.

The zero-cycle test initially exposed an ordering defect: the MTU4 flat
request check observed MTU3 after its increment. The shared tick now takes
all compare decisions before updating either counter. The failing attempt
is retained as `stage2-mtu-zero-first.log` and
`stage2-regression-zero-first.log`; neither is counted as a passing check.
All scratch uses the supplied runtime TMPDIR (or its `.s49` alias for the
Meson socket suites); no subprojects were re-fetched.

## Stage 1 manual evidence and implemented behavior (historical)

Authority: the supplied R01UH0448EJ0120 Rev.1.20 manual, retained as
`.issue49-runtime/rx113.pdf` and `rx113.txt`. The following references are
printed manual page numbers, not PDF viewer offsets.

| Reference | Behavior |
| --- | --- |
| 20.2.3, Tables 20.12-27, pp.408-417 | TIOR initial low/high, set/clear/toggle, output prohibition and capture/buffer exclusion for all 16 MTIOC signals |
| 20.2.14, p.430 | Stopping a counter retains the output; writing TIOR while stopped reinstates its initial level |
| 20.2.16-17, pp.432-433 | TOER gates its six documented signals; reserved bits read one; TRWER protects TOER and TOCR1/2 |
| 20.2.18-20, pp.434-438 | TOCR1/2 and TOLBR masks/reset/readback, set-once TOCL protection; PSYE remains writable after TOCL |
| 20.3.3/20.3.5, pp.461, 467-470 | PWM1 A/B and C/D pairing onto A/C; PWM2 duty pads and clear-to-initial behavior; buffered duty changes and simultaneous-match retention |
| 20.5.2, p.529; 20.6.26, p.547 | Existing compare-edge timing retained; zero-period optimization preserves first output transitions and observable toggles |
| 18.3, 19.2, Tables 19.2-21, pp.353-395 | All documented MTIOC pad alternatives, package filtering, PMR/PFS selection, GPIO ownership, existing open-drain/input resolution and 64-pin PSRA behavior |

TOCR polarity, PSYE waveform generation and TOLBR-to-TOCR2 transfer apply
**only to the advanced modes** (20.2.18-20). They have no waveform effect
in normal/PWM1/PWM2 operation. Their register/protection behavior was present
in stage 1; reset-synchronized waveform/transfer effects are now added by
the stage-2 subset above. Complementary effects still remain.
This is not a claim that storing these fields implements advanced PWM.

Manual corrections resolved without inventing a hardware policy:

* Reset-synchronized PWM is 20.3.7 (pp.477-479), complementary PWM is
  20.3.8 (pp.480-510), and dead-time compensation is 20.3.11 (pp.516-519).
  The brief's reference to 20.3.11 for reset-synchronized PWM is inaccurate.
* Tables 19.17/18 print `MTIOC4BD` for PD2. The package pin lists, including
  Table 1.5, consistently identify PD2 as MTIOC4D. The routing uses MTIOC4D.

MTU outputs are sixteen level wires and sixteen enable wires, connected to
RX113 PORT. Tests observe PORT's physical `levels`/`driving` properties and
PIDR, rather than an internal timer latch. PORT retains its existing GPIO,
analog selection, external input, open-drain and package behavior. No shared
peripheral or RX62N source was modified.

Matches from one count edge are evaluated together. In PWM1, equal paired
matches leave the pad unchanged. In PWM2, a duty match coincident with
its own or a synchronous cycle clear also leaves that pad unchanged;
other duty pads restore their TIOR initial level. This preserves the
manual's history-dependent 0%/100% cases. Buffer transfers do not replace
already-decided match events. PWM2 zero writes and overflow also restore
initial levels. Both counter stop and module stop retain pad outputs.

Migration version 3 saves the timer output latch and output controls, and
the PORT signal inputs. Derived pad state is restored on load. Same-build
migration of a live PWM waveform is tested at a fractional count interval;
older streams do not contain a saved waveform latch and are not evidence
of waveform restoration. Existing migration, phase counting, cascade, ELC,
clock and module-stop tests remain unchanged.

## Stage 1 validation (historical)

| Check | Result and evidence in `.issue49-runtime/` |
| --- | --- |
| RX-only configure and build | PASS: `configure.log`, `build-final.log` |
| MTU qtest | 30/30 PASS, including nine new pad cases: `mtu-first.log`, `regression.log` |
| All configured RX qtests and quick functional tests | 32 suites OK, 4 existing skips, 0 failures: `regression.log` |
| All RX113 parts, reset, RX113/RX62N FPU, LCDC and IWDT functional | PASS within `regression.log` |
| Existing peripheral qtests, including DTC/ELC/USB/CTSU | PASS within `regression.log`; existing test expectations unchanged |
| Genuine FIT PLL/HOCO, before and after reset | Both PASS: `fit-build-*.log`, `fit-pll.log`, `fit-hoco.log`, `verify-*/result.json` |
| SYSTEM protection/timing, without/with FIT ELF | Both PASS: `system.log`, `system-elf.log` |
| Both gdbsim machines listed, run three seconds and exit cleanly | Both PASS: `machine-help.log`, `gdbsim-smoke.json` |
| Existing gdbsim U-Boot functional | PASS: `gdbsim-functional.log`; existing Linux/sash flaky annotation unchanged |
| Changed-file checkpatch | PASS: zero errors and warnings; `checkpatch-files.log` |
| Raw-diff checkpatch | Zero errors, one generic new-report MAINTAINERS warning; `checkpatch-patch.log` |
| Patch forward/reverse dry runs and byte comparison | PASS: `patch-validation.log`; reconstructed files exactly match this tree |

The nine new cases cover all 16 outputs' initial/set/clear/toggle states,
PWM1 A/C pairing on all five channels, PWM2 on MTU0..2, three consecutive
periods, one nanosecond before transitions, history-dependent 0%/100%,
simultaneous paired matches, counter-stop retention, return to normal mode,
TOER's six gates, TRWER and TOCL, reset, synchronous PWM2 clearing,
buffered PWM1 duty/period changes, module-stop retention, zero-period
first transitions and toggles with IRQs disabled, alternate MPC routes,
64-pin PSRA/unbonded pads, and live pad-waveform migration. The original
21 MTU tests retain their assertions and all pass.

The raw-diff check uses `--no-signoff` because this deliverable is an
uncommitted diff, not a signed commit/mail patch. Commit preparation is
left to the PMO. Its sole MAINTAINERS warning concerns the new fork-local
report; no implementation style warning remains.

`tested-binary.sha256` identifies the binary used by all regression and FIT
checks. Build dependency metadata damaged by the initial overlapping
builds is separately regenerated in `build-cache-clean.log`; the binary
and MTU-test executable comparison is recorded in
`build-hash-comparison.log`. Both rebuilt executables are byte-identical
to those used for the passing checks.

All scratch/build products are in this worktree; the primary `TMPDIR` is
`.issue49-runtime/.t`. The generic socket suites require a shorter Unix
socket pathname than the absolute runtime path permits. For the Meson
regression invocations only, `regression.sh` uses the short `.s49` symlink
as TMPDIR; it verifies that this resolves to the exact same
`.issue49-runtime/.t` directory. No scratch is placed in `/tmp`. Subprojects were already present and were not
re-fetched. FIT probes use the unchanged pinned-hash build script and
cached vendor archives. The scratch Python hook supplies TMPDIR to
QEMUMachine's temporary-directory argument. It does not change device code
or test expectations.

Overlapping initial build commands caused an invalid archive at link
time. The duplicate commands were interrupted, the archive moved aside,
and validation uses a subsequent single build. The failed attempt's logs
are retained; it is not counted as a passing build. The first full suite
also exposed Unix socket path limits in generic monitor/netdev tests
(`regression-first*.log`), resolved by the short alias above. The first
gdbsim attempt could not update a timestamp in the read-only neighboring
cache; copying the unchanged cached U-Boot asset into this worktree's
writable cache resolved it (`gdbsim-functional-first.log`). No test was
excluded or assertion changed to obtain the final green results.

Reproduce from the worktree root:

```sh
. .issue49-runtime/env.sh
# For a fresh build directory:
# mkdir build && (cd build && ../configure --target-list=rx-softmmu --disable-docs)
ninja -C build -j8
build/tests/qtest/rx113-mtu-test
bash .issue49-runtime/regression.sh
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-mtu.c hw/rx/rx113-port.c \
  hw/rx/rx113.c include/hw/rx/rx113_mtu.h include/hw/rx/rx113_port.h \
  tests/qtest/rx113-mtu-test.c
```

## Stage 2a remaining work and D-006 merge gate (historical)

The required order remains stage 2, then stage 3. Engineering estimates
include manual-based qtests and regression, not just register storage.

| Remaining scope | Work and estimate |
| --- | --- |
| Stage 2 coupled PWM engine | MTU3/4 up/down chronology, TCNTS crest/trough regions, all five hidden temporary registers, old/new compare arbitration, interlocks, TDDR/TCDR/TCBR/TDER/TWCR and restart behavior, TOCR polarity/PSYE and TOLBR transfer, TITCR/TITCNT/TBTER, and MTU5 measurement/capture for compensation. Reset-synchronized PWM is now delivered separately above. Approximately 5-8 engineering days, consistent with Issue #29's estimate. Do not substitute independent ordinary counters or immediate buffer copies. |
| MTU4 underflow | Still part of stage 2: ELC event 0x1a and the complementary-mode trough request on TCIV4 vector 138 must be resolved together; no extra ICU vector should be fabricated. |
| Stage 3 POE | RX113 POE register model (manual section 21), qualified PORT POE pin inputs, software and oscillator-stop sources, high-impedance cutoff, status/IRQ and documented release conditions. Approximately 2-3 engineering days after stage 2, including pad tests and SYSTEM/ICU integration. |
| Full issue acceptance | Complementary crest/trough/IRQ, dead-time separation, advanced complementary buffer transfer/interlock and POE assertion/release qtests have not run because these features are not implemented. They remain mandatory before declaring Issue #49 complete. |

Input capture outside the explicitly requested MTU5 compensation path
remains outside Issue #49. Existing Issue #29 limitations for sub-PCLK
synchronizer/bus arbitration and peer-module features are unchanged.

## Stage 2a next implementation step (historical)

Start with a manual-derived transition table for the coupled complementary
engine (20.3.8(a)–(c), Figures 20.39–42, pp.483–487), including Ta/Tb1/Tb2
and TCNTS activation, crest/trough direction changes and the initial
TDDR offset. Add all five hidden temporary registers and their pending
write transaction: MTU4.TGRD is the commit write, Ta accepts it immediately,
Tb defers it, and only the selected end-of-Tb boundary transfers it to
active registers. Do not reuse reset PWM's direct cycle-buffer transfers
for this path. Then implement the documented old/new compare arbitration
and interlocks, with crest/trough pad/IRQ tests before enabling modes
13–15. Include MTU4 underflow ELC 0x1a and TCIV4 vector 138 together in
that engine; the current delivery deliberately does not change either.

Still mandatory in stage 2: TDDR/TCDR/TCBR/TCNTS and hidden-state migration;
TDER read-before-clear and TWCR read-before-set/restart behavior;
complementary TOCR/PSYE and TOLBR transfers; TITCR/TITCNT/TBTER skip and
transfer links; and the MTU5 measurement/capture compensation path in
20.3.11 (pp.516–519). These registers/functions are **not implemented by
this patch**. Their absence is not masked by register-only storage.
Retain the unsupported diagnostics until the corresponding engine is
validated. Stage 3 POE must wait for stage 2 completion.

## Stage 2a patch contents (historical)

* `hw/rx/rx113-mtu.c`
* `tests/qtest/rx113-mtu-test.c`
* `docs/rx113/issue-49-mtu-advanced-report.md`

The patch is relative to `3f31fe9583a0ab265880b46299d2acf84afb5a24`.
It excludes AGENTS.md, runtime/manual/build assets, the scratch alias and
repository metadata. Suggested commit subject:
`hw/rx: Drive RX113 reset-synchronized PWM from a shared count edge`.

## Stage 1 patch contents (historical)

* `hw/rx/rx113-mtu.c`
* `hw/rx/rx113-port.c`
* `hw/rx/rx113.c`
* `include/hw/rx/rx113_mtu.h`
* `include/hw/rx/rx113_port.h`
* `tests/qtest/rx113-mtu-test.c`
* `docs/rx113/issue-49-mtu-advanced-report.md`

The patch excludes AGENTS.md, pre-existing `probe.txt`, the `.s49` scratch
alias, runtime assets,
manuals, toolchains, build products and repository metadata. Suggested
commit subject: `hw/rx: Drive RX113 MTIOC pads in normal and PWM modes`.
