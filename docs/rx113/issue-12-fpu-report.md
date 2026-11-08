# Issue 12: Restrict FPU on the RX113 CPU

## Adopted behaviour

On the RX113 CPU (no FPU in any variant, `rx62n-rx113-diff.md` section 2):

| Guest action | Behaviour |
| --- | --- |
| FADD, FSUB, FMUL, FDIV, FCMP, FTOI, ROUND, ITOF (all addressing forms) | Undefined instruction exception: PSW and PC pushed on ISP, PM/U/I cleared, vector fetched from `0xFFFFFFDC`, saved PC = address of the faulting instruction. |
| `MVFC FPSW,Rd` | Reads 0 (logged as `LOG_GUEST_ERROR`). |
| `MVTC ...,FPSW` | Write ignored (logged as `LOG_GUEST_ERROR`). |

RX62N (`rx62n` CPU, both gdbsim machines) is unchanged.

## Primary-source basis (R01UH0448EJ0120 Rev.1.20)

- 13.1.1: an undefined instruction exception occurs when "an instruction not
  implemented" is executed. 13.5.1: hardware pre-processing (PSW saved, PM/U/I
  cleared, PC saved, vector from `FFFF FFDCh`). Table 13.1: the saved PC is
  "the PC value of the instruction that generated the exception". Figure 2.4
  confirms the `FFFFFFDCh` vector. QEMU's existing exception 23 path already
  implements exactly this.
- 2.2.2 lists the eight control registers (ISP, USP, INTB, PC, PSW, BPC,
  BPSW, FINTV); FPSW is not among them, and the manual defines no
  floating-point instructions. Hence FPU instructions are "not implemented".
- Inference (not stated explicitly in the hardware manual): for the FPSW
  accesses, MVFC/MVTC themselves are implemented instructions and the
  manual gives no exception for them, so they are treated like QEMU's
  existing non-existent control registers (read 0, write ignored) rather
  than raising an exception. The RX Family software manual (R01US0032) was
  not available offline to confirm; see Remaining items.

## Implementation

- `target/rx/cpu.h`: `RXCPUClass.has_fpu` (default true).
- `target/rx/cpu.c`: new `rx113-rx-cpu` type (`TYPE_RX113_CPU`) whose
  class_init clears `has_fpu`. `rx62n` is untouched and keeps `true`.
- `target/rx/translate.c`: the FPU `trans_*` functions return false (the
  existing decode-failure path raises the undefined instruction exception)
  and the FPSW cases of `move_from_cr`/`move_to_cr` fall back to
  RAZ/WI when `!has_fpu`. With `has_fpu` true the generated code is
  identical to before.
- `hw/rx/rx113.c`, `hw/rx/rx113-machine.c`: the MCU and machine now use
  `TYPE_RX113_CPU`.

Placing the switch in a CPU model type (class-level, constant per CPU) keeps
it off by default for every existing CPU, needs no TB-flag change, and
matches how QEMU models other per-core feature differences.

## Verification

- New `tests/functional/rx/test_rx113_fpu.py` (registered in
  `meson.build`, assetless, quick): runs each of 13 FPU instruction forms
  plus an FPSW write/read case and a FADD result check as flash-resident
  code. RX113 must end in the undefined-instruction handler with the exact
  faulting PC (`U`) and FPSW reads 0 (`Z`); `gdbsim-r5f562n8` must execute
  them (`N`, `D`, and FADD 1.0+1.0 = 2.0). Both pass.
- Genuine FIT probe (PLL and HOCO) via `fit_boot/verify.py`: `main()` reached
  and SCI0 output preserved, also after reset. `fit_boot/system.py` PASS.
  (A stray macro edit during development that broke BCLR/BSET was caught by
  this probe and fixed before commit.)
- `test_rx113.py` (2 tests), `rx-icu-test` qtest, `-M help` lists both gdbsim
  machines and `rx113-r5f51138`: pass. gdbsim functional tests need network
  assets and were not run; RX62N is covered by the new regression and qtest.
- checkpatch: 0 errors on changed files (one pre-existing >80-column
  warning in `translate.c`, not in lines touched here).
- No new network fetches: `r_bsp.zip` was copied from the Issue 6 worktree.

## Remaining items

- Confirm FPSW access semantics (RAZ/WI vs. exception) against the RX
  Family software manual; the choice is a one-line change in
  `move_from_cr`/`move_to_cr`.
- RXv2-only FPU instructions (FSQRT, FTOU, UTOF) are not decoded by this
  RXv1 core at all, so they already raise the undefined instruction
  exception; the RX113's RXv2 integer extensions remain out of scope.
- gdb register view still exposes an `fpsw` register for RX113.
