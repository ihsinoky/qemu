<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #6 Worker report: FIT startup on RX113

Date: 2026-11-05. Base: `rx113/dev` at
`712de4e8bd`. Branch: `rx113/issue-6-fit-boot`.
Work directory: `/home/hatch/workspace/projects/qemu/.issue6-worktree`.
This continuation resumed the existing clone, sources, test drafts and
partially completed builds after an environment restart.

Result: **PASS**. Genuine R_BSP startup reaches `main()` and transmits the
expected SCI0 string in PLL and HOCO configurations, including after reset.
A minimal RX113-only SYSTEM implementation resolves the observed HCOVF wait.

## Firmware provenance and scope

This probe uses **genuine Renesas FIT R_BSP v7.71**, not a handwritten
reproduction of its startup sequence. Source:
[Renesas rx-driver-package](https://github.com/renesas/rx-driver-package/tree/bcc409d331b0bff68d4b09bf6fc41376f75d42bc),
package `FITModules/r_bsp_v7.71.zip` at that immutable revision.
Its source headers identify BSD-3-Clause licensing and
Renesas copyright notices. The package and its sources remain local build
inputs; no third-party files or generated ELF are added to the commits.
The committed builder downloads the pinned package and checks SHA-256:

```text
f00271622c11722e3de7bb73c7c426cd0f89a2390f880d909368187891971fe0
```

The executable R_BSP sources are unmodified: reset assembly, `resetprg.c`,
RX113 `mcu_clocks.c`, interrupt/register-protection/runtime support, vector table and
`generic_rx113/hwsetup.c`. The test supplies a board-selection `platform.h`
and copies the board reference configuration to `r_bsp_config.h`, disabling
I/O library support. It uses memory-size selection 8 (R5F51138), little-endian
data, RX100 instruction selection and no FPU. An empty test-only `stdio.h`
satisfies an unconditional include; likewise an empty `assert.h` is provided
because `fsp_common_api.h` includes it but the compiled sources contain no
`assert()` calls. No libc functions are linked. `libgcc` supplies the integer
division helper `__udivdi3`. Neither test header supplies runtime behavior.
The unused `r_bsp_locking.c` API implementation is excluded: it uses the
Renesas extension `__builtin_rx_xchg`, absent in upstream GCC 14.2.0.
Startup's `bsp_ram_initialize()` and the `g_bsp_Locks` array remain linked
unchanged. This does not establish compatibility of the other FIT APIs.
All 647 extracted package files were compared byte-for-byte with the pinned
ZIP and were unchanged.
The linker script and SCI0 application are custom test harness code, not a
Renesas SCI FIT driver and not a product application.

Default configuration: main oscillator 16 MHz, PLL 32 MHz, USB-dedicated PLL
enabled, clock divisors one, HOCO/sub-clock/LCD disabled. `--hoco` enables
HOCO and selects it as the system clock, with the system clock selected for
USB; the reference main-oscillator enable remains set. OFS1 is `0xffffffff`
in both configurations (HOCO disabled at reset).

R_BSP initializes USP/ISP, INTB, oscillator/clock controls, C data and BSS,
interrupt callbacks, protection support and board setup before calling
`main()`. The application checks a ROM-to-RAM data canary and BSS marker,
unlocks PRC1, clears `MSTPCRB.MSTPB31` for SCI0, relocks protection and
prints `RX113 FIT main reached\n` with polling. On QMP reset the marker
must be cleared by R_BSP before the string can be emitted again.

## Toolchain and resumed builds

GNU binutils 2.43.1 and GCC 14.2.0, target `rx-elf`, built from public GNU
source archives already downloaded by the previous run:

- <https://ftp.gnu.org/gnu/binutils/binutils-2.43.1.tar.xz>
- <https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz>

SHA-256, in the same order:

```text
13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd
a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9
```

These are GPL toolchain sources, with component licensing supplied in their
`COPYING*` files; no registration or proprietary toolchain is required.
GCC uses the local prerequisite sources GMP 6.2.1, MPFR 4.1.0 and MPC 1.2.1.
For a fresh build, GCC's `contrib/download_prerequisites --no-isl` fetches
the pinned prerequisites. Configure commands (from the worktree root):

```sh
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
mkdir -p .t build/rx-binutils build/rx-gcc
export TMPDIR="$PWD/.t"
(cd build/rx-binutils && ../../.issue6-runtime/binutils-2.43.1/configure \
  --target=rx-elf --prefix="$PWD/../../.issue6-runtime/toolchain" \
  --disable-nls --disable-werror --disable-gdb --disable-sim)
make -C build/rx-binutils -j8 MAKEINFO=true
make -C build/rx-binutils MAKEINFO=true install
export PATH="$PWD/.issue6-runtime/toolchain/bin:$PATH"
(cd build/rx-gcc && ../../.issue6-runtime/gcc-14.2.0/configure \
  --target=rx-elf --prefix="$PWD/../../.issue6-runtime/toolchain" \
  --enable-languages=c --without-headers --disable-bootstrap --disable-nls \
  --disable-multilib --disable-shared --disable-threads --disable-libssp \
  --disable-libquadmath --disable-libgomp)
make -C build/rx-gcc -j8 all-gcc
# On this VM only, cp -p fails to preserve ownership. Edit the generated
# build/rx-gcc/gcc/Makefile, replacing "cp -p " with
# "cp --preserve=mode,timestamps ", then rerun all-gcc.
TAR_OPTIONS=--no-same-owner make -C build/rx-gcc install-gcc
make -C build/rx-gcc -j8 all-target-libgcc
TAR_OPTIONS=--no-same-owner make -C build/rx-gcc install-target-libgcc
```

At resume, neither toolchain install nor the QEMU build was complete.
Incremental builds reused existing objects. Binutils initially failed due
to absent `makeinfo`; `MAKEINFO=true` skips documentation. A zero-byte
`unwind-ia64.o` left by the restart then caused an undefined `unw_decode`
reference; removing that incomplete object fixed it. During inspection,
an actively compiling GCC `xstrdup.o` was also removed; its incremental
build was restarted and rebuilt the missing object. No clean rebuild was
needed. The QEMU build resumed from approximately 2181/2422 targets and
completed. Only `rx-softmmu` was configured; build and temporary files stayed
inside the clone, with `TMPDIR` set to `.t`.
GCC then hit ownership-preservation failures in `cp -p` and `tar xpf` on
this VM. Only the generated GCC Makefile copy flags were adjusted as above;
`TAR_OPTIONS=--no-same-owner` fixed installation. Toolchain sources were not
modified. The compiler and libgcc were ready by about 08:08 UTC, within the
45-minute continuation allowance (resume started 07:36 UTC).

## Execution and register diagnosis

On the unmodified Phase 1 binary, the PLL-configured ELF produced no SCI0
output. QMP stop and `info registers` captured **PC `0xfff804a3`**, R7 =
`0x00080000`, with HOCOCR (`0x00080036`) and OSCOVFSR (`0x0008003c`) both
zero. Disassembly showed:

```text
fff804a0: mov.b 60[r7], r10
fff804a3: tst #8, r10
fff804a7: beq.w fff805c1
fff805c1: nop
fff805c2: bra.w fff804a0
```

`rx-elf-addr2line -fe .issue6-runtime/probe/boot.elf 0xfff804a3`
identifies `clock_source_select`, `mcu_clocks.c:400`: the wait for
`OSCOVFSR.HCOVF == 1`. Even with HOCO disabled by the configuration, a zero
HOCOCR from the stub falsely indicates that it is operating; FIT therefore
waits for stabilization before proceeding. The original static suspicion
about OSCOVFSR was correct, but the first observed wait is **HCOVF**, before
the main-oscillator/PLL waits.

Only `hw/rx/rx113.c` changes model behavior. It replaces the two SYSTEM
stubs with a small RX113-only SysBus child. Unknown registers retain the
zero-read/discard-write behavior with `LOG_UNIMP`. The state has reset and
VMState support; migration itself was not exercised.

Hardware authority: **R01UH0448EJ0120 Rev.1.20**, reused from
`../.issue1-runtime/rx113.pdf`, SHA-256
`a41f74559bd8f8aa36749519da119bcc28dcb40f6a23e6f5d403b9113fd19356`.
Each behavior group cites the corresponding manual section in code.

| Register / address | Manual section | Change and relation to boot |
|---|---|---|
| HOCOCR `0x80036`, OFS1 `0xffffff88` | 7.2.2, 9.2.11 | HCSTP reset follows OFS1.HOCOEN (1 for the probe); read after ROM restoration in reset exit. Removes the false indication that HOCO is running. |
| OSCOVFSR `0x8003c` | 9.2.12, pp.175–176 | Read-only MOOVF/PLOVF/HCOVF/UPLOVF follow oscillator enables; PLL flags additionally require main oscillation. Stability is instantaneous. Resolves the observed HCOVF wait and permits subsequent oscillator waits to complete. |
| PRCR `0x803fe` | 12.1.1, Table 12.1 | Accept only keyed 16-bit writes with PRKEY=A5; read back PRC0–3 without the key. Gate the implemented writable registers by PRC0/1/2. |
| SCKCR `0x80020`, SCKCR3 `0x80026`, PLLCR/2 `0x80028/2a`, UPLLCR/2 `0x8002c/2e` | 9.2.1–6 | Reset values, masks and readback; PLL stop bits drive status. Clock selections do not alter device clocks. |
| MOSCCR/SOSCCR/LOCOCR `0x80032/33/34` | 9.2.7–10 | Retain stop bits and reset values. Main stop controls MOOVF; SOSCCR readback satisfies FIT's sub-clock stop check. |
| MOSCWTCR `0x800a2`, HOCOWTCR `0x800a5`, MOFCR `0x8c293` | 9.2.17–18, 9.2.20 | Retain masked wait/source/drive settings under the documented protection bits. No wait-time or analog simulation. |
| MSTPCRB `0x80014` | 11.2.3 | Reset to all ones, preserve reserved ones, protect writes by PRC1, retain module-stop bits. The application reads SCI0's bit 31 back as zero before enabling SCI0. Module-stop does not gate peripheral models. |

After the change, both configurations printed `RX113 FIT main reached` on
SCI0, then printed it again after QMP `system_reset`/`cont`. The application
can only print after checking the initialized data value and zero BSS marker.
The post-boot register observations were:

| Value | PLL configuration | HOCO configuration |
|---|---|---|
| SCKCR / SCKCR3 | `0x00000000` / `0x0400` | `0x00000000` / `0x0100` |
| OSCOVFSR | `0x25` (main, PLL, USB PLL) | `0x09` (main, HOCO) |
| HOCOCR / MOSCCR / SOSCCR | `1` / `0` / `1` | `0` / `0` / `1` |
| MSTPCRB / PRCR | `0x7fffffff` / `0` | `0x7fffffff` / `0` |

The remaining unimplemented accesses in the PLL run were RSTSR1 at
`0x8c291`, RTC.RCR3 at `0x8c426` and PORT direction registers during board
setup. They did not block these configurations; this result does not verify
RTC, reset-cause reporting or pin behavior.

## Reproduction

See `tests/functional/rx/fit_boot/README.md` for the standalone probe
commands. All commands below run from the work directory above.

```sh
export PATH="$HOME/workspace/tools/build-venv/bin:$PWD/.issue6-runtime/toolchain/bin:$PATH"
export TMPDIR="$PWD/.t"
export PYTHONPATH="$PWD/python"
# Existing configure/subproject state was reused in this continuation.
make -C build -j8
python3 tests/functional/rx/fit_boot/build.py .issue6-runtime/probe
./build/pyvenv/bin/python3 tests/functional/rx/fit_boot/verify.py build/qemu-system-rx \
  .issue6-runtime/probe/boot.elf .issue6-runtime/verify-pll
python3 tests/functional/rx/fit_boot/build.py .issue6-runtime/probe-hoco --hoco
./build/pyvenv/bin/python3 tests/functional/rx/fit_boot/verify.py build/qemu-system-rx \
  .issue6-runtime/probe-hoco/boot.elf .issue6-runtime/verify-hoco
timeout 30s python3 tests/functional/rx/fit_boot/system.py \
  build/qemu-system-rx .issue6-runtime/system-test
timeout 30s python3 tests/functional/rx/fit_boot/system.py \
  build/qemu-system-rx .issue6-runtime/system-test-elf \
  --elf .issue6-runtime/probe/boot.elf
export QEMU_TEST_QEMU_BINARY="$PWD/build/qemu-system-rx"
export PYTHONPATH="$PWD/python:$PWD/tests/functional"
export MESON_BUILD_ROOT="$PWD/build"
./build/pyvenv/bin/python3 tests/functional/rx/test_rx113.py
make -C build check-qtest-rx -j4
```

The before-fix binary is retained at
`.issue6-runtime/qemu-system-rx-before`; run `verify.py` with that binary,
the same PLL ELF and `--expect-stall` to reproduce the original wait.
The builder retains `boot.dis`, `boot.nm` and `boot.map` beside each ELF.

## Verification

- Resumed QEMU build and build with the SYSTEM change: **PASS**.
- Original Phase 1 binary with the PLL probe: expected stall reproduced at
  HCOVF, PC `0xfff804a3`; no SCI output. Evidence in `verify-before/`.
- Genuine FIT PLL probe: **PASS**, expected SCI0 string before/after reset.
  Evidence in `verify-pll/result.json` and `execution.log`.
- Genuine FIT HOCO probe: **PASS**, expected SCI0 string before/after reset.
  Evidence in `verify-hoco/result.json` and `execution.log`.
- Direct SYSTEM checks: **PASS** with and without the probe ELF. Covers
  HOCO reset state derived from loaded OFS1, PRCR key and access width,
  protected writes, SCKCR/SCKCR3 readback, oscillator start/stop dependencies,
  read-only status and MSTPCRB retention/protection. Evidence in
  `system-test.log` and `system-test-elf.log`.
- Phase 1 functional tests: **PASS**, both ELF/raw subtests, before and
  after the change. Evidence in `phase1-before.log` / `phase1-after.log`.
- gdbsim smoke: **PASS** on both `gdbsim-r5f562n7` and `gdbsim-r5f562n8`,
  before and after the change: each stays running for three seconds and
  terminates with SIGTERM and exit 0. This is an idle-machine smoke, not a
  Linux boot. Evidence in `gdbsim-before.log` / `gdbsim-after.log`.
- RX qtest regression: **PASS**, 9 programs successful, 2 skipped, 0 failed;
  includes the 5 `rx-icu-test` subtests. Evidence in `qtest-after.log`.
- Checkpatch on all eight changed/new files: **0 errors, 0 warnings**.
  `git diff --check`: **PASS**. Combined patch check using
  `scripts/checkpatch.pl --no-tree --no-signoff` has **0 errors, 1 warning**:
  the usual new-file MAINTAINERS advisory. This fork-local series does not
  change upstream maintainer assignments. Sign-off checking is disabled for
  the raw diff; no human DCO certification is claimed. The initial raw-diff
  run without that option reported the missing sign-off, not a code defect.

All named evidence paths are under `.issue6-runtime/`; build logs include
`build-base-resume.log`, `build-system.log`, `gcc-resume2.log`,
`gcc-resume3.log`, `gcc-install-final.log`, `libgcc-build.log` and
`binutils-resume3.log`. The earlier failed attempts are retained, including
the initial probe draft's `-mno-fpu` typo (corrected to `-mnofpu`), missing
platform-header guard, and linker symbol/section setup. Final `probe-build.log`
and `probe-hoco-build.log` contain only the built ELF path.

To repeat the standalone gdbsim smoke:

```sh
for machine in gdbsim-r5f562n7 gdbsim-r5f562n8; do
  ./build/qemu-system-rx -M "$machine" -display none \
    -monitor none -serial none &
  rx113_smoke_pid=$!
  sleep 3
  kill -0 "$rx113_smoke_pid" && kill -TERM "$rx113_smoke_pid"
  wait "$rx113_smoke_pid"
done
```


## Limits and remaining work

No shared ICU/SCI/CMT/TMR/CPU or RX62N/gdbsim source was modified.
The supported result is reset-to-main startup control flow for the two
specified FIT configurations and custom polling SCI0 output. Peripheral
input remains fixed at 32 MHz. Clock validity, stabilization time, voltage,
module clock gating, unsupported SYSTEM registers, RTC/PORT behavior,
sub-clock and LCD operation, production FIT configuration, interrupt-driven
FIT drivers and FPU restrictions remain outside this verification.
The executed-block logs contain no floating-point or FPSW instructions.
Other FIT APIs can still require Renesas compiler extensions. A full clock
tree, realistic stabilization delays and module gating need separate design
work. No shared-model change or unresolved blocker was needed for this boot
probe.

## Delivery

No push, merge, GitHub PR, or upstream submission was performed. Commits
remain in the local shared clone. PMO/PO owns import, push and Draft PR
creation against `rx113/dev`. The root checkout is untouched.

```sh
# From the original repository in a session where its .git is writable:
git fetch ./.issue6-worktree \
  rx113/issue-6-fit-boot:rx113/issue-6-fit-boot
# Or transfer the verified incremental bundle (base 712de4e8bd required):
git fetch ./.issue6-worktree/.issue6-runtime/issue-6-fit-boot.bundle \
  rx113/issue-6-fit-boot:rx113/issue-6-fit-boot
```

Delivery commits, in order:

1. `ee1807e79001db7a2ef1e0ba29eeeef2f9a2065e` —
   `hw/rx: Model RX113 startup clock and protection registers`.
2. `c165a50fd9437dd6bfc307a4687c9868c4ba828d` —
   `tests/functional: Verify RX113 boot with Renesas FIT R_BSP`.
3. Branch tip — `docs/rx113: Record Issue 6 FIT boot validation` (this report).

The bundle contains these three commits and requires base `712de4e8bd`.
It is verified in the clone before delivery and can be transferred without
the shared clone's object-store dependency. All work completed within the
three-hour continuation timebox.

List exact delivery commits with:
`git -C .issue6-worktree log --reverse --oneline 712de4e8bd..rx113/issue-6-fit-boot`.
