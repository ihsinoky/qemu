# Issue #72 report: USB0 production host adapter

The USB0 core (Issue #45, D-013) completed transfers only from
explicit fixture inputs. This issue adds the production host
adapter: a real USBBus on the rx113 machine, so guest firmware
can enumerate and exchange data with QEMU's emulated USB
devices. Decision recorded as D-027.

## What was built

All in `hw/rx/rx113-usb.c`, plus `select USB` in
`hw/rx/Kconfig` (the rx target did not link the USB subsystem
before):

- The rx113-usb device creates a USBBus (`usb-bus.0`) with one
  root port in its instance init. Port attach/detach drive the
  core's existing `line` inputs (full-speed devices pull D+ up,
  low-speed D-), so the core's own attach detection, SYSSTS0
  line-state reporting and ATTCH/DTCH interrupts work against
  real devices unchanged.
- While the guest drives the bus reset (DVSTCTR0 reset state),
  the adapter resets the attached device once per assertion
  (`usb_device_reset`), from `usb_update_bus()` - the frame
  timer does not run during reset signalling. This puts the
  device into USB_STATE_DEFAULT at address 0, as a real reset
  pulse would.
- On the core's own 1 ms frame tick, after the core's periodic
  bookkeeping, the adapter submits at most one pending packet
  per pipe: a pending setup transaction (SUREQ) as a SETUP
  packet, a staged transmit packet as OUT, and an IN token for
  an armed receiving pipe (periodic pipes only inside the
  core's expected window). Packets go to the device resolved by
  `usb_find_device(port, DEVSEL)` - DEVSEL is the device
  address and DEVADDn is indexed by it - so devices are found
  directly on the root port or behind a hub where QEMU places
  one. Addresses with no reset device keep their packets
  pending, untouched.
- Results return through `usb_sie_completion()`, the exact
  entry the fixture properties use: success with data is `rx`,
  transmit success `ack`, NAK `nak`, STALL `stall`, anything
  else `error`. Synchronous devices complete inline; async
  ones (usb-storage's SCSI path) complete through the port's
  `complete` callback. A per-pipe channel holds the single
  in-flight packet; a generation counter drops completions
  that arrive after a detach, controller reset or migration.

## Findings along the way

- QEMU chains an automatic hub onto a bus that has exactly one
  free port (`usb_claim_port`), so `-device ...,port=1` is
  needed to place a device directly on the root port. The
  adapter does not care: address resolution goes through
  `usb_find_device()`, which recurses into hubs.
- QEMU's BOT mass-storage device STALLs an IN token while it
  still expects a CBW (a protocol violation on the wire too),
  and holds IN packets asynchronously while a SCSI command is
  in flight. Both behaviors pass through the adapter
  unchanged: the STALL is delivered to the core as a stall
  completion, the held packet completes when the data lands.
- A host NAK is a silent retry (manual section 27.3.3.2): no
  NRDY status, the pipe stays armed. The keyboard test pins
  this contract against a real device.
- Including `hw/usb/usb.h` collided with the core's static
  attach-timer callback `usb_attach()`; it is renamed
  `usb_attach_check()`. No behavior change.

## Scope limits

- Function mode stays fixture-driven; no adapter is built for
  it. QEMU has no gadget-side bus toward an external host to
  bridge to, and the MDD use case is host-side firmware.
- One root port, as on the hardware. Hub enumeration is the
  guest firmware's job (standard hub class requests), with
  addressing resolved by the QEMU core's device lookup.
- Adapter state is not migrated: bus topology is command-line
  configuration, and in-flight packets are abandoned across a
  migration (generation counter), to be retried by the guest
  like a bus error.

## Verification

New qtests in `tests/qtest/rx113-usb-test.c` (fixture tests
untouched, all passing):

- `adapter-enumerate`: usb-storage on the root port; attach
  interrupt from the real line state, bus reset, then
  GET_DESCRIPTOR at address 0 read from the DCP FIFO matches
  the device's actual descriptor (vendor 0x46f4, product
  0x0001), SET_ADDRESS 5, GET_DESCRIPTOR again at address 5,
  SET_CONFIGURATION.
- `adapter-storage`: a full BOT exchange - CBW (INQUIRY) on
  a bulk OUT pipe, 36 bytes of INQUIRY data on the bulk IN
  pipe (SCSI vendor "QEMU"), and the CSW with success status.
- `adapter-nak`: a usb-kbd interrupt IN pipe polled every
  second frame; the device NAKs, and per the host-NAK contract
  nothing is recorded and the pipe stays armed; a control
  transfer afterwards proves the link is alive. (This build
  has no sendkey command, so a real key report cannot be
  injected from qtest.)

Full regression and functional suites: see the PR body for
the recorded counts.
