<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# RX113 genuine FIT R_BSP startup probe

This optional, standalone test downloads Renesas R_BSP v7.71 at a pinned
revision and checks its SHA-256. The third-party startup sources are not
vendored or edited. The generated `platform.h` selects `generic_rx113`;
`r_bsp_config.h` comes from that board's reference configuration with
`BSP_CFG_IO_LIB_ENABLE=0`. The MCU memory-size selection is 8 (R5F51138).
The default clock configuration uses a 16 MHz main crystal, a 32 MHz PLL
system clock and the USB-dedicated PLL. The sub-clock and LCD are disabled.
`--hoco` additionally enables HOCO, selects it as the system clock and
selects the system clock for USB. Both configurations keep main oscillation
enabled, as in the supplied reference configuration.

The application and linker script are test harness code, not FIT drivers.
R_BSP's reset assembly initializes the stacks, and its C reset code performs
clock setup, data copying, BSS clearing and board setup before calling
`main()`. The application checks a data canary and zero-initialized BSS,
clears SCI0's module-stop bit, configures SCI0 and prints
`RX113 FIT main reached` using polling. A second boot after QMP reset checks
that startup reinitializes the BSS marker. No libc is linked. Empty local
`stdio.h` and `assert.h` accommodate unconditional includes; neither API is
used by the compiled code. GCC's `libgcc` supplies integer division support.
The unused `r_bsp_locking.c` API implementation is excluded because it
requires the Renesas GCC extension `__builtin_rx_xchg`, absent from upstream
GCC 14.2.0. Startup's lock-array initialization remains linked unchanged.

Requires Python 3, network access on first build, RX GCC/binutils named
`rx-elf-*` (including `libgcc`), and an rx-softmmu QEMU build with its
Python venv, which provides the `qemu.qmp` dependency for `verify.py`.
The toolchain build and exact versions are documented in `docs/rx113/issue-6-fit-boot-report.md`.
Run from the QEMU source root; keep all scratch in the worktree:

```sh
export PATH="$PWD/.issue6-runtime/toolchain/bin:$PATH"
export PYTHONPATH="$PWD/python"
mkdir -p .issue6-runtime/probe
# An existing verified r_bsp.zip may be copied into probe to avoid a download.
python3 tests/functional/rx/fit_boot/build.py .issue6-runtime/probe
./build/pyvenv/bin/python3 tests/functional/rx/fit_boot/verify.py build/qemu-system-rx \
  .issue6-runtime/probe/boot.elf .issue6-runtime/verify-pll
python3 tests/functional/rx/fit_boot/build.py .issue6-runtime/probe-hoco --hoco
./build/pyvenv/bin/python3 tests/functional/rx/fit_boot/verify.py build/qemu-system-rx \
  .issue6-runtime/probe-hoco/boot.elf .issue6-runtime/verify-hoco
# Also check protected writes, readback and oscillator flag transitions.
timeout 30s python3 tests/functional/rx/fit_boot/system.py \
  build/qemu-system-rx .issue6-runtime/system-test
timeout 30s python3 tests/functional/rx/fit_boot/system.py \
  build/qemu-system-rx .issue6-runtime/system-test-elf \
  --elf .issue6-runtime/probe/boot.elf
```

`verify.py` saves `result.json` (SCI0 text, CPU registers and SYSTEM reads)
and `execution.log` (translated-block disassembly). It requires the expected
SCI0 string before and after reset. To capture the original SYSTEM-stub
stall with an unmodified Phase 1 binary, use the same ELF and add
`--expect-stall`; inspect the saved PC and disassembly against `boot.dis`
and `boot.nm` to identify the wait loop. This mode only checks lack of SCI
output and records evidence; it does not classify an arbitrary hang.

Clock stabilization is instantaneous in the minimal QEMU model. Clock
selection readback does not change the fixed 32 MHz peripheral input, and
module-stop readback does not gate the existing peripheral models. These
checks establish startup control flow for the selected configurations,
not clock timing, electrical accuracy or general FIT compatibility.
