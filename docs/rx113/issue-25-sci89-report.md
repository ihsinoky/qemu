<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 25: Instantiate SCI8 and SCI9 on RX113

Work branch: `rx113/issue-25-sci89`, base `1025a92ac2`.
The complete `.issue25-runtime/instructions.md`, root `AGENTS.md`,
D-006/D-007, RX113 wiring and the local R01UH0448EJ0120 Rev.1.20 manual
were consulted. No push, merge, PR or upstream submission was performed.
No new network downloads were needed: the pinned subprojects and cached
Renesas R_BSP archive were reused.

## Implementation and manual evidence

SCI8 and SCI9 now instantiate the existing `renesas-sci` model, replacing
the whole-channel stubs. The MCU has eight SCI children. A single channel
table supplies address, interrupt base, stop register/bit and extension
region for both realization and live clock/gate propagation. The shared
SCI model and all RX62N files are unchanged.

| Channel | MMIO base | ERI / RXI / TXI / TEI | Module stop | Backend index |
|---|---|---|---|---|
| SCI8 | `0x0008a100` | 230 / 231 / 232 / 233 | MSTPCRC.MSTPC27 | 6 |
| SCI9 | `0x0008a120` | 234 / 235 / 236 / 237 | MSTPCRC.MSTPC26 | 7 |

Manual authority:

* Table 5.1, pp.114-115, and sections 28.2.2-16 give the byte register
  addresses. Offsets 0..7 are SMR, BRR, SCR, TDR, SSR, RDR, SCMR and SEMR.
  Offsets 8..13 are SNFR, SIMR1/2/3, SISR and SPMR. Following the existing
  SCIe channels, each extension stub covers offset 8..31 (`0x18` bytes),
  including the reserved tail; neither stub overlaps the shared model.
* Table 14.3, pp.271-272, gives the vectors above. ERI/TEI are level
  sources and RXI/TXI are edge sources. Existing RX113 ICU IPR and trigger
  tables already contain these entries and need no changes.
* Section 11.2.4, p.217: MSTPCRC is at `0x00080018`; MSTPC27 controls
  SCI8 and MSTPC26 controls SCI9. One stops the clock and zero enables it.
  PRCR.PRC1 protects writes. Reset value is `0x7ffffffe`; reserved bits
  read as one. The documented writable mask is `0x8c180001`.
* Section 28 introductory text, Table 28.3 and Figure 28.1, pp.806/809/810,
  group SCI8/9 with SCI0/2/6 as SCIe. Their register definitions are common.
  In section 28.2.10, p.839, SEMR.ACS0 is reserved for this entire group;
  the MTU clock option belongs to SCI1/5/12. No SCI8/9-specific register
  difference requires a shared-model change.
* Section 28 uses PCLK to mean PCLKB. Both new children participate in
  the same source/divider changes and retained gate interface as Issue 10.

The SYSTEM migration version advances from 3 to 4 and appends MSTPCRC
without moving existing fields. Its decoder initializes MSTPCRC to reset
for older SYSTEM sections. Same-build running/stopped migration tests
include the new children; whole-machine migration from older binaries
with only six SCI devices is not claimed.

## Serial backend allocation

The existing order remains SCI0, SCI1, SCI2, SCI5, SCI6, SCI12 (indices
0..5). SCI8 and SCI9 are appended at indices 6 and 7. Thus the seventh
`-serial` argument connects SCI8 and the eighth connects SCI9. For example,
use six `-serial null` arguments followed by `-serial stdio` to connect
SCI8. Unspecified backends remain disconnected, following `serial_hd()`.
The socket-backed test sends a distinct byte in both directions through
each of all eight backends, checking the original order as well as the
new assignments.

## Reproduction

Run from the source root, with all temporary files inside the worktree:

```sh
export TMPDIR="$PWD/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$HOME/workspace/projects/qemu/.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
export PYTHONPATH="$PWD/python"
mkdir -p .t build
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
make -C build -j4
QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx" build/tests/qtest/rx113-sci-test
QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx" build/tests/qtest/rx113-clock-test
meson test -C build --no-rebuild --suite qtest-rx --suite func-rx --print-errorlogs
```

The genuine FIT probe uses the commands in
`tests/functional/rx/fit_boot/README.md`, substituting
`.issue25-runtime/fit-pll` and `.issue25-runtime/fit-hoco` for build
outputs and `.issue25-runtime/verify-pll` / `verify-hoco` for evidence.
Copy the existing verified `r_bsp.zip` into both output directories before
running `build.py`; use `--hoco` for the second configuration.

## Verification

Final validation completed on 2026-11-09 using `build/qemu-system-rx`
(SHA-256 `58b41d11e0d452abbcb603ace89d11868569c3a5051fb2800aea1350bd3ce329`).
All evidence paths below are relative to `.issue25-runtime/`.

| Check | Actual result | Evidence |
|---|---|---|
| RX-only build, existing configuration and pinned subprojects | PASS, exit 0 | `final-build-retry.log`, `configure.log` |
| SCI backend order and SCI8/9 functionality | 3/3 PASS | `final-sci.log` |
| Clock propagation, gates, migration and RX62N legacy path | 9/9 PASS | `final-clock.log` |
| Full RX qtest + quick functional suites | 21 OK, 4 existing skips, 0 failures (25 entries) | `final-regression.log` |
| Phase 1, ELF/raw flash, all four part variants and reset | 8/8 PASS | `final-regression.log` |
| RX113 FPU restriction / RX62N FPU | 2/2 PASS | `final-regression.log` |
| LCDC functional / LCDC / RTC / PORT / ICU qtests | 1/1, 6/6, 20/20, 6/6, 5/5 PASS | `final-regression.log` |
| Genuine FIT rebuild, PLL / HOCO | 2/2 builds PASS | `final-fit-build-pll.log`, `final-fit-build-hoco.log` |
| FIT startup, PLL / HOCO, before and after reset | 4/4 startup observations PASS | `final-fit-pll.log`, `final-fit-hoco.log`, `verify-{pll,hoco}/result.json` |
| SYSTEM protection and exact oscillator deadlines, without / with PLL ELF | 2/2 runs PASS | `final-system.log`, `final-system-elf.log` |
| Both gdbsim machines listed, running for 3 seconds, clean shutdown | 2/2 PASS, exit 0 each | `machine-help.log`, `gdbsim-smoke.json` |
| gdbsim U-Boot functional test | 1 PASS, Linux/sash has 1 existing flaky skip | `final-gdbsim-uboot.log`, `final-gdbsim-details.log` |
| Changed-file checkpatch (all six files) | 0 errors, 0 warnings | `final-checkpatch-files.log` |
| Whitespace and reverse import-patch check | PASS, exit 0 | `patch-validation.log` |

The SCI test checks distinct bytes in both directions on all eight serial
backends. For each new channel it checks all four ERI/RXI/TXI/TEI vectors,
PRC1 protection, reset and reserved-bit readback, PCLKB changes during
transmission, retained transmit/receive timing across stop/restart, blocked
MMIO while stopped, receive overrun, and reset restoration. The clock test
also exercises SCI8/9 in its running/stopped migration cases and checks
frequency propagation to both new children.

The four suite skips are existing cdrom, qos, info_usernet and linters.
The Linux/sash skip is the pre-existing upstream flaky-test annotation.
No required test was excluded to obtain these results.

The initial build exposed a missing `poll` declaration in the new SCI
qtest; replacing its host wait with GLib `GPollFD`/`g_poll` was the only
source correction during this continuation. The subsequent complete build
and all tests above passed. Ninja recovered its interrupted log and
recompiled additional objects. An initial concurrent test-list query hit
Meson's regeneration lock; actual test execution began after the successful
build. A harmless host-Meson Python `sitecustomize` import warning was
resolved in the local scratch hook; functional tests used the QMP-enabled
build venv and passed.

The exact test commands are retained in `final-regression.sh` and
`gdbsim-smoke.py`. The local `test-env/sitecustomize.py` redirects
QEMUMachine's default scratch into TMPDIR. U-Boot was copied from the
existing local cache into `cache/download`; `QEMU_TEST_CACHE_DIR` points
there. All scratch is inside this worktree; no test framework was changed.

## Delivery

The repository's `.git` directory is read-only in this execution environment,
so no commit was made. The import patch is `.issue25-runtime/issue25.patch`.
A copy of the index (`patch.index`) and a local object directory
(`patch-objects`, with `.git/objects` as its alternate) allow `git add -N`
for the new test and report without writing `.git`. With those Git
environment overrides, `git diff HEAD > .issue25-runtime/issue25.patch`
includes all six deliverable files. `git apply --check --reverse` verifies
that the patch matches the final working tree. Runtime logs, scratch and
fork-local `AGENTS.md` are excluded. No push or merge was performed.

## Remaining limitations and D-006 accounting

The requested SCI8/9 instantiation, byte-stream I/O, interrupt wiring,
PCLKB propagation and retained stop/restart are implemented. There is no
SCI8/9-specific shared-model change awaiting approval.

The following inherited or out-of-scope behavior remains explicit for
the PO/PMO merge review under D-006:

* Extension registers SNFR, SIMR1/2/3, SISR and SPMR remain logging stubs,
  exactly as required by the existing-channel extension policy. Faithful
  noise filtering and simple I2C/SPI require pin/clock/protocol behavior
  and shared SCI interfaces, beyond an RX113-only instantiation change.
  Estimated effort: several days for register/mode infrastructure and
  one to two weeks or more for protocol/pin behavior and meaningful tests.
* The shared model's existing simplifications carry over, including its
  baud-duration formula, stored-only SCMR/SEMR options and lack of faithful
  synchronous/smart-card operation. Timing tests deliberately verify the
  existing model's 72 us character duration at BRR=8/PCLKB=32 MHz, not a
  hardware-accurate baud equation. Correcting these requires separately
  authorized shared-model work with RX62N compatibility analysis;
  estimated several days for baud/register semantics, weeks for modes.
  No new channel-specific discrepancy was found.
* MSTPCRC's non-SCI controls (RAM0, CAC, IrDA and DSLPE) retain documented
  writable/readback bits but have no newly implemented functional effect.
  This preserves the instructed invariant that other peripherals do not
  change. CAC is still a stub and IrDA/deep-sleep machinery is absent;
  RAM access gating also needs memory/CPU integration. Estimates: one to
  three days for RAM gating, several days for deep sleep, and one to two
  weeks or more for CAC/IrDA models and tests. These are separate peripheral
  and low-power tasks; stored bit readback does not validate their function.
