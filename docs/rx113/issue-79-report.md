# Issue #79 report: USB/IP bridge control-transfer pacing

## Problem

The Issue #74 live smoke intermittently deadlocked during
enumeration (roughly half of runs, worse under host CPU load): the
client timed out waiting for a control transfer's RET_SUBMIT while
all 506 qtests passed.

## Diagnosis

Instrumented runs plus a QMP register probe of the wedged guest
showed both sides waiting, each locally consistent:

- The guest firmware sat in its completion wait after writing CCPL,
  polling INTSTS0 for CTSQ to return to idle (0) at the end of
  control transfer N.
- The bridge had already completed transfer N, already injected
  transfer N+1's SETUP, and waited in the data stage for FIFO data
  the firmware would never write, because it had never seen
  transfer N end.

`rx113_usbip_drive()` iterated the whole URB queue in one call:
when a control URB finished, the next control URB was driven and
its SETUP injected in the same call stack, so CTSQ went 2 -> 0 -> 1
inside a single host-side evaluation and a polling guest could miss
the idle level entirely. On a real bus the next SETUP arrives at
least a frame after the status stage, so the firmware always has
time to observe the idle state. The bridge was faster than the bus
it emulates, in a way firmware can observe.

## Fix

One new state bit (`usbip_ctrl_finished`): `usbip_urb_finish()`
sets it for control URBs, `rx113_usbip_drive()` clears it at entry
and refuses to drive further control URBs once it is set. The next
drive (frame tick, submit, or SYSCFG write) injects the following
SETUP, at least one frame later. Non-control URBs are unaffected.
Decision recorded as D-030.

## Verification

- rx113-usb qtests: 28/28.
- Full RX regression: see PR (506 tests).
- Live smoke: in 15 runs on an unloaded host after the fix, zero
  control-transfer stalls (previously ~50% of runs). One run failed
  differently — a bulk IN short read (64 of 100 bytes) — which is
  tracked separately as Issue #80.
- Caveat: under extreme vCPU starvation (a parallel full QEMU
  build on this 1.5-CPU VM) the alias can still occur, because the
  guest may not be scheduled at all across a client round trip.
  Interrupt-driven firmware is immune by construction (requests
  latch); the polling smoke firmware is the sensitive case.
