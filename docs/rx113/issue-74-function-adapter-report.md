# Issue #74 report: USB0 function adapter (Stage 1)

The USB0 core (Issue #45, D-013) models the function side at the
SIE boundary, but until now only the fixture properties could
play the host: real firmware waited for an enumeration that never
came. Stage 0 (survey on the issue) established that QEMU has no
gadget-side bus to bridge to, and that USB/IP is the one
host-PC-reachable topology. This stage builds the server.
Decision recorded as D-028.

## What was built

All in `hw/rx/rx113-usb.c`:

- A USB/IP server (protocol as in the kernel's
  Documentation/usb/usbip_protocol.rst) living in the
  rx113-usb device. It exists only when the `usbip-listen`
  string property is set (a socket address such as
  "127.0.0.1:3240"), listens through a QIOChannel socket on the
  main loop, and serves one session at a time. With the
  property unset nothing is created and guest-visible behavior
  is bit-identical. The actually bound port is readable via
  the read-only `usbip-port` property, which also makes
  port 0 (ephemeral) usable.
- OP_REQ_DEVLIST answers with the single emulated device
  (bus id "1-1"); OP_REQ_IMPORT starts the session: VBUS is
  driven (VBSTS follows), and once the guest raises its
  pull-up (SYSCFG DPRPU) the bridge injects one bus reset and
  kicks SOF interpolation, so the frame tick runs and the
  firmware sees the attach sequence a real host produces.
  Dropping the pull-up re-arms the reset. TCP disconnect,
  device reset and migration end the session: all pending
  URBs are dropped and the bridge-owned VBUS is released.
  Adapter state is not migrated (same stance as D-027).
- URBs are translated onto the very SIE boundary the fixture
  uses (`usb_sie_completion` / `usb_sie_token` / the bus-event
  and SOF entry points), so every core semantic - DATA toggle
  discipline, BRDY/BEMP, the control-stage machine, the
  SET_ADDRESS auto response - is inherited, not re-implemented:
  - Control URBs run as a state machine driven on the frame
    tick (and on submit / SYSCFG writes): the SETUP packet is
    injected as a setup completion; data-stage packets move
    through rx/ack completions; the stage token then moves to
    the status stage, whose completion is only injected once
    the firmware signals CCPL (or the SET_ADDRESS auto
    response applies) - the exact sequence the fixture tests
    script by hand.
  - Bulk and interrupt URBs resolve their pipe from PIPECFG
    by EPNUM and the DIR bit (set = SIE sends = host IN) -
    the map is core-internal state, which is why the server
    had to live in the device. OUT data is chunked to the
    pipe's max packet and injected per free FIFO plane; IN
    URBs collect packets until a short packet or the
    requested length. A pipe STALL completes the URB with
    -EPIPE. One URB per endpoint queue is driven at a time.
  - CMD_UNLINK removes the named URB (no RET_SUBMIT follows)
    and answers RET_UNLINK; TCP streams are reassembled
    through partial reads and writes.

## Divergences and limits

- SET_ADDRESS never reaches the server: vhci completes it
  locally (verified in vhci_hcd sources during Stage 0), as it
  does for real USB/IP devices. The emulated firmware's address
  state therefore only changes if the client stack sends the
  request through, which vhci does not; SET_CONFIGURATION and
  the data pipes are unaffected.
- The protocol has no mid-session bus-reset signal, so a
  host-side vhci port reset is invisible to the guest until a
  re-import.
- Isochronous URBs are refused with -EOPNOTSUPP (the pipe
  engine stays available to the fixture unchanged).
- The DEVLIST/IMPORT device record reports zero VID/PID and
  descriptor classes: the firmware's descriptors cross only
  during enumeration itself. This is display metadata for the
  client tools; enumeration reads the real descriptors.
- URBs have no bridge-side timeout; a firmware that never
  arms a pipe leaves the URB pending, like a NAK-ing device
  on a real bus. The client stack owns timeouts and UNLINK.

## Verification

- New qtests (`tests/qtest/rx113-usb-test.c`, suite now 28)
  carry a minimal USB/IP client inside the test and play the
  firmware through MMIO, mirroring how the fixture tests play
  the host:
  - usbip-enumerate: DEVLIST/IMPORT handshake, VBUS and the
    pull-up-triggered reset, GET_DESCRIPTOR (device) through
    a full control read whose bytes arrive intact, and
    SET_CONFIGURATION reaching the Configured device state.
  - usbip-bulk: a bulk OUT payload lands in the pipe FIFO
    byte for byte; a bulk IN payload loaded by the firmware
    is delivered to the client.
  - usbip-interrupt: an interrupt IN report (the HID shape)
    is delivered by URB polling.
  - usbip-unlink: a pending IN URB is unlinked (RET_UNLINK,
    no RET_SUBMIT), and the queue keeps working afterwards.
- The pre-existing 24 USB tests pass unmodified, as does the
  full 20-suite RX regression (506 tests, 0 failures) and the
  func-rx suite (the known worktree-only func-rx-vnc
  environmental failure is unchanged). checkpatch reports no
  errors on the changed files.

## Remaining work

- Stage 2 (separate go-ahead): attach a real vhci (Linux, or
  WSL via usbipd-win) and run the PO's own CDC/HID
  communication tool against firmware on the model. Windows
  native tools cannot see a USB/IP device (no maintained
  Windows-side client); the tool is self-made, so running it
  under WSL or giving it a TCP transport with a small
  WSL-side bridge are the two candidate shapes.

## Addendum (2026-11-12): Stage 2 preparation findings

A wire-layout audit against the kernel's normative sources
(Documentation/usb/usbip_protocol.rst, the tools' packed
struct usbip_usb_device and drivers/usb/usbip/usbip_common.h)
found one real divergence, tracked as Issue #76:
OP_REP_IMPORT must carry the full 312-byte device record,
path included; the implementation sent a 56-byte suffix and
the in-test client shared the assumption. All other audited
layouts matched (op headers, DEVLIST count + interface
records, the 48-byte URB header field offsets).

With the fix, a live interoperability smoke also passes:
a bare-metal RX113 firmware (bulk echo device, EP1 IN /
EP2 OUT, polling) runs on the model under TCG - a real guest
CPU, not the qtest MMIO actor - and an independent Python
USB/IP client written from the protocol document enumerates
it (descriptors byte-checked, SET_CONFIGURATION) and
completes bulk echo round trips of 100, 5 and 64 bytes.
Three consecutive runs passed. Remaining Stage 2 work is
the on-machine check with a real vhci (Linux or WSL) and
the PO's own communication tool.
