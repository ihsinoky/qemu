#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check RX113 startup register protection and oscillator dependencies."""
import argparse
import json
from pathlib import Path
import subprocess
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--elf', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / 'system-stderr.log').open('w') as err:
        image = ['-kernel', str(args.elf.resolve())] if args.elf else []
        qmp_path = args.output.resolve() / 'qmp.sock'
        proc = subprocess.Popen(
            [str(args.qemu.resolve()), '-M', 'rx113-r5f51138',
             '-accel', 'qtest', '-qtest', 'stdio', '-display', 'none',
             '-monitor', 'none', '-serial', 'none',
             '-qmp', f'unix:{qmp_path},server=on,wait=off', *image],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=err, text=True)

        def command(cmd):
            proc.stdin.write(cmd + '\n')
            proc.stdin.flush()
            reply = proc.stdout.readline().strip()
            assert reply.startswith('OK'), (cmd, reply)
            return reply.split()[1:]

        def read(addr, width='b'):
            return int(command(f'read{width} {addr:#x}')[0], 16)

        def write(addr, value, width='b'):
            command(f'write{width} {addr:#x} {value:#x}')

        def step(ns):
            command(f'clock_step {ns}')

        def status():
            value = read(0x8003c)
            assert value & ~0x2d == 0, hex(value)  # Reserved bits stay zero.
            return value

        def rises_after(mask, ns):
            assert status() & mask == 0
            step(ns - 1)
            assert status() & mask == 0
            step(1)
            assert status() & mask == mask

        try:
            qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            qmp.settimeout(10)
            for _ in range(100):
                try:
                    qmp.connect(str(qmp_path))
                    break
                except FileNotFoundError:
                    time.sleep(0.05)
            stream = qmp.makefile('rw')
            assert 'QMP' in json.loads(stream.readline())

            def qmp_command(name):
                stream.write(json.dumps({'execute': name}) + '\n')
                stream.flush()
                while True:
                    reply = json.loads(stream.readline())
                    if 'event' not in reply:
                        assert 'return' in reply, reply
                        return

            qmp_command('qmp_capabilities')
            hoco_stop = (read(0xffffff88, 'l') >> 8) & 1
            assert read(0x80036) == hoco_stop
            assert read(0x8003c) == (0 if hoco_stop else 8)
            assert read(0x80032) == 1
            assert read(0x8002a) == 1
            assert read(0x803fe, 'w') == 0
            assert read(0x80014, 'l') == 0xffffffff
            write(0x80032, 0)  # Protected.
            hoco_stop = (read(0xffffff88, 'l') >> 8) & 1
            assert read(0x80036) == hoco_stop
            assert read(0x8003c) == (0 if hoco_stop else 8)
            assert read(0x80032) == 1
            write(0x803fe, 0x5a0f, 'w')  # Wrong key.
            assert read(0x803fe, 'w') == 0
            write(0x803fe, 1)  # PRCR requires a keyed word write.
            assert read(0x803fe, 'w') == 0
            write(0x803fe, 0xa501, 'w')
            assert read(0x803fe, 'w') == 1  # Key is not retained.
            write(0x80020, 0x12000102, 'l')
            assert read(0x80020, 'l') == 0x12000102
            write(0x80026, 0x400, 'w')
            assert read(0x80026, 'w') == 0x400
            write(0x8002a, 0)  # PLL without main oscillator.
            assert read(0x8003c) & 5 == 0
            write(0x80032, 0)
            rises_after(1, 2048000)
            rises_after(4, 50000)
            write(0x8002e, 0)
            rises_after(0x20, 50000)
            write(0x8003c, 0)  # Status is read-only.
            assert read(0x8003c) & 0x25 == 0x25
            write(0x80032, 1)
            assert read(0x8003c) & 0x25 == 0
            write(0x80036, 1)
            write(0x80036, 0)
            rises_after(8, 34500)
            write(0x80036, 1)
            assert read(0x8003c) & 8 == 0
            write(0x80014, 0x7fffffff, 'l')  # PRC1 still disabled.
            assert read(0x80014, 'l') == 0xffffffff
            write(0x803fe, 0xa502, 'w')
            write(0x80014, 0x7fffffff, 'l')
            assert read(0x80014, 'l') == 0x7fffffff
            write(0x803fe, 0xa500, 'w')
            write(0x80014, 0xffffffff, 'l')
            assert read(0x80014, 'l') == 0x7fffffff
            # All documented LOCO-based main waits, including stopped LOCO.
            write(0x803fe, 0xa507, 'w')
            write(0x80034, 1)
            for setting, cycles in enumerate(
                    [2, 1024, 2048, 4096, 8192, 16384, 32768, 65536]):
                write(0x80032, 1)
                write(0x800a2, setting)
                write(0x80032, 0)
                # Rewriting enable and unrelated registers must not restart.
                step(1)
                write(0x80032, 0)
                write(0x80020, 0, 'l')
                rises_after(1, cycles * 250 - 1)
                rises_after(0x24, 50000)  # Both PLLs were left enabled.
            for setting, cycles in [(5, 138), (6, 266)]:
                write(0x80036, 1)
                write(0x800a5, setting)
                write(0x80036, 0)
                rises_after(8, cycles * 250)
            # Stop cancels outstanding waits; restart gets a full interval.
            write(0x80032, 1)
            write(0x800a2, 0)
            write(0x80032, 0)
            step(250)
            write(0x80032, 1)
            step(100000)
            assert status() & 0x25 == 0
            write(0x80032, 0)
            rises_after(1, 500)
            step(25000)
            write(0x8002a, 1)
            write(0x8002e, 1)
            step(100000)
            assert status() & 0x24 == 0
            write(0x8002a, 0)
            write(0x8002e, 0)
            rises_after(0x24, 50000)
            write(0x80036, 1)
            write(0x80036, 0)
            step(1000)
            write(0x80036, 1)
            step(100000)
            assert status() & 8 == 0
            write(0x80036, 0)
            rises_after(8, 66500)
            # Prohibited settings do not accidentally assert stable flags.
            write(0x80032, 1)
            write(0x800a2, 8)
            write(0x80032, 0)
            write(0x80036, 1)
            write(0x800a5, 0)
            write(0x80036, 0)
            step(100000000)
            assert status() == 0
            # Reset must cancel timers and restore the option-byte semantics.
            write(0x80032, 1)
            write(0x800a2, 7)
            write(0x80032, 0)
            write(0x80036, 1)
            write(0x800a5, 6)
            write(0x80036, 0)
            step(1000)
            qmp_command('system_reset')
            assert status() == (0 if hoco_stop else 8)
            step(100000000)
            assert status() == (0 if hoco_stop else 8)
            assert read(0x80032) == 1
            assert read(0x8002a) == 1
            assert read(0x8002e) == 1
            stream.close()
            qmp.close()
            print('PASS: exact virtual-time oscillator delays, cancellation, '
                  'restart, reset, stopped LOCO, reserved bits; PRCR '
                  'key/protection, read-only flags, oscillator '
                  'dependencies, stop transitions, MSTPCRB readback')
        finally:
            proc.terminate()
            proc.wait(timeout=10)


if __name__ == '__main__':
    main()
