<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 33 — RX113 DTCa implementation report

Worker branch: `rx113/issue-33-dtc`. No push, merge or upstream submission.
Base commit: `573b7a563861ada0e42fe2c9e61317548f38c60d`.
The implementation follows `.issue33-runtime/instructions.md` and D-003 /
D-006. Shared peripheral/ICU sources and RX62N sources are unchanged.

## Implementation and manual basis

References are to the supplied R01UH0448EJ0120 Rev.1.20. The local PDF was
converted with `pdftotext -layout`; the extracted text is a scratch asset.

| Reference | Behavior |
| --- | --- |
| 16.2.7–11, pp.300–303; Table 5.1 | DTCCR at 0x82400, DTCVBR at +4, DTCADMOD at +8, DTCST at +0xc, read-only DTCSTS at +0xe; widths, reserved bits and reset values |
| 16.2.8 | DTCVBR 1 KiB alignment, bit 27 sign extension into bits 31–28 |
| 16.2.1–6; 16.3.1; 16.10, pp.296–305,323 | Guest vector read at DTCVBR+4*n; 16-byte full / 12-byte short descriptors, little-endian word layout, ignored descriptor-address low two bits, 24-bit signed short addresses |
| 16.4.3, p.310 | Normal byte/word/longword transfer; independent fixed/increment/decrement addressing; 16-bit CRA decrement, including initial zero representing 65536 |
| 16.4.4, p.311 | Repeat transfer; CRAL decrement/reload from CRAH; selected source/destination area rewound; zero size represents 256; no completion interrupt for DISEL=0 |
| 16.4.5, p.312 | A block of 1–256 elements per request; selected block area rewound, CRAL reloaded, CRB decremented with zero representing 65536 |
| 16.4.6; Table 16.3, pp.308,313 | Adjacent descriptor chains after every transfer or only at count boundary; repeat boundary uses CRAL, block boundary uses CRB; final descriptor determines interrupt and DTCE handling |
| 16.4.1–2, pp.308–309 | Actual retained-register read skip, including skipped vector read; RRS=0 invalidation; forced reread after chain or terminal normal/block transfer; fixed-address write-back skip; full-address mode never writes MRA/MRB |
| 14.2.6; Table 14.3; 14.4.2–4 | Only documented DTCER vectors writable; IER masks activation, IR remains latched; DTC arbitration by lowest vector, independent of IPR and CPU PSW |
| 14.4.1.1; Table 14.4; 16.3; 16.7 | IR cleared before a nonterminal DISEL=0 last transfer; terminal/DISEL=1 requests retained for CPU; terminal DTCE clear; serial interfaces retain one additional request while IR=1 |
| 11.2.2; 16.9, pp.214,322 | PRC1-protected MSTPA28; reset bit value zero; no new acceptance while stopped; accepted chain completes before gate takes effect; inaccessible registers read zero/discard writes while gated |
| 16.2.11; 16.8 | ACT/vector monitoring and named transfer-end event pulse after an entire request |

All data movement uses checked QEMU address-space transactions against guest
memory and MMIO. There is no host-pointer interpretation of guest addresses,
no synthesized transfer payload and no firmware-specific fast path. A
prohibited mode/size or failed bus transaction logs `LOG_GUEST_ERROR`, stops
DTCST and invalidates cached transfer information. This is a deterministic
handling policy for invalid input, not a new hardware error/status register.

A timer processes one descriptor per step, at a **1 ns scheduling interval**.
A block is atomic within that step (at most 256 data elements). Chain steps
continue with ACT set, keeping guest-generated unbounded chains from spinning
inside one host callback. ACT is also observable by a transfer reading
DTCSTS. This interval is not hardware execution timing; see the explicit
D-006 residual item below. Already-accepted chains continue after DTCST=0
or MSTPA28=1, as required by 16.2.10 and 16.9. Requests remain latched during
DTCST/module stop and are considered when restarted. Register/configuration
changes during an active transfer must follow the manual's stop/wait rules.

Reset cancels the timer, clears DTC controls/cache/pending requests and
DTC-owned ICU request state, including a completion request already sent to
the CPU. It does not reset unrelated ICU state or external signal drivers.
Migration includes pending and retained requests, input levels, active-chain
position, cached registers, gate/start state and timer. DTC-selected external
IRQ detection modes are restored on load because the shared ICU does not
migrate its source-sense array. Old stub-only machine migration is not a
compatibility contract.

## RX113-only activation path

`rx113.c` creates `/machine/mcu/dtc` after the unchanged shared ICU. SCI,
S12AD, MTU, CMT, TMR, PORT and RTC output wires now enter the DTC's 256 GPIO
inputs at their existing vector numbers. The ICU's SWINT output also enters
input 27. IRQs with DTCE=0 are forwarded immediately and unchanged to the
original ICU GPIO. DTC-selected edge requests are latched in the ICU's IR
backing array without calling its CPU request path. Pending requests are
coalesced while IR=1, with the documented extra serial request retention.
External IRQs require edge detection as specified by 14.4.3; a prohibited
level-sensitive DTC selection is diagnosed rather than turned into a fake
edge transfer.

A second MMIO region overlays the ICU's 0x87000–0x875ff region only on RX113.
DTCER accesses use the shared ICU's existing, migratable `dtcer[]` backing
array, without invoking its stored-only "DTC not implemented" handler.
Other MMIO accesses delegate directly to the original ICU MemoryRegion,
with the same access widths and endian. The wrapper notices IR clears and
IER/destination changes to resume retained requests without requiring a new
peripheral edge. It does not implement a replacement CPU interrupt arbiter.

The CPU ACK wire passes through a second RX113 wrapper. While delegating an
ACK to the shared ICU, it temporarily hides IR bits belonging to pending or
active DTC requests, then restores them. This prevents the ICU's CPU rescan
from choosing a DTC-owned request. DISEL/completion requests are delivered
through the existing shared ICU GPIO path and remain visible to that rescan.
Thus ICU priority and CPU/FIR delivery remain shared behavior. These narrow
backing-array accesses and wrappers are RX113 integration choices; **no
shared ICU source or interface was changed**. Direct qtest injection into
`/machine/mcu/icu` bypasses this front end; DTC request injection uses
`/machine/mcu/dtc`, or a real connected peripheral as the integration test
does. The latter checks S12ADI0, MTU0 TGIA0 and SCI0 TXI0.

The one-element named `transfer-end` GPIO output exposes the DTC event for a
future ELC. Peripheral inputs for all Table 14.3 DTC vectors are available;
this does not instantiate currently absent USB/RSPI/SSI/RIIC/ELC peers.

## Validation

All checks below use the final binary recorded in
`.issue33-runtime/tested-binary.sha256`. Runtime logs, FIT build assets and
delivery patch live in `.issue33-runtime/`, outside the delivered source
changes. Build and all scratch use `build/`, `.t/` and `.issue33-runtime/`,
never `/tmp`.

| Check | Result | Evidence in `.issue33-runtime/` |
| --- | --- | --- |
| RX-only configure/build | PASS, no compiler warnings | `configure.log`, `build.log`, `build-final.log`, `build-test2.log` |
| New DTC qtest | 125/125 PASS, standalone and suite | `dtc-final.log`, `regression.log` |
| RX qtests and quick functional tests | 26 OK, 4 existing skips, 0 failures | `regression.log` |
| Phase 1 / four MCU parts / reset | 8 PASS | `regression.log` |
| RX113 and RX62N FPU | 2 PASS | `regression.log` |
| LCDC functional / qtest | 1 / 6 PASS | `regression.log` |
| RTC / PORT / ICU | 20 / 6 / 5 PASS | `regression.log` |
| IWDT functional / qtest | 2 / 49 PASS | `regression.log` |
| MTU / S12AD / clock / SCI | 21 / 14 / 9 / 3 PASS | `regression.log` |
| Genuine FIT PLL and HOCO, before/after reset | Both PASS, rebuilt with supplied RX toolchain | `fit-build-*.log`, `fit-*.log`, `verify-*/result.json` |
| SYSTEM checks without/with FIT ELF | Both PASS | `system.log`, `system-elf.log` |
| gdbsim U-Boot functional | PASS | `gdbsim-functional.log` |
| gdbsim-r5f562n7 / gdbsim-r5f562n8 smoke | Both listed, run 3 seconds, exit 0 | `machine-help.log`, `gdbsim-smoke.json` |
| Changed-file checkpatch | ERROR 0, WARNING 0 | `checkpatch-files.log` |
| Whitespace and patch validation | PASS | `patch-validation.log` |

The four existing skips are cdrom, qos, info_usernet and linters. Two early
new-test harness errors were corrected: qtest rejects a zero-time clock
advance, and intercepting every ICU output also disconnects its SWINT wire.
The migration test now records the actual advancing step; the CPU-output
matrix injects a request into the DTC input while the other cases exercise
real SWINTR. No model behavior or acceptance assertion was weakened to
address these harness errors. Earlier logs remain available.

The qtest matrix covers full/short layouts, all three data sizes and all
source/destination address encodings (including both fixed encodings),
normal/repeat/block updates, maximum 256-element repeats/blocks, zero-count
encodings, both repeat/block
sides, unconditional/conditional chains, terminal IRQ versus DISEL,
read-skip invalidation and cached migration, write-back skip, pending-source
priority/IER/DTCST/MSTP gates, register widths/masks/reset, short flash
address sign extension, real SCI/ADC/MTU origins, external falling-edge
requests, serial request retention, in-flight chain stop, ACT read by DMA,
invalid-mode handling, and active/pending migration.

Reproduce from the worktree root:

```sh
export TMPDIR="$PWD/.t"
export PATH="$HOME/workspace/tools/build-venv/bin:$HOME/workspace/projects/qemu/.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
mkdir -p "$TMPDIR" build
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
ninja -C build -j4
export QTEST_QEMU_BINARY="$PWD/build/qemu-system-rx"
build/tests/qtest/rx113-dtc-test
bash .issue33-runtime/regression.sh
```

The runtime regression script executes RX qtests, quick functional tests,
gdbsim's functional test, rebuilt genuine FIT PLL/HOCO probes before/after
reset, SYSTEM timing checks without/with the FIT ELF, and both gdbsim smoke
machines. FIT source ZIPs and gdbsim's pinned cache asset were copied from
Issue 31; FIT's unchanged builder verifies the pinned ZIP hash. Configure
uses its normal existing/vendored Python dependencies. No new firmware,
manual or subproject download was requested by this worker.

## D-006 residual scope and merge-gate items

The requested core transfer modes, chain execution, registers, completion
interrupts and MSTP are implemented. No DTC control bit is stored as an
unused placeholder. The following are explicit limits requiring PMO/PO
assessment, with approximate implementation-and-test cost:

| Remaining behavior | Reason and expected size |
| --- | --- |
| Table 16.8 physical execution cycles, per-access bus arbitration/CPU stalls and sub-descriptor races | The 1 ns descriptor scheduling interval is functional ordering only. A faithful implementation needs ICLK propagation, memory/peripheral wait-state accounting, CPU/DTC bus ownership and nonpreemptible vector/descriptor transactions. The current CPU/shared bus infrastructure does not expose this contract; implementing it without altering shared/RX62N behavior exceeds this issue's allowed scope. Approximately 3–5 days for infrastructure plus 1–2 days for DTC phase timing, interleaving and regression tests. Firmware timing/throughput must not be inferred from this model. |
| Big-endian memory descriptors/data | The existing RX translator always uses MO_LE and the RX113 loader accepts little-endian ELF. DTC therefore uses the same little-endian machine contract, including when the unmodeled MDE option requests big endian. Supporting the hardware's big-endian memory plus fixed-endian peripheral arrangement faithfully requires CPU/loader/option integration outside this RX113-only issue. Approximately 2–4 days plus the corresponding CPU and DTC matrix. |
| Physical deep-sleep/software-standby entry, wakeup and bus drain | MSTPA28 and DTCST work. The machine lacks full CPU/SYSTEM power-state transitions; a DTC-local flag cannot faithfully provide them. Approximately 3–5 days for power-state infrastructure and 1 day for DTC-specific checks. |
| ELC routing and request generation by unimplemented peers | A real transfer-end output and all documented vector inputs are provided. ELC and several origin peripherals remain pre-existing stubs. ELC routing alone is approximately 2–4 days; each origin peripheral requires its own model and tests. No DTC activation is fabricated for absent hardware. |

These are explicit integration/timing limits, not assertions that the
hardware omits those functions. Invalid/prohibited register sequences and
unmapped-memory failures have the diagnostic handling policy above; analog
pad qualification remains the existing PORT/peripheral contract.

## Delivery

The attempted `git add` failed creating `.git/index.lock` because `.git`
is read-only (`git-add.log`). No commit was created. The complete changes
remain in the working tree, and the importable patch is
`.issue33-runtime/issue33.patch`. Its SHA-256 is in `issue33.patch.sha256`;
`changed-files.txt` lists the exact eight files below. File-level checkpatch
has zero errors/warnings. Raw-diff checkpatch uses `--no-signoff` (this is not
a signed commit/mail patch) and has zero errors; its generic MAINTAINERS
new-file advisory remains. Apply on the base
commit above with `git apply /path/to/issue33.patch`. Reverse applicability
against this finished tree and forward application in an isolated base-file
fixture are checked in `patch-validation.log`.

* `hw/rx/meson.build`
* `hw/rx/rx113.c`
* `hw/rx/rx113-dtc.c`
* `include/hw/rx/rx113.h`
* `include/hw/rx/rx113_dtc.h`
* `tests/qtest/meson.build`
* `tests/qtest/rx113-dtc-test.c`
* `docs/rx113/issue-33-dtc-report.md`

The patch excludes AGENTS.md, runtime/manual/toolchain assets, `.t/` and
builds. Shared ICU/peripheral sources and RX62N are unchanged. No push,
merge or upstream action was performed.
