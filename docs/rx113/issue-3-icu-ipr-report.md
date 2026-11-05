<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #3 Worker report: configurable RX ICU IPR count

Date: 2026-11-05. Base: `rx113/dev` at
`469bc23912f27abb5fa7b43c259d887a091e11ed`.
Branch: `rx113/issue-3-icu-ipr`.

## Implementation and compatibility

- Property: `rx-icu.ipr-count`, unsigned 32-bit, default **142**. Realize
  accepts **1 through 256**. The upper limit follows the byte-sized IPR
  map and the MMIO boundary at DMRSR (`0x400`).
- Storage: a zero-initialized allocation of exactly `ipr-count` bytes,
  released by instance finalization. This keeps allocation and migration
  proportional to the configuration and avoids relying on structure layout.
- Byte MMIO at ICU offset `0x300 + n` accesses IPR `n` for
  `0 <= n < ipr-count`. Read/write masks remain four bits; invalid access
  sizes retain their previous handling. DMRSR begins at `0x400`, including
  with 256 IPRs.
- **Legacy compatibility exception:** the original 142-byte array had a
  144-byte MMIO decode. Bytes 142/143 overlapped `dmasr[0/1]` in the existing
  structure. Default-count accesses to `0x38e/0x38f` explicitly preserve
  that alias, including masked writes through the IPR address and masked
  reads of full-byte DMRSR values. Simply rejecting those addresses or
  expanding the array would change guest-observable RX62N behavior.
- The RX62N map also contains 142 through 145. Its priority reads previously
  overlapped all four DMRSR bytes. At the default count, priority lookup
  explicitly preserves these reads, including their full-byte values.
- Other out-of-range map entries, or a missing map element, return priority
  zero without indexing outside either allocation. RX62N's `0xff` entries
  denote unassigned vectors; their old reads outside the IPR/DMRSR arrays
  were undefined and are now safe. No new realize-time map rejection is
  imposed on the existing RX62N tables. With count 256, all byte map values,
  including 255, index genuine IPR storage. IRQ request/ACK selection policy
  otherwise remains unchanged; zero priority does not introduce an extra
  IRQ suppression rule.
- Production changes are confined to `hw/intc/rx_icu.c` and its header.
  `hw/rx/rx62n.c` and `hw/rx/rx-gdbsim.c` are unchanged.

## VMState decision

The ICU stream is version **2**, minimum version **2**. It records
`ipr_count` with `VMSTATE_UINT32_EQUAL` before a variable-length IPR array.
A destination with a different property value rejects the stream before
loading the array, so incoming state cannot resize or overrun the allocation.
All other serialized fields retain their previous order and representation.
Default streams now contain 142 IPR bytes plus the count; extended streams
contain 256 IPR bytes plus the count. Legacy aliases remain in the separately
serialized DMRSR bytes.

Version 1 streams are deliberately rejected rather than silently interpreting
142 saved bytes as a differently sized array. This fork has no published
migration ABI, and the issue expressly permits a practical fork-local decision.
A version bump with an explicit rejection is preferable here to partial
compatibility dependent on the destination configuration. This changes saved
state compatibility, while preserving default guest MMIO and supported IRQ
behavior. Recreate old snapshots before using this branch. Migration still
requires matching machine/device configuration; this work does not change
pre-existing omissions elsewhere in the RX migration model.

## Verification

All commands run inside `.issue3-worktree/` unless noted. Build and
scratch files stay in the worktree; `/tmp` is not used. Meson/ninja come
from `~/workspace/tools/build-venv/bin` at the front of `PATH`.
The four already-fetched subprojects were copied from the original local
checkout; none were fetched again.

```sh
export PATH="$HOME/workspace/tools/build-venv/bin:$PATH"
mkdir -p build .t
export TMPDIR="$PWD/.t"
(cd build && ../configure --target-list=rx-softmmu --disable-docs)
make -C build -j4
make -C build check-qtest-rx -j4
```

- Configure/build: **PASS**. Initial full build used `make -j8`; the final
  incremental build used `make -j4` and compiled the new qtest.
- RX qtest suite: **PASS**, 9 test programs passed, 2 skipped, no failures
  or timeouts. The skipped programs are `cdrom-test` and `qos-test`, which
  have no applicable RX tests. `rx-icu-test` passed all **5 subtests**:
  default and count-256 on each gdbsim machine, plus count-1 boundaries and
  safe out-of-range-map IRQ request/ACK handling.
- Extended IPR: both machines read/write every byte from IPR0 through
  IPR255 with independent four-bit values. Specifically IPR214 at
  `0x873d6` reads back **0x0b** after writing **0xab**. DMRSR remains
  independent in count-256 mode.
- Default regression: the two default-mode qtest subtests also passed
  against the original unmodified `../build/qemu-system-rx`, including
  all 142 IPR bytes, legacy DMRSR aliases, and rejected high IPR accesses.
- Machine listing: `./build/qemu-system-rx -M help` lists both
  `gdbsim-r5f562n7` and `gdbsim-r5f562n8`.
- Empty-flash smoke: baseline and candidate each ran both machines for
  **3 seconds** with `-display none -monitor none -serial none`, without
  an early exit, then terminated cleanly on SIGTERM (status 0).
- VMState: the local `.issue3-runtime/verify.py` script uses QMP file
  migration with the CPU stopped and qtest MMIO. Counts **1, 142 and 256**
  each round-trip every configured IPR value and a DMRSR value. Loading a
  count-256 stream into count 142 rejects it with `8e != 100` before IPR
  loading. An actual version-1 stream generated by the baseline binary is
  rejected as older than minimum version 2. These are device-state tests,
  not a claim that RX CPU execution state migration is complete.
- Invalid count checks: **0, 257 and UINT32_MAX** are rejected with
  `ipr-count must be between 1 and 256`. The RX62N board's existing
  `error_abort` caller terminates these invalid configurations with
  SIGABRT; core dumps were disabled for this check.
- Checkpatch file checks on every changed file: **0 errors, 0 warnings**.
  The combined patch check also has **0 errors**, with one advisory warning
  asking whether new files need a MAINTAINERS update. `git diff --check`
  passes.

The first full qtest run used a longer `TMPDIR`, which made the existing
network test's UNIX socket path exceed the platform limit and time out.
The same issue reproduced against the baseline. Using the short worktree
`.t/` path resolved it: the entire suite and the baseline network test then
passed. No network test or production networking code was modified.

Evidence is retained under `.issue3-runtime/`: `configure.log`, `build.log`,
`build-final.log`, `qtest.log`, `baseline-qtest.log`, `smoke-results.log`,
`migration-results.log`, `invalid-count-results.log`, and
`checkpatch-files.log`. `verify.py` and `smoke.py` retain the local runtime
checks. These generated files are not committed.

## Delivery

The original repository metadata is read-only in this environment. Work is
in the local shared clone `.issue3-worktree/`, based directly on `rx113/dev`.
Only local commits are created; no push, PR, upstream submission or merge.
The PMO owns push and creation of a Draft PR against the fork's `rx113/dev`.

From the original repository, import the branch without merging:

```sh
git fetch ./.issue3-worktree \
  rx113/issue-3-icu-ipr:rx113/issue-3-icu-ipr
```

Alternatively import the incremental bundle in
`.issue3-worktree/.issue3-runtime/issue-3-icu-ipr.bundle`:

```sh
git fetch ./.issue3-worktree/.issue3-runtime/issue-3-icu-ipr.bundle \
  rx113/issue-3-icu-ipr:rx113/issue-3-icu-ipr
```

The bundle requires the base commit named above, which is already present
in the target fork's `rx113/dev`. Use it when transferring to another machine:
the local shared clone alone depends on the original object store. A full
history bundle could not be made because the supplied source object store
lacks a historical object; the incremental bundle includes all new objects
and is verified against the existing base. List all delivery commits with
`git log --reverse --oneline rx113/dev..rx113/issue-3-icu-ipr` in the clone.
