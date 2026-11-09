<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #22 — RX113 PORT/MPC Worker report

Date: 2026-11-09. Branch: `rx113/issue-22-port`.
Base: `6eeb6766b74dce731978d44da78fe67a0c2e4348`.
Scope and acceptance source: `.issue22-runtime/instructions.md`.

## Implementation and manual evidence

The PORT/MPC stubs are replaced by one RX113-specific SysBus device,
`/machine/mcu/port`, mapping 0x0008c000–0x0008c1ff. No shared peripheral,
CPU, RX62N, SYSTEM, SCI, timer, LCDC or RTC implementation is modified.
The existing CONFIG_RX113_MCU selects the new source; no additional Kconfig
symbol or public MCU state field is needed.

Manual: supplied R01UH0448EJ0120 Rev.1.20, chapters 18 and 19, pages
344–397. PDF SHA-256:
`a41f74559bd8f8aa36749519da119bcc28dcb40f6a23e6f5d403b9113fd19356`.
The same section references appear in the source and tests.

| Register / function | Evidence and modeled behavior |
|---|---|
| Port/pin availability | Table 18.1: PORT0–5, 9, A–F, J; 83 logical pins for 100-pin packages, 47 physical pins for 64-pin packages. P35 is input-only. |
| PDR, +0x00 | 18.3.1, 18.4, Tables 18.3/18.4: reset 0; direction overrides only GPIO mode. Reserved/unbonded PDR bits remain writable for firmware's required initialization; P35 direction remains input. |
| PODR, +0x20 | 18.3.2: reset 0, writable output latch, reserved bits read 0. Changing direction later uses the retained latch. |
| PIDR, +0x40 | 18.3.3: read-only resolved pad state, independent of PDR/PMR; P35 readable. ASEL disconnects digital reading (19.2.2, Table 19.22). Undefined/floating inputs deterministically read 0. |
| PMR, +0x60 | 18.3.4, 18.2, Table 19.22: reset 0; peripheral selection disconnects GPIO output. PSEL=0 is Hi-Z. |
| ODR0/1, +0x80 | 18.3.5/6, Table 18.2: reset 0; N-channel open drain releases on 1; P10/P14/PE1 also support P-channel open drain, which releases on 0. Reserved fields masked. |
| PCR, +0xc0 | 18.3.7: reset 0; pull-up supplies undriven inputs, an external low overrides it. General output mode disables the pull-up, including open-drain release. Ports 4/9 and PJ6/7 have no pull-up. |
| PSRA, 0x8c121 | 18.3.8, Figure 18.10: reset 0, bits 7:6 select PC1/0 GPIO on the PB7/6 physical pads in the 64-pin package. Peripheral selection remains PORTB.PMR/PFS. Reserved bits read 0. |
| PWPR, 0x8c11f | 19.2.1: reset 0x80. B0WI protects PFSWE using the pre-write B0WI state; PFSWE protects all PFS writes. Clearing B0WI and setting PFSWE needs separate writes. |
| PmnPFS, 0x8c140 + port*8 + bit | 19.2.2–19.2.15: reset 0, pin-specific PSEL/ISEL/ASEL masks, including no P35 PFS. ISEL and ASEL exist only on pins enumerated in those sections. |
| IRQ selectors | Table 19.1, 19.3.2/4/5: route selected digital pad levels to ICU vectors 64–71. ASEL and selected LCD/CTSU functions suppress IRQ input. Edge detection remains in the unchanged ICU. |

RX113 defines no DSCR/DSCR2 drive-strength register in chapter 18 or Table
5.1. Figure 18.10 labels DSCR in its switching diagram, but supplies no
register address or definition; no RX62N register layout is borrowed.
Reserved addresses return 0 and
ignore writes. The defined registers accept byte accesses, as in Table
5.1. Documented prohibited configurations (e.g. ODR=3, one function assigned
to several pins, changing PFS with PMR=1, changing PSRA with active
PMR/PDR/PCR) do not acquire invented hardware guarantees.

Package choice is separate from the capacity-only machine names.
Default is the full 100-pin package. Select the documented 64-pin package
with `-global rx113-port.package-pins=64`; other values are rejected.
Package-specific PFS and non-PDR reserved bits are masked. PSRA is reserved
in the 100-pin configuration. For the switched 64-pin pads, GPIO control
comes from PC0/1, observation/injection stays at physical PB6/7, and PC0/1
PIDR reflects the switched pad. PORTB.PMR still controls peripheral mode.
The switched PORTB.PIDR bits read zero, while physical-pad observation
continues through the PB6/7 GPIO lines and QOM state.

All registers and external driver state are migrated. Derived outputs are
recomputed after load. MCU reset clears control registers and releases
outputs, but preserves the board's external drivers.

## Observing and driving pins

GPIO groups on `/machine/mcu/port`:

- `pin` inputs: index `port_number * 8 + bit`, with A=10, B=11, C=12,
  D=13, E=14, F=15, J=18. P10 is index 8, PE0 is 112, PJ7 is 151.
  Level 0/1 drives the pad externally; a negative level releases that driver.
- `pin-out` outputs: resolved digital pad levels, same index.
- `pin-oe` outputs: active output-driver enables, same index. Thus a
  released open drain is distinguishable from an actively driven zero.
- `irq` outputs 0–7: selected raw digital IRQ pad levels, connected to the
  existing ICU inputs 64–71.

Read-only QOM strings `levels` and `driving` provide 19 two-digit hex bytes,
in numeric port order 0 through J, retaining holes at numeric indices 6/7/8/16/17. They
expose the same pad levels/output enables as the GPIO groups. In 64-pin
mode the physical PB6/7 lines expose the switched PC0/1 output enables;
PC0/1 has no separate physical pad. `levels` also includes the PC PIDR
aliases. Nonexistent physical pins have no drive or injected input effect.

Example qtest input injection and guest input read:

```c
qtest_set_irq_in(qts, "/machine/mcu/port", "pin", 8, 1);
g_assert_cmphex(qtest_readb(qts, 0x8c041) & 1, ==, 1);
qtest_set_irq_in(qts, "/machine/mcu/port", "pin", 8, -1);
```

The model resolves actively driven GPIO output before external input;
there is no analog contention/current simulation. Undriven digital pads
use the enabled input pull-up, otherwise deterministic zero. These are
explicit digital simulation conventions for electrically undefined states.

## Peripheral routing inventory and D-006 remaining work

All existing published peripheral lines were inspected. Interrupt requests
are not substitutes for data pins. PMR/PFS programming does not fabricate
TXD/TMO/RTCOUT/LCD waveforms. Existing SCI character-backend output stays
independent of pin selection, preserving the existing peripheral behavior. PMR enabling a peripheral emits a `-d unimp`
diagnostic; its unavailable pad output is left undriven. PFS selection,
protection, analog disconnect, GPIO disconnect and IRQ selection operate
rather than silently treating a selected peripheral as GPIO.

| Existing device / published interface | Connected / remaining, reason and estimated scale |
|---|---|
| ICU: 256 GPIO inputs and ACK; IRQ/FIR/SWI outputs | ISEL pin selectors connect to inputs 64–71; rising-edge path tested. The existing ICU treats TRG_LEVEL as active-high rather than the manual's low-level external IRQ detection and has no working NMI input path (NMISR reads zero). Full IRQ low-level/NMI/filter accuracy requires shared ICU interface/behavior work, prohibited here, and RX62N regression review: approximately 2–4 engineering days. P35 level is readable, but does not deliver NMI. |
| SCI0/1/2/5/6/12: four interrupt outputs, character backend | No RXD/TXD/SCK/CTS/RTS GPIO exists. A character backend has no bit edges to connect. Faithful pin routing needs serial framing, baud timing, receive sampling, output-enable and synchronous/flow-control interfaces plus compatibility tests for RX62N: approximately 1–2 weeks. SCI8/9 are still stubs. No shared SCI change is permitted in this issue. |
| TMR0–3: CMIA/CMIB/OVI interrupt outputs | No TMO/TMCI/TMRI GPIO exists; compare IRQs are not TMO waveforms. TCOR/toggle/clear output actions and external clock/reset sampling require shared timer interface and timing work with RX62N tests: approximately 3–5 days. CMT publishes only CMI interrupt outputs and has no PORT pad function in Table 19.1. |
| RTC: ALM/PRD/CUP and separate PRD-to-LCDC outputs; `rtcout` QOM getter | Existing PRD-to-LCDC wiring is retained. PRD is not RTCOUT. RTCOUT is currently only a sampled QOM boolean, without an output-enable/edge notification line. Correct pin output needs RTC-owned scheduling at RTCOS-selected edges and immediate enable/reset/clock-stop notifications, then MPC gating on P16/P32/PA1/PB0: approximately 2–3 days including reset/migration tests. Polling the getter would miss edges and create a second clock owner. Existing RTC behavior may not be changed here. |
| LCDC: PRD input, display RAM/segment-pattern QOM state | No COM/SEG/CAP/VL waveform GPIO is published. The visible segment bitmap is a display state, not a pin voltage. Faithful multiplexing needs common/segment phase, bias voltage, charge-pump pins and shared COM/SEG role selection: approximately 1–2 weeks plus a representation for multilevel pad voltages. Existing LCDC behavior may not be changed here. |
| SYSTEM: subclock running/stopped boolean output | Existing RTC clock wiring is retained. This is oscillator availability, not CLKOUT edges. Correct CLKOUT needs the clock tree/rate propagation deferred by D-005 and clock-divider scheduling: several days after that prerequisite. |
| MTU/POE, S12AD, DAC, CTSU, USB, CAC, other Table 19.1 functions | No usable pad-signal implementation exists to connect. Faithful pin behavior requires the respective peripheral models, including analog interfaces for ADC/DAC/comparators and multilevel LCD/touch behavior. This spans multiple peripheral issues, several weeks collectively; it cannot be achieved by storing PSEL or wiring unrelated interrupt lines. |

These are explicit D-006 residuals for PMO/PO review, not claims of complete
peripheral-pin compatibility. Estimates are engineering scale, not promises.
The core GPIO/register/protection acceptance scope is implemented.

## Validation

The continuation re-read the instruction, D-006, skeleton report and
manual chapters 18/19. It retained the existing implementation, corrected
the switched PORTB.PIDR behavior against Figure 18.10, and extended the
64-pin test accordingly. The MPC case additionally checks protected TXD0
selection on P10 and GPIO output disconnection/reconnection through PMR.

Final results (all repeated with `build/qemu-system-rx`):

| Check | Result | Evidence under `.issue22-runtime/` |
|---|---|---|
| RX-only build | PASS | `build-resume.log`, `build-current-check.log` |
| PORT/MPC qtest | 6/6 PASS: registers/reset, input/output, open drain/pull-up, MPC protection/selection, IRQ mux, 64-pin switching | `meson-testlog-final.txt` |
| RX qtest suite | 12 OK / 2 skipped / 0 failed; includes 5 ICU, 6 LCDC and 20 RTC cases | `regression-final.log` |
| RX functional suite | 7 OK / 2 skipped / 0 failed; includes 8 Phase 1/part boot/reset, 2 FPU and 1 LCDC cases | `regression-final.log` |
| Genuine FIT PLL and HOCO | PASS, SCI0 main marker before and after reset | `fit-build-*-final.log`, `fit-*-final.log`, `verify-*-final/result.json` |
| SYSTEM protection/oscillator probes | PASS, without image and with fresh PLL ELF | `system-final.log`, `system-elf-final.log` |
| Both gdbsim machines | Listed; running after 3 s; SIGTERM exit 0 | `machines-final.log`, `gdbsim-r5f562n*-final.log`, `validation-final.json` |
| checkpatch, all seven delivery files | 0 errors / 0 warnings | `checkpatch-files-final.log`, `checkpatch-report-final.log` |
| Whole import patch | 0 errors / 1 generic MAINTAINERS warning for added files | `checkpatch-patch-final.log` |
| Patch import / whitespace | PASS; exact reconstruction and reverse-apply checks | `patch-validation-final.log` |

The four existing skips are cdrom, qos, info_usernet and linters.
The gdbsim checks are idle-machine smoke tests, not kernel boots.
`tested-binary.sha256` identifies the binary used for the acceptance run.

Reproduction from this worktree:

```sh
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
export TMPDIR="$PWD/.t"
export PYTHONPATH="$PWD/.issue22-runtime/test-env:$PWD/python:$PWD/tests/functional"
make -C build -j8
meson test -C build --no-rebuild --suite qtest-rx --suite func-rx \
  --print-errorlogs -j2
export PATH="$HOME/workspace/projects/qemu/.issue6-worktree/.issue6-runtime/toolchain/bin:$PATH"
python3 tests/functional/rx/fit_boot/build.py .issue22-runtime/probe-pll
python3 tests/functional/rx/fit_boot/build.py .issue22-runtime/probe-hoco --hoco
python3 .issue22-runtime/run-final.py
scripts/checkpatch.pl --no-tree -f hw/rx/rx113-port.c \
  include/hw/rx/rx113_port.h hw/rx/rx113.c hw/rx/meson.build \
  tests/qtest/rx113-port-test.c tests/qtest/meson.build \
  docs/rx113/issue-22-port-report.md
```

The runtime runner records every command and exit status in
`validation-final.json`; it runs both FIT boot/reset probes, both SYSTEM
probes and the two idle gdbsim smoke checks as well as the Meson suites.
The pre-existing verified `r_bsp.zip` archives are reused and hash-checked
by `build.py`; the probe ELFs are freshly rebuilt. No downloads are needed.

The sandbox makes `/var/tmp` read-only. The untracked
`.issue22-runtime/test-env/sitecustomize.py` changes only QEMUMachine's
default scratch location to TMPDIR for the test processes; product and
shared test framework sources are unchanged. Interrupted Ninja build-log
recovery caused additional rebuilding; all builds remain RX-only
(`TARGET_DIRS=rx-softmmu`). The former preview binary/logs are retained
for history but are not the final validation evidence.

## Delivery

No push, merge, PR, upstream submission or network download is performed.
The provided FIT/toolchain dependencies and pinned subprojects are reused.
Staging failed because `.git/index.lock` cannot be created on the
read-only Git directory (`git-stage-final.log`), so no commit is created.
All changes remain in the working tree on `rx113/issue-22-port`.

Delivery files:

- `hw/rx/rx113-port.c`
- `include/hw/rx/rx113_port.h`
- `hw/rx/rx113.c`
- `hw/rx/meson.build`
- `tests/qtest/rx113-port-test.c`
- `tests/qtest/meson.build`
- `docs/rx113/issue-22-port-report.md`

The import artifact is `.issue22-runtime/issue-22-port.patch`; the same
file list is saved in `.issue22-runtime/changed-files.txt`, and its SHA-256
is in `.issue22-runtime/SHA256SUMS`. It includes the seven files above and
excludes AGENTS.md, binaries, runtime scratch, supplied PDF and toolchain.
The patch is checked against copies of the three tracked base files from
the base commit, applied there, and every resulting file compared byte
for byte with the final source. Reverse-apply checking also covers all
seven files against the final working tree (`patch-validation-final.log`).

In a writable checkout at the base commit, PMO can import without merging:

```sh
git apply --check /path/to/issue-22-port.patch
git apply /path/to/issue-22-port.patch
```

PMO/PO still owns the merge gate, including explicit review of the D-006
residuals above. No claim of complete peripheral waveform routing is made.
