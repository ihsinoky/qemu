<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #1 Worker report — RX113 skeleton, run 2

Date: 2026-11-05. Base: `rx113/dev` at
`90b3ee2f476944f4643e0534e1efd54b30e4897d` (Issue #3 merged).
Branch: `rx113/issue-1-skeleton-v2`.

## Implementation

- New abstract MCU `rx113-mcu`, concrete part `r5f51138-mcu`, and machine
  **`rx113-r5f51138`**, with Kconfig/Meson integration. The name identifies
  the MCU and internal-memory machine, following QEMU's lower-case,
  hyphen-separated naming and the run2 instruction; no external SDRAM or
  gdbsim DTB assumptions apply.
- RAM 64 KiB at `0x00000000`, E2 data flash 8 KiB at `0x00100000`, code
  flash 512 KiB at `0xfff80000`. The flash base is calculated as
  `(1ULL << 32) - rom_flash_size`, rather than a fixed part-specific base.
  Flash uses ROM memory regions: programming/erase is outside this issue.
- ICU at `0x00087000`, using the existing model with **`ipr-count=256`**.
  IRQ/FIR outputs and CPU ACK are connected just as for RX62N. The full
  RX113 vector-to-IPR and level-trigger tables are supplied by the MCU.
- Two CMT units at `0x00088000/0x00088010` drive vectors 28–31.
  Two TMR instances at `0x00088200/0x00088210` drive 174–179/180–185;
  each instance contains two channels with CMIA/CMIB/OVI outputs.
- SCI0/1/2/5/6/12 basic registers (+0 through +7) are instantiated at
  `0x0008a000/a020/a040/a0a0/a0c0/b300`, respectively. A table supplies
  vector bases 214/218/186/222/226/238, in ERI/RXI/TXI/TEI order.
  Serial backends are ordered SCI0, SCI1, SCI2, SCI5, SCI6, SCI12.
- Every modeled timer/SCI receives a fixed **32 MHz PCLKB**, without the
  RX62N XTAL multiplier. SYSTEM writes do not change that frequency.
- `-kernel` accepts little-endian RX ELF, with all loadable segments in
  code flash. The ELF entry is deliberately ignored; the RX113 machine
  reset hook reads the reset vector at `0xfffffffc` after device reset and
  ROM restoration. Firmware must
  supply this vector and its RAM initialization code. `-bios` accepts a
  raw image starting at the capacity-derived flash base, bounded by the
  flash size. The two options are mutually exclusive.
- RX62N, gdbsim, CPU, ICU and all existing peripheral implementations
  remain unchanged.

## Stubs and limitations

`unimplemented-device` logs accesses with `-d unimp`, returns zero on
reads and discards writes. Table 5.1 supplies the register block locations:

| Stub | Base | Reserved size |
|---|---|---|
| SYSTEM / SYSTEM status | 0x00080000 / 0x0008c290 | 0x400 / 0x10 |
| DTC | 0x00082400 | 0x10 |
| IWDT / DAC | 0x00088030 / 0x00088040 | 0x10 each |
| MTU0–5 | 0x00088600 | 0x300 (includes MTU5 through 0x000888b6) |
| S12AD | 0x00089000 | 0x100 |
| SCI8 / SCI9 | 0x0008a100 / 0x0008a120 | 0x20 each |
| CAC / DOC / ELC | 0x0008b000 / 0x0008b080 / 0x0008b100 | 0x20 / 0x10 / 0x30 |
| PORT / MPC / RTC | 0x0008c000 / 0x0008c100 / 0x0008c400 | 0x100 each |
| USB0 / LCDC / CTSU | 0x000a0000 / 0x000a0800 / 0x000a0900 | 0x200 / 0x100 / 0x100 |

SCI0/1/2/5/6 extension registers at +8 through +0x1f are stubs;
SCI12 extensions at +8 through +0x33 are stubs, including its special
serial interface. SCIX0–3 are represented in the ICU tables but have no
interrupt-producing peripheral implementation. Reserved holes inside
these coarse stub blocks also return zero. LCD display state, pin routing,
DMA/DTC transfers, clock switching, module-stop and flash programming are
not modeled. This is a Phase 1 skeleton, not full firmware compatibility.

## Source of IPR data and mechanical transcription

Design inputs: `rx62n-rx113-diff.md` **v2**, run1 report and Issue #3 report.
The run1 report is absent from the supplied dev tree despite the run2
instruction's expectation; it was read in full with
`git show rx113/issue-1-skeleton:docs/rx113/issue-1-skeleton-report.md`.
No old run1 code was imported.

Hardware manual: **R01UH0448EJ0120 Rev.1.20**, existing
`../.issue1-runtime/rx113.pdf` and layout text `rx113.txt`; neither was
re-downloaded. SHA-256 of the PDF:
`a41f74559bd8f8aa36749519da119bcc28dcb40f6a23e6f5d403b9113fd19356`.
The source comments cite **Table 14.3, pp.266–272**.

The retained runtime script `.issue1-runtime/implement.py` initially
extracted the seven Table 14.3 pages from the layout text. It matched
`vector-number + four-digit hex offset + detection-form` on each data row,
asserted the resulting vector sequence was exactly 0 through 255, and:

1. Initialized all 256 IPR entries to `0xff` (unassigned).
2. Parsed decimal `IPRnnn` on assigned rows; blank merged IPR cells
   inherited the previous assigned row's IPR. Reserved/trap rows cleared
   the inherited value and retained `0xff`.
3. Collected every `Level` and `Edge/Level` row into the level list.
   IRQ0–7 therefore start in level mode; existing IRQCR writes can select
   edge detection. The ICU model's existing detection behavior is used.
4. Emitted the literal 256-byte C table (hex values) and level list, then
   reviewed the shared TMR/SCI IPR groups against v2 and the manual.

The resulting level list is 16, 32–34, 44, 47, 57, 64–71, 90, 108,
170–171, 186, 189, 214, 217, 218, 221, 222, 225, 226, 229, 230, 233,
234, 237, 238, 241–246 and 249. SCI8/9 vectors remain in the table even
though those channels are stubs. No compressed IPR numbering is used.
The generator is historical scratch, not a command to rerun over final
hand-reviewed source files.

## Difference-table §7 follow-up

| Item | Run2 disposition |
|---|---|
| BOM / other part capacities | R5F51138 is the instructed default (Table 1.3 capacities). Product BOM is unavailable; PO confirmation remains. R5F51135–37 classes are deferred pending RAM-capacity confirmation. |
| Full IPR/trigger table | Completed from all 256 Table 14.3 rows; Issue #3's extension is used without modification. |
| CMT/TMR compatibility | Existing models instantiated with the confirmed layout and interrupt order. Verification below records the exercised behavior; no claim of full model accuracy. |
| SCI extensions / SCI12 | Basic six-channel implementation; extensions and SCIX-producing logic deferred as described above. |
| SCI8/9 existence | Table 5.1 (pp.112–113) explicitly lists SCI8 at 0x0008a100 and SCI9 at 0x0008a120, including SMR through SPMR. The v2 assumption based on missing header definitions is superseded by this manual evidence. Both are stubs in this issue. |
| FPU / FPSW / FIT | Existing RXv1 execution core reused. FPU restrictions and real FIT initialization remain unverified and deferred; RX113 firmware must avoid FPU instructions. |
| SYSTEM startup | Minimal firmware smoke exercises reset and SCI0 directly, without SYSTEM setup. Real firmware waiting on oscillator-ready flags can stall on zero-returning stubs; this smoke does not establish FIT startup compatibility. |
| Machine selection | Implemented as explicitly authorized by run2; final acceptance/merge remains with PO. |

## Verification

- Configure and full `make -j8`: **PASS**, exit 0. Final incremental
  `make -j4` after the RX113 reset correction: **PASS**, exit 0.
  The first make introspection emitted a ninja inherited-jobserver file
  descriptor diagnostic, but the actual build proceeded and completed;
  the final incremental build has no such diagnostic.
- `-M help`: **PASS**, lists `rx113-r5f51138` and both existing gdbsim
  machines (`gdbsim-r5f562n7`, `gdbsim-r5f562n8`).
- ELF and raw-image functional smoke: **PASS**, both subtests. Also run
  through Meson as `func-rx-rx113`: **PASS**, 2 subtests, about 6.6 seconds.
- Peripheral integration via retained `.issue1-runtime/verify-peripherals.py`:
  **PASS**. All 256 IPR bytes work without a global property override;
  RAM endpoints read/write, both flash regions' endpoints reject ordinary
  guest writes, CMT0–3 emit vectors 28–31, TMR0–3 compare-A emits
  174/177/180/183, and SCI0/1/2/5/6/12 transmitter enabling with TIE emits
  each table-assigned TXI. Full TMR event coverage, serial modes and timer
  timing accuracy are not established by these checks.
- gdbsim regression: **PASS**, each machine runs for 3 seconds without
  early exit and terminates on SIGTERM with status 0. The final RX qtest
  suite has **9 passing test programs, 2 skipped, 0 failures**, including
  all 5 existing `rx-icu-test` subtests for the two gdbsim machines and
  configurable IPR counts. The skips are cdrom-test and qos-test.
- Invalid image checks: **PASS**, non-ELF kernel, ELF with a load segment
  outside code flash, oversized raw image, and simultaneous kernel/bios
  options all exit 1 with an error rather than starting the guest.
- Checkpatch on every changed file: **0 errors, 0 warnings**. The combined
  patch check has **0 errors** and one advisory warning about whether new
  files need MAINTAINERS entries. No upstream maintainer assignment is
  proposed for this fork-local skeleton. `git diff --check` passes.

The initial ELF smoke caught an actual reset-order issue: `rom_reset()`
can free the loader buffer after installing data in a ROM MemoryRegion;
the shared RX CPU's `rom_ptr()`-based reset handler then cannot obtain
its vector. RX113's machine reset calls `qemu_devices_reset(type)` and
then reads the vector from the flash MemoryRegion's backing storage.
Both first boot and subsequent resets are tested; CPU/RX62N code stays
unchanged. Reading only the ELF entry would have hidden this problem.

The direct functional-test invocation initially lacked `MESON_BUILD_ROOT`.
The generic test helper's machine-availability probe also attempted to
create a directory under read-only `/var/tmp`; the new test configures
its own VM directly instead. Its scratch files stay in the build tree.
Neither issue required changes to the shared test framework.

Evidence remains in `.issue1-runtime/`: `configure.log`, `build.log`,
`build-final.log`, `machines.log`, `smoke-functional.log`, `smoke-meson.log`,
`peripherals.log`, `gdbsim-smoke.log`, `qtest-final.log`, `invalid-images.log`,
`checkpatch-files.log`, and `checkpatch-patch.log`. Test console/QMP logs
are under `build/tests/functional/rx/`. Runtime scripts and generated
artifacts are not committed.

All commands run inside `.issue1-v2-worktree/`. Build and scratch stay
inside it. `TMPDIR` is set explicitly so tools do not use `/tmp`.
The four previously downloaded, pinned subprojects were copied from the
original checkout; none were fetched again.

```sh
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
mkdir -p build .t
export TMPDIR="$PWD/.t"
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
make -C build -j8
```

The committed `tests/functional/rx/test_rx113.py` constructs a two-segment
ELF32 image without an RX assembler. One segment contains the instructions
`mov.l #0x8a000,r1; mov.b #0x20,2[r1]; mov.b #'R',3[r1]; bra.b .`;
the other contains a reset vector pointing to `0xfff80000`. `e_entry` is
zero, so successful SCI output proves use of the reset vector. A second
case constructs a 512 KiB raw flash image with the same program/vector.
Each case observes `R` on SCI0, runs for three seconds, checks the machine
is still running and PC is the loop at `0xfff8000e`, and resets/restarts
and observes `R` again. To reproduce this smoke alone:

```sh
export QEMU_TEST_QEMU_BINARY="$PWD/build/qemu-system-rx"
export PYTHONPATH="$PWD/python:$PWD/tests/functional"
export MESON_BUILD_ROOT="$PWD/build"
./build/pyvenv/bin/python3 tests/functional/rx/test_rx113.py
```

Alternatively run the registered test through the build system:

```sh
meson test -C build --no-rebuild func-rx-rx113 --print-errorlogs
make -C build check-qtest-rx -j4
python3 .issue1-runtime/verify-peripherals.py
python3 .issue1-runtime/verify-invalid-images.py
```

## Delivery

The original `.git` is read-only. The local shared clone
`.issue1-v2-worktree/` holds the branch and commits and depends on the
original object store. No merge, push, GitHub PR or upstream submission
was performed. PMO owns push and Draft PR creation against `rx113/dev`.
GitHub's latest dev ref was checked with `git ls-remote` and matched the
base above before branching. The root checkout remains on `rx113/dev`.

Import without merging (from the original repository in a writable
session), using either the clone or the incremental bundle:

```sh
git fetch ./.issue1-v2-worktree \
  rx113/issue-1-skeleton-v2:rx113/issue-1-skeleton-v2
# Or, for transfer to a different machine with the base commit present:
git fetch ./.issue1-v2-worktree/.issue1-runtime/issue-1-skeleton-v2.bundle \
  rx113/issue-1-skeleton-v2:rx113/issue-1-skeleton-v2
```

Delivery commits, in order:

1. `dc5915a4f46c4e1866322333f6fb4c22b61002c5` —
   `hw/rx: Add the RX113 R5F51138 MCU and machine skeleton`.
2. The tip of `rx113/issue-1-skeleton-v2` —
   `docs/rx113: Record Issue 1 run 2 implementation and validation`
   (this report, committed separately from implementation).

The incremental bundle contains these two commits and requires the dev
base named at the start. It is verified against that base in the local
clone; unlike the shared clone directory, it can be transferred alone to
a repository that already has the base.

List delivery commits with exact hashes in the clone with:
`git log --reverse --oneline origin/rx113/dev..rx113/issue-1-skeleton-v2`.
