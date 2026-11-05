#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run an externally built FIT probe; retain PC, MMIO and execution logs."""
import argparse
import json
from pathlib import Path
import socket
import time

from qemu.machine import QEMUMachine


def receive(vm):
    vm.console_socket.settimeout(10)
    output = b''
    while b'RX113 FIT main reached\n' not in output:
        data = vm.console_socket.recv(1024)
        if not data:
            raise RuntimeError('SCI0 console closed: ' + repr(output))
        output += data
    return output.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu', type=Path)
    parser.add_argument('elf', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--expect-stall', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    with QEMUMachine(str(args.qemu.resolve()), base_temp_dir=str(out),
                     log_dir=str(out)) as vm:
        vm.set_machine('rx113-r5f51138')
        vm.set_console()
        vm.add_args('-kernel', str(args.elf.resolve()), '-d', 'in_asm',
                    '-D', str(out / 'execution.log'))
        vm.launch()
        if args.expect_stall:
            time.sleep(0.5)
            try:
                vm.console_socket.settimeout(0.1)
                data = vm.console_socket.recv(1024)
            except socket.timeout:
                data = b''
            assert not data, repr(data)
            results['console'] = ''
        else:
            results['console'] = receive(vm)
        vm.cmd('stop')
        results['registers'] = vm.cmd('human-monitor-command',
                                     command_line='info registers')
        results['system'] = {}
        for addr, size in [(0x80014, 'w'), (0x80020, 'w'),
                           (0x80026, 'h'), (0x80032, 'b'),
                           (0x80033, 'b'), (0x80036, 'b'),
                           (0x8003c, 'b'), (0x803fe, 'h')]:
            cmd = f'xp /1{size}x 0x{addr:x}'
            results['system'][hex(addr)] = vm.cmd(
                'human-monitor-command', command_line=cmd)
        if not args.expect_stall:
            vm.cmd('system_reset')
            vm.cmd('cont')
            results['console_after_reset'] = receive(vm)
    (out / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
