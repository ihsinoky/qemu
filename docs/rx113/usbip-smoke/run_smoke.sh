#!/bin/sh
# Live function-adapter smoke (manual harness, not a build test).
# Builds the bare-metal bulk-echo firmware with an rx-elf
# toolchain, runs it on the model under TCG, and drives
# enumeration + bulk echo from client.py, an independent
# USB/IP client written from the protocol document.
# Expected tail of output: "LIVE SMOKE: PASS".
# See ../function-adapter-handover.md for context.
set -e
cd "$(dirname "$0")"
TC=${RX_TC:-rx-elf}
QEMU=${QEMU:-../../../build/qemu-system-rx}
$TC-gcc -O1 -ffreestanding -nostdlib -mcpu=rx100 -c fw.c -o fw.o
$TC-gcc -O1 -ffreestanding -nostdlib -mcpu=rx100 -c start.S -o start.o
$TC-ld -T link.ld fw.o start.o -o fw.elf
"$QEMU" -M rx113-r5f51138 -accel tcg -display none -monitor none \
    -serial none -parallel none \
    -kernel fw.elf \
    -global rx113-usb.usbip-listen=127.0.0.1:3240 &
QPID=$!
trap 'kill $QPID 2>/dev/null; wait $QPID 2>/dev/null' EXIT
# Under TCG the guest needs a few seconds to finish its clock
# setup before the first attach; do not shorten this blindly.
sleep 3
python3 client.py
