# USB0 function adapter — operation and handover notes

This document is the entry point for anyone picking up the
RX113 model's USB function adapter later — in particular
the remaining Stage 2 verification of Issue #74, which is
expected to happen long after the implementation work.
Everything normative lives elsewhere and is only pointed
to here: decisions in `decisions.md` (D-013, D-027, D-028,
D-029), behaviour evidence in `findings.md`, per-issue
detail in `issue-72-usb-adapter-report.md` and
`issue-74-function-adapter-report.md`.

## State of the model (2026-11-12)

- The peripheral catalogue is complete: every peripheral
  in the RX113 hardware manual is a real model, no
  register stubs remain. The RX qtest regression is
  20 suites / 506 tests / 0 failures.
- USB0 has both production adapters:
  - Host side (Issue #72, D-027): a real USBBus inside the
    device model; QEMU USB devices (usb-storage, usb-kbd…)
    enumerate against host-mode firmware.
  - Function side (Issue #74 Stage 1, D-028): an in-device
    USB/IP server bridging URBs onto the SIE boundary, so
    device-side firmware can talk to a live host stack.
- The only open issue is #74, and only for its Stage 2:
  an on-machine check with a real vhci and the PO's own
  communication tool. Nothing else is known to be missing;
  optional follow-ups (peer links, extra SCI modes, RTC
  residuals, analogue behaviour) are listed with estimates
  at the end of each issue report and are not required for
  firmware-logic work.

## Building and running

```
mkdir build && cd build
../configure --target-list=rx-softmmu --disable-docs
ninja qemu-system-rx
./qemu-system-rx -M rx113-r5f51138 -kernel fw.elf   # ELF
./qemu-system-rx -M rx113-r5f51138 -bios flash.bin  # raw flash
```

Machine variants `rx113-r5f51135/36/37` select the smaller
flash/RAM parts. Guest firmware must do its own clock
setup; for USB the firmware is responsible for PRCR
unlock, UCLK = 48 MHz, releasing the USB0 module stop
(MSTPCRB bit 19), SYSCFG USBE|SCKE, and DPRPU for
function mode — exactly as on hardware.

## The function adapter in brief

- Enable with a device property on the command line:
  `-global rx113-usb.usbip-listen=127.0.0.1:3240`
  (any `socket_parse` address; port 0 is allowed, the
  bound port is readable back via the read-only
  `usbip-port` property). With the property unset the
  server does not exist and guest-visible behaviour is
  bit-identical.
- One session at a time. On OP_REQ_IMPORT the server
  drives VBUS; when the guest raises DPRPU it injects one
  bus reset and kicks SOF interpolation, reproducing a
  real host's attach sequence. A pull-up drop re-arms
  the reset. TCP disconnect, device reset and migration
  drop the session and all pending URBs; adapter state is
  not migrated.
- URBs translate onto the D-013 SIE boundary. Control
  transfers run the setup/data/status stage machine,
  with the status stage gated on the firmware writing
  CCPL exactly as hardware requires. Bulk/interrupt URBs
  resolve their pipe by PIPECFG EPNUM plus the DIR bit —
  note DIR means "the SIE sends" in both modes (set =
  device-to-host in function mode).
- The wire format follows the kernel's USB/IP protocol
  (version 0x0111). OP_REP_IMPORT carries the full
  312-byte device record (Issue #76 fixed a shortened
  reply; if interop misbehaves after future changes,
  audit the layouts in this area first).

## Recorded divergences (D-028)

- SET_ADDRESS is never delivered to the model: vhci
  completes it locally, exactly as with real USB/IP
  devices, so on this path the model's device state does
  not pass through the USB Address state.
- The protocol has no mid-session bus-reset signal; a
  host-side port reset is invisible until a re-import
  (or until the firmware drops and re-raises its
  pull-up).
- Isochronous URBs are refused with -EOPNOTSUPP.
- The device record reports zero VID/PID/class; the real
  descriptors are only exchanged during enumeration.

Accepted limitations elsewhere in the model follow the
same discipline: each is recorded in its issue, and the
load-bearing ones are decisions (e.g. D-029, the IWDT
lazy-interval WUNI re-request delay, accepted because the
first underflow is always exact and normal servicing
exits the lazy state).

## Live smoke (do this first when resuming)

`docs/rx113/usbip-smoke/` holds a self-contained harness
that proves the adapter end to end without any external
hardware: a bare-metal bulk-echo firmware (fw.c/start.S/
link.ld, needs an rx-elf toolchain) runs on the model
under TCG — a real guest CPU, not the qtest MMIO actor —
and client.py, an independent USB/IP client written from
the protocol document, enumerates it and checks bulk
echo round trips. `run_smoke.sh` ties it together; the
expected last line is `LIVE SMOKE: PASS`. Note the
built-in startup sleep: under TCG the guest needs a few
seconds to finish clock setup before the first attach.

Reliability history: the smoke originally stalled
intermittently during enumeration (about half of runs).
That was Issue #79 — the bridge completed a control
transfer and injected the next SETUP inside one drive
call, so polling firmware could miss CTSQ returning to
idle and deadlock; fixed by pacing control transfers to
bus timing (D-030, PR #81). After the fix, enumeration
stalls were gone in 15 consecutive unloaded runs. One
later run failed differently, with a bulk IN short read
(64 of 100 bytes) — tracked as Issue #80, not yet
diagnosed; if the smoke fails in the bulk phase with an
echo mismatch, suspect #80, and retry on an idle machine.

## Stage 2 checklist (the remaining verification)

Goal: the PO's own communication tool exchanges real
traffic with target firmware running on the model,
through a real vhci. The tool is self-made and presents
the device as CDC or HID.

1. Build QEMU from `rx113/dev` as above.
2. Launch the model with the target firmware and
   `-global rx113-usb.usbip-listen=<addr>:3240`.
3. On a Linux host or a WSL2 distro: `modprobe vhci-hcd`,
   then `usbip list -r <addr>` (the model's bus id is
   `1-1`), then `usbip attach -r <addr> -b 1-1`. From
   Windows, usbipd-win's `usbipd attach --wsl` targets a
   distro; QEMU may run inside the same distro, or be
   reached over the WSL network from another one.
   A Windows-native process cannot see the device — there
   is no COM port on the Windows side; CDC appears as
   /dev/ttyACM* and HID as /dev/hidraw* inside Linux.
4. Point the tool at that node: either port the tool to
   Linux, or keep it on Windows and add a TCP transport
   plus a small bridge in WSL (socat-class for CDC byte
   streams).
5. Check: enumeration completes in the guest's terms
   (the firmware reaches its Configured state), a data
   round trip succeeds in both directions, and a
   disconnect/re-attach cycle re-enumerates cleanly.
6. On success, close Issue #74 with the environment, the
   tool shape used, and the traffic checked. On failure,
   capture where it stalls against the troubleshooting
   notes below before changing model code.

## Troubleshooting

- `usbip list` shows nothing / attach hangs in the
  OP phase: check the listen address is reachable and
  the version/record layouts (this was Issue #76's
  failure mode).
- Enumeration never starts: the guest has not raised
  DPRPU, or USB is not actually running — verify the
  firmware's clock setup (UCLK 48 MHz) and module-stop
  release; VBUS only appears after IMPORT.
- Enumeration stalls mid-descriptor: the firmware's
  control flow must clear VALID and finish with CCPL;
  the adapter deliberately gates the status stage on
  CCPL, as hardware does. (Historical bridge-side cause:
  Issue #79 — fixed; see D-030. If this recurs on a build
  without the #79 fix, that is the first suspect.)
- Bulk echo returns a short count (e.g. 64 of 100
  bytes): known intermittent Issue #80, undiagnosed;
  capture the run and retry on an idle host.
- Bulk data stalls in one direction: check PIPECFG —
  EPNUM must match the endpoint number, DIR is "SIE
  sends", and pipes are configured after
  SET_CONFIGURATION.
- Everything works in qtest but not live: run the smoke
  harness above; it separates model faults from
  firmware faults (the qtest actor is lockstep, the
  smoke firmware is not).

## Records index

- `docs/rx113/project.md` — project charter and policies
  (fork only, RX62N behaviour invariant, implement by
  default).
- `docs/rx113/decisions.md` — D-001…D-029.
- `docs/rx113/findings.md` — hardware cross-check ledger
  F-001…F-006.
- `docs/rx113/issue-*-report.md` — per-issue design,
  verification and limitations.
- Repository `AGENTS.md` — fork-local working rules.
