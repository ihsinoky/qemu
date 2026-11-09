<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue 16: Add the remaining RX113 part numbers

## Capacity evidence

Source: the supplied RX113 Group User's Manual: Hardware,
R01UH0448EJ0120 Rev.1.20 (September 7, 2020), Table 1.3, page 47.
The RAM column uses merged cells: R5F51137 shares the 64-Kbyte cell
with R5F51138; R5F51135 shares the 32-Kbyte cell with R5F51136.
The original PDF table was inspected visually, including these cell
boundaries. Figure 1.1, page 48, independently lists the same capacities.

| Part | Code flash | RAM | E2 DataFlash | Capacity source | Code flash range |
| --- | --- | --- | --- | --- | --- |
| R5F51135 | 128 KiB | 32 KiB | 8 KiB | Table 1.3, all three values | 0xfffe0000-0xffffffff |
| R5F51136 | 256 KiB | 32 KiB | 8 KiB | Table 1.3, all three values | 0xfffc0000-0xffffffff |
| R5F51137 | 384 KiB | 64 KiB | 8 KiB | Table 1.3, all three values | 0xfffa0000-0xffffffff |
| R5F51138 (existing) | 512 KiB | 64 KiB | 8 KiB | Table 1.3, all three values | 0xfff80000-0xffffffff |

Table 42.2, page 1318, confirms every code flash range. RAM starts at
0x00000000 and DataFlash at 0x00100000 as in the existing model.

Tables 1.1 and 1.2 describe peripheral differences by package, rather
than by these memory-size part numbers. Table 1.3 gives each of the four
parts the same package choices within each temperature grade. The new
machines retain the existing model's peripheral configuration (including
its package-independent superset); no package selection is introduced.
Section 42.4.9 also describes a memory-plane boundary restriction for
program/erase commands on 384/512-Kbyte products. This follows flash
capacity and does not affect this model, which maps ROM and does not
implement the flash programming controller. No additional difference
requiring a change to modeled peripheral behavior was found.

## Implementation

* `rx113.h` and `rx113.c`: add `r5f51135-mcu`, `r5f51136-mcu` and
  `r5f51137-mcu`, with Table 1.3 references beside the capacity settings.
  All four MCU types use the same RX113 realization path. Code flash
  continues to end at 0xffffffff, with its base calculated from capacity.
* `rx113-machine.c`: introduce an abstract RX113 machine parent with
  common initialization/reset behavior and a per-class MCU type. Register
  `rx113-r5f51135`, `rx113-r5f51136`, `rx113-r5f51137` and the existing
  `rx113-r5f51138` as concrete machines. ELF loading, raw flash loading,
  reset-vector handling and the RX113 CPU type retain their behavior.
* `test_rx113.py`: reuse the existing ELF/raw flash boot tests in three
  subclasses. Each image starts at that part's flash base, specifies an
  ELF entry of zero where applicable, and sets the hardware reset vector.
  Check SCI0 output before/after reset, the running state and final PC,
  plus code flash, RAM and DataFlash ranges from `info mtree`.
  The existing R5F51138 test expectations are preserved.
* Shared device/CPU models and RX62N files are unchanged.

## Verification

All required checks pass (2026-11-09):

| Check | Result | Local evidence under `.issue16-runtime/` |
| --- | --- | --- |
| Configure and build, `--target-list=rx-softmmu --disable-docs` | PASS | `configure.log`, `build.log` |
| `qemu-system-rx -M help` | Four RX113 parts and both gdbsim machines listed | `machines.log` |
| `test_rx113.py` (Phase 1 and added parts) | 8/8 PASS, ELF and raw boot/reset on all four parts | `functional.log` |
| `test_rx113_fpu.py` | 2/2 PASS, including RX62N comparison | `functional.log` |
| `test_rx113_lcdc.py` | 1/1 PASS | `functional.log` |
| Genuine FIT probe, PLL and HOCO, R5F51138 | SCI0 main marker before and after reset, both PASS | `verify-pll/result.json`, `verify-hoco/result.json` |
| SYSTEM exact-delay/protection regressions, without and with FIT ELF | Both PASS | `system-flash.log`, `system-elf.log` |
| Existing gdbsim U-Boot smoke | PASS, expected U-Boot console banner | `gdbsim.log` |
| Entire `qtest-rx` suite | 10 PASS, 2 SKIP, 0 failures/timeouts | `qtest-short-path.log` |
| Checkpatch on all five changed/new files | 0 errors, 0 warnings | `checkpatch-final.log` |

The RX qtest suite includes `rx-icu-test` (5 subtests) and
`rx113-lcdc-test` (6 subtests), both passing. The two skips are `cdrom-test`
and `qos-test`, which have no applicable RX cases. The first suite run
hit a `netdev-socket` timeout because the unnormalized `build/../` plus
the long temporary path exceeded the Unix socket path limit. Repeating
with a short worktree-local temporary path passed, without code changes.
Use an absolute, normalized `TMPDIR="$PWD/.t"` from the source root.

Functional tests were run with Meson:

```sh
meson test -C build --no-rebuild --print-errorlogs -j2 \
  func-rx-rx113 func-rx-rx113_fpu func-rx-rx113_lcdc
meson test -C build --no-rebuild --print-errorlogs -j2 --suite qtest-rx
```

The existing gdbsim `test_uboot` was run separately. Its machine-discovery
helper hardcodes `/var/tmp`, which is read-only in this environment.
The local launcher `.issue16-runtime/run-gdbsim.py` instead supplies the
machine names from this binary's `-M help`, then executes the unchanged
test with its normal console assertion and cached U-Boot asset.
The unrelated flaky Linux/sash test remains skipped by its existing
decorator. No shared test or Python helper was modified.

The initial Make invocation printed a Ninja jobserver descriptor warning
and error during its build-system setup, then continued and completed all
2425 build steps with exit status zero.

## Environment and remaining items

Build directory: `build/`; temporary files and logs: `.issue16-runtime/`.
Only `rx-softmmu` is configured, with documentation disabled. Meson/ninja
come from `~/workspace/tools/build-venv/bin`.
FIT probes are rebuilt with the supplied Issue 6 `rx-elf-*` toolchain.
Their pinned, SHA-256-checked `r_bsp.zip` is copied from the Issue 6
worktree. The gdbsim U-Boot asset is copied from the existing local cache
into a worktree-local test cache. No new source or test-asset downloads
are needed.

Existing limitations, including package-specific pins/peripherals,
flash programming, dynamic peripheral clocks and module-stop gating,
remain outside this issue.

The checked-out branch is `rx113/issue-16-parts`. The environment mounts
`.git` read-only: staging fails with `Unable to create .../.git/index.lock:
Read-only file system`. Therefore no local commits can be created in this
session. Changes remain in the working tree, with an importable patch at
`.issue16-runtime/issue-16-parts.patch`. Apply the patch in a writable clone
based on `abf1442e10`, review it and commit with a subsystem-prefixed message
such as `hw/rx: Add the remaining RX113 part numbers`. Do not apply it again
over this already modified worktree. No push, merge or upstream submission
is performed.
