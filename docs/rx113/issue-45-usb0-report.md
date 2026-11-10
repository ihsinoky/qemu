<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Issue #45: USB0 implementation, run 3

Date: 2026-11-10. Branch: `rx113/issue-45-usb0`.
Base: `3bb63e0564a67a05ec6d761ad3ecd9d87e70c78e`.
Status: implementation complete at the D-012/D-013 register/SIE boundary;
worker delivery for PMO review. No commit or merge was made.
This report supersedes run 2. Its four unfinished implementation areas
are addressed below. Production bus adapters remain separate follow-ups.

Authority: RX113 hardware manual R01UH0448EJ0120 Rev.1.20, Table 5.1,
section 27, Table 14.3 and sections 9/11; project decisions D-012/D-013
in `.issue45-runtime/issue-45-usb0-v2.md`. The completion brief is
`.issue45-runtime/issue-45-usb0-v3.md`. No new project decision is needed.

## Implementation

The RX113-only 0x000a0000/0x200 placeholder is replaced by
`hw/rx/rx113-usb.c`. All 62 Table 5.1 register addresses are decoded.
BUSWAIT, TESTMODE and USBADDR are absent, as required by D-012.
Registers require aligned word accesses, except the three FIFO data ports,
which accept bytes and words. Other widths and register holes have no
effect and return zero. Pipe configuration windows are banked for pipes
1-9. RX62N and shared peripheral implementations are unchanged.

The run-2 bulk/interrupt core is retained: single/two-plane buffering,
CPU/SIE ownership, byte/word endian handling, odd final bytes, RCNT/REW,
BVAL/BCLR, ACLRM, BFRE, DCLRM, ZLPs and transaction counters. Reading a
FIFO never synthesizes received data or completes a transmit. BCLR and
ACLRM never generate BEMP. A full maximum-sized packet automatically
hands its plane to the SIE; shorter packets require BVAL. Section
27.3.5.3 explicitly specifies this packet-sized handoff. There is no
RX113 CNTMD register or programmable allocation window to invent.

BRDY, NRDY and BEMP drive the INTSTS0 summaries through their per-pipe
enables. The status flags are W0C; writing one cannot create an event.
A transmit completing while another plane has a partial CPU write does
not generate premature BRDY/BEMP. A received ZLP queued behind unread
BFRE data now waits until the preceding plane is consumed before asserting
transfer-completion BRDY. DCLRM also releases such a queued ZLP when it
becomes the current plane. The new queued-ZLP test covers both settings.
TRCLR now respects the manual's prohibition on clearing a running counter
or a counter whose pipe still contains buffered data.

### DCP and control stages

Host setup parameters are exposed separately from FIFO data. SUREQ
persists across unsuccessful attempts; ACK generates SACK, and three
consecutive non-ACK results generate SIGN. Either outcome clears SUREQ.
SUREQCLR cancels the pending setup. Setup request registers and DEVSEL
are protected while SUREQ is set. The host controls data/status direction
with DCPCFG.DIR and DATA1 selection with SQSET, as in section 27.3.6.1.
DCPCFG.SHTNAK disables reception after a short packet.

Function setup reception latches all four request words, sets VALID,
forces PID=NAK and SQMON=DATA1, and clears CCPL. VALID prevents PID/CCPL
changes until software clears it. The core distinguishes control read,
control write and no-data requests, enters status stages on the opposite
token, and completes status only when authorized by CCPL/PID or automatic
SET_ADDRESS handling. Status packets bypass the data FIFO and do not
toggle SQMON. Nonzero control-read status payloads are accepted as specified.

Sequence errors set CTSQ=6/CTRT and STALL. The core detects premature
opposite-direction tokens, wrong status tokens, first control-write DATA0
and control-read status DATA0. First DATA0 is an error even if software
has changed SQMON. An error-stage interrupt holds CTSQ=6 until W0C;
a new setup received meanwhile is exposed as a new stage interrupt after
that clear. Ordinary duplicate DATA PIDs do not advance the toggle, FIFO
or BRDY, including when a preceding packet still fills the buffer.

Correct SET_ADDRESS requests automatically supply their status ZLP and
update the internal address and DVSQ on completion, without requiring
VALID clear, BUF, CCPL or FIFO preparation. Invalid address/request fields
and the configured state require software handling. Successful software
SET_CONFIGURATION completion updates the device state. Bus reset clears
setup words, VALID/control stage, CCPL, address and pipe PIDs, and reports
the default device state and connection speed. Buffer/configuration state
not specified for initialization is retained.

The duplicated wIndex bullet in section 27.3.6.2(4) is interpreted together
with its explicit no-data-transfer qualification and the preceding stage
rules: automatic SET_ADDRESS also requires wLength=0.

### Frames and isochronous transfers

Host UACT starts a 1-ms frame timer; clearing UACT, detach, EOF error or
clock/module gating stops it. FRNM wraps at 2048 and SOFR is generated at
the frame boundary. Explicit function SOF reception latches the supplied
frame number and starts interpolation. The first interpolation period is
1 ms; subsequent periods use the preceding actual SOF reception interval.
Interpolated SOFs drive SOFR and interval processing but leave FRNM
unchanged, as stated at the end of section 27.3.10. Bus reset and suspend
initialize interpolation; it does not run before the first received SOF.

Pipes 1/2 accept isochronous completions. Overrun/underrun sets OVRN/NRDY;
received data CRC errors set CRCE/NRDY without applying the bulk three-error
retry/NAK rule. Isochronous transfers do not toggle SQMON. A maximum-packet
size violation still produces BEMP/STALL. Host periodic completion inputs
are restricted to their interval frames. Interrupt pipes use the same
host interval timing. A failed host isochronous transaction consumes its
periodic opportunity.

Function interval counting starts according to IITV: immediately for
zero, and after the first successful packet for nonzero intervals. Missing
OUT packets generate NRDY at the following SOF. With IFIS set, missing IN
packets flush the oldest eligible plane at that SOF, generating BRDY when
CPU access becomes available, never BEMP. Only the oldest plane becomes
transmit eligible at an SOF; a second plane waits for the next boundary.
An early IN completion reports underrun and preserves unready data.
Unexpected tokens otherwise use the available buffer, as in figures
27.17-19. ACLRM initializes the interval state without changing PIPEPERI.

Function bulk ATREPM supplies automatic IN ZLPs, toggling on successful
transmission without BRDY/BEMP, or returns OUT NAK and NRDY without storing
data. ACLRM receive mode discards correctly received data without filling
the FIFO. These responses still require explicit external bus outcomes.

### Digital bus state and output controls

Digital GPIO inputs provide VBUS, D+/D- port state, overcurrent pins, ID
and the USB-PHY portable-device-detection output. They drive SYSSTS0,
VBSTS and INTSTS1 events. Attach/detach qualification uses a 2.5-us stable
port-state interval. These are port/PHY state inputs, not individual USB
packet-bit waveforms. Transactions are supplied separately at the SIE
boundary. RHST distinguishes reset in progress and low/full speed after
reset. HTACT follows the active host sequencer.

DVSTCTR0 controls now have bus effects: USBRST drives reset when UACT and
RESUME permit it; RESUME drives K; UACT runs frames; RWUPE detects a stable
remote K state; function WKUP waits out the remaining 2 ms after suspended
state detection, drives K for 10 ms and self-clears. HNPBTOA suppresses
function resume detection while it holds the suspended state. Function
resume detection works with SCKE=0, sets RESM and returns to the previous
device state. Detach and EOF error clear host UACT. The adapter-facing
output snapshot exposes reset/resume drive, VBUSEN, EXICEN and pull controls.

External input levels survive MCU reset and module stop. Reset refreshes
read-only pin monitors without manufacturing transition interrupts for
held inputs. VBUS, resume, overcurrent, bus-change and PDDET detection can
request USBR while the register clock is stopped. Module-stop inputs are
retained as physical levels; no stopped-module transition event is queued.

USBBCCTRL0/USBMC retain their digital control fields. PDDET is an explicit
digital USB-PHY output, not a comparator-voltage model. PDDETSTS is visible
when IDPSINKE0 enables it. No analog voltages or comparator stimuli are
invented; CHGDETSTS remains zero without such a source.

### DTC, clocks, reset and migration

USBI0=38, USBR0=90, D0FIFO=36 and D1FIFO=37 use the existing DTC frontend.
USBI/FIFO aggregate requests pulse on assertion; USBR is level. Only the
FIFO sources are DTC-activatable. The new DTC test programs actual transfer
descriptors and moves bytes/words between RAM and each FIFO, both ways,
for all BFRE/DCLRM combinations: 16 configurations, each with RX and TX.
It verifies RAM contents, SIE packet contents, descriptor completion and
retained/released buffer ownership. No DTC source classification changed.

UCLK derives from the selected system oscillator or UPLL, including
stability and divider/multiplier checks. SIE processing/FIFO readiness
require live 48-MHz UCLK, USBE and SCKE; host transfers also require UACT.
MSTPB19 stops access, requests and timers while retaining module state.
SCKE=0 permits SYSCFG access and the documented external interrupt inputs.
USBE disable applies Table 27.3 initialization.

Migration includes FIFO bytes/ownership/partial accesses, counters and
errors, DCP stage/address/pending setup state, interval/eligibility state,
external levels, frame/attach/wakeup timers and IRQ assertion history.
Bounds are checked on load. USB VMState version 2 intentionally rejects
run-2 experimental version-1 snapshots; neither format is released.
Tests migrate a partially consumed RX plane, a pending TX and partial
second TX plane, automatic SET_ADDRESS, held pin levels, two failed host
setup attempts and pending attach/frame timers. The timer test aligns
the destination qtest clock before loading, following existing MTU tests.

## Register mask and access audit

Section 27.2 supplies the masks below. The qtests walk individual bits of
SYSCFG, FIFO selectors, ordinary writable registers and all pipe windows
and controls. Separate transfer/state tests exercise action, monitor and
W0C fields. Word-only restrictions, reserved-one USBMC, all reset addresses,
mode-dependent request access, VALID and SUREQ locks are tested.

| Register group (offsets) | Writable mask / special behavior |
| --- | --- |
| SYSCFG (00) | 0579; USBE/SCKE gates, mode/pulls, CNEN monitor |
| SYSSTS0 (04) | read-only line/ID/overcurrent/host activity |
| DVSTCTR0 (08) | 0ff0; read-only RHST; bus/frame/wakeup effects above |
| CFIFO/D0FIFO/D1FIFO (14/18/1c) | byte/word data only, selected direction/ownership/MBW |
| CFIFOSEL (20) | 852f; REW self-clears |
| D0/D1FIFOSEL (28/2c) | b50f; REW self-clears, DREQE/DCLRM |
| FIFOCTR (22/2a/2e) | BVAL/BCLR actions; FRDY/DTLN are monitors |
| INTENB0/1 (30/32) | ff00 / d871 |
| BRDY/NRDY/BEMPENB (36/38/3a) | 03ff each |
| SOFCFG (3c) | 0140; BRDYM readiness, TRNENSEL bus scheduling configuration |
| INTSTS0 (40) | f808 W0C; summaries and device/control monitors read-only |
| INTSTS1 (42) | d871 W0C; all events have explicit producers |
| BRDY/NRDY/BEMPSTS (46/48/4a) | generated pipe bits W0C; BRDYM overrides BRDY clearing |
| FRMNUM (4c) | c000 W0C errors; FRNM read-only |
| USBREQ/VAL/INDX/LENG (54-5a) | ffff in host mode, locked during SUREQ; function input latches |
| DCPCFG/MAXP (5c/5e) | 0090 / f07f; DEVSEL locked during SUREQ |
| DCPCTR (60) | PID/CCPL/SUREQ storage, SQSET/SQCLR/SUREQCLR actions; VALID lock |
| PIPESEL (64) | 000f, selections 0-9 only; zero window unselected |
| PIPECFG (68) | c69f pipes 1-5, c41f pipes 6-9 |
| PIPEMAXP (6c) | f1ff pipes 1/2, f078 pipes 3-5, f07f pipes 6-9 |
| PIPEPERI (6e) | 1007; use/type/mode restrictions in Table 27.15 |
| PIPE1-9CTR (70-80) | 0603 pipes 1-5 / 0203 pipes 6-9; SQ actions, read-only status |
| PIPE1-5TRE/TRN (90-a2) | TRENB=0200; TRCLR action with counter/buffer guards; TRN writable mask ffff when disabled, current count when enabled |
| USBBCCTRL0 (b0) | 00bf; digital PHY PDDET monitor, no analog measurement source |
| USBMC (cc) | 0001, reserved bit 1 reads one |
| DEVADD0-5 (d0-da) | 00c0; target speed supplied to a future adapter |

Programming restrictions from sections 27.2/27.3.4 remain applicable:
configure pipes at PID=NAK and outside FIFO selection; use valid maximum
packet sizes/types/endpoints; do not change CURPIPE with DREQE set; do not
combine BFRE with BRDYM; do not modify sequence/ATREPM while active; obey
host/function restrictions for bus outputs and DEVADD. Prohibited settings
have no promised bus behavior. The bit-mask test also checks reserved bits
without claiming those isolated field patterns are valid transfers.

PBUSY and EDGESTS have no nonzero observable interval in this atomic SIE
fixture: the register read is outside transaction/IRQ pulse processing.
Actual bus packet duration and token issuance within a frame belong to the
production adapter. DEVSEL/DEVADD/TRNENSEL and PHY output controls are
configuration for that adapter, rather than invented bus completions.

## D-013 fixture contract, extended in run 3

Properties live on `/machine/mcu/usb0`, outside guest MMIO. Inputs use the
same completion/state handlers that a future adapter will drive; there is
no loopback implementation. Invalid syntax, mode, direction, ownership,
clock/stop state and host periodic windows return QMP errors without
pretending a transfer succeeded.

Write `sie-completion`:

| Value | Meaning |
| --- | --- |
| `rx:N:HEX` | Successfully received packet, using the expected DATA PID |
| `rx0:N:HEX`, `rx1:N:HEX` | Received packet with explicit DATA0/DATA1 |
| `ack:N` | Successful transmission of the oldest eligible packet; for iso this means completion without a handshake |
| `nak:N` | Actual handshake result; host retry or function buffer/ATREPM NAK |
| `stall:N` | Host received STALL |
| `error:N` | Failed host attempt; for iso RX, received data CRC/bit-stuffing failure |
| `setup:0:HEX` | Function received/ACKed an eight-byte setup request |

N is one digit, 0-9. Payloads are limited to 256 bytes; empty HEX is a ZLP.
Pipe 0 host ACK/error/NAK/STALL applies to setup while SUREQ is pending;
otherwise it applies to the DCP FIFO data path. Function status completion
uses `rx[1]:0:HEX` for control read, `ack:0` for control write/no-data.
ATREPM and automatic control status packets need no CPU FIFO handoff.

Additional properties:

| Property | Direction / value |
| --- | --- |
| `sie-token` | write `in` or `out`: a function DCP token, including data-to-status transitions and wrong-token errors; does not acknowledge data |
| `sie-sof` | write decimal frame number 0-2047: actual function SOF reception |
| `bus-event` | write `reset` or `suspend` in function mode; `eof-error` in host mode; qualified SIE/PHY detections, not analog waveforms |
| `sie-tx` | read immutable `N:hex;` snapshots in pipe order, including automatic ZLP responses |
| `sie-setup` | read the eight pending host setup bytes as hex; empty if no host setup is pending |
| `bus-output` | read `drive=idle\|se0\|k;vbusen=N;exicen=N;pullup=N;pulldown=N`; pullup bits correspond to SYSCFG DMRPU/DPRPU |

The digital named GPIO inputs are `vbus` (one line), `line` (line 0 = D+,
line 1 = D-), `overcurrent` (line 0 = OVRCURB, line 1 = OVRCURA), `id`
and `pddet` (one line each). They represent external port/PHY state, not
USB transfer acknowledgements. Input levels default to zero and persist
through reset. Output/snapshot reads never consume or acknowledge a packet;
ordinary software buffer clearing cannot substitute for an SIE completion.

## Validation and delivery

Final checks and their evidence are under `.issue45-runtime/`:

| Check | Result / evidence |
| --- | --- |
| rx-softmmu build | PASS; `build-run3-delivery.log` |
| USB qtest | PASS, 21 scenarios; `usb-run3-final.log` |
| All configured RX qtest/quick functional suites | PASS, 32 programs, zero failures, 4 expected skips; `regression-run3-final.log` |
| FIT PLL and HOCO, including reset | PASS; `run3-fit-pll.log`, `run3-fit-hoco.log` |
| SYSTEM with/without FIT ELF | PASS; `run3-system.log`, `run3-system-elf.log` |
| Machine help / both gdbsim smoke runs | PASS; `run3-machines.log`, `run3-gdbsim-smoke.log` |
| RX62N U-Boot | PASS; `run3-functional-gdbsim.log`; existing Linux/sash flaky skip retained |
| Changed C/header checkpatch | 0 errors, 0 warnings; `checkpatch-run3-final.log` |
| Delivery patch checkpatch | 0 errors; `checkpatch-patch-run3.log` |
| Patch forward dry run/apply/content comparison/reverse dry run | PASS; `delivery-validation-run3.log` |

The final combined suite uses `run-regression-run3.sh`. The four expected
program-level skips are cdrom-test, qos-test, info_usernet and linters.
VNC retains its existing crypto capability skip within the passing program.
The first run, without the scratch wrapper, preserved the known three
generic functional `/var/tmp` failures in `regression-run3.log`; no source
change was made to those tests. `regression-wrapper.py` invokes the
existing `run-functional.py` only for their helper VM scratch default.
The final suite passes in one invocation. The separate gdbsim U-Boot run
uses the same scratch wrapper; its initial `/var/tmp` failure is retained
in `run3-functional-gdbsim-initial.log`. `.t` is a short symlink into
`.issue45-runtime/.t`, avoiding Unix socket path-length limits; no scratch
is stored in tmpfs `/tmp`. No subproject was re-fetched.

Ninja repeatedly reported recovery of a truncated dependency/log record
and rebuilt broadly. Each complete make invocation succeeded. During
iteration, `rebuild-usb-core.py`/`rebuild-usb-test.py` used Ninja's expanded
compiler/link commands for focused checks. The final standard make and
regressions use the final tree. The binary hash is recorded in
`run3-tested-binary.sha256`.

Delivery: `.issue45-runtime/issue-45-usb0-complete.patch` and its SHA-256
sidecar. `make-delivery.py` constructs the explicit eight-file patch from
HEAD, verifies forward application against an isolated baseline, applies
it, compares all eight files byte-for-byte and verifies reverse application.
`git diff --check` passes. Checkpatch uses `--no-tree --no-signoff`; no DCO
sign-off is asserted on this uncommitted artifact. AGENTS.md, probe.txt,
manuals, runtime briefs/scripts/logs, scratch aliases and build products
are excluded. No push, PR, merge, upstream submission or Git metadata write
was made; commits and integration remain with the PMO/PO.

## Separate production-adapter follow-ups

There is no USBBus/USBDevice adapter and no claim of actual USB attachment,
enumeration or host-device interoperability. Under D-013, a host adapter
still needs endpoint/device mapping, scheduling within the allowed frames,
asynchronous USBPacket lifetimes/cancellation and real result translation
(4-7 engineer-days). A function adapter needs a USBDevice-facing topology,
guest-controlled enumeration/endpoints, pin/PHY state integration and
packet lifetimes (5-10 engineer-days). These remain planning estimates.
Neither follow-up is an unfinished register/SIE core item from run 2.
