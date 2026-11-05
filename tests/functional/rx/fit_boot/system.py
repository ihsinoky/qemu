#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check RX113 startup register protection and oscillator dependencies."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--elf', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / 'system-stderr.log').open('w') as err:
        image = ['-kernel', str(args.elf.resolve())] if args.elf else []
        proc = subprocess.Popen(
            [str(args.qemu.resolve()), '-M', 'rx113-r5f51138',
             '-accel', 'qtest', '-qtest', 'stdio', '-display', 'none',
             '-monitor', 'none', '-serial', 'none', *image],
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

        try:
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
            assert read(0x8003c) & 5 == 5
            write(0x8002e, 0)
            assert read(0x8003c) & 0x20 == 0x20
            write(0x8003c, 0)  # Status is read-only.
            assert read(0x8003c) & 0x25 == 0x25
            write(0x80032, 1)
            assert read(0x8003c) & 0x25 == 0
            write(0x80036, 0)
            assert read(0x8003c) & 8 == 8
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
            print('PASS: PRCR key/protection, read-only flags, oscillator '
                  'dependencies, stop transitions, MSTPCRB readback')
        finally:
            proc.terminate()
            proc.wait(timeout=10)


if __name__ == '__main__':
    main()
