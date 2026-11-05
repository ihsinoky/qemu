#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import time

from qemu_test import QemuSystemTest, wait_for_console_pattern


class RX113Machine(QemuSystemTest):
    """Boot from flash's reset vector and transmit on SCI0 without assets."""

    FLASH_BASE = 0xfff80000
    FLASH_SIZE = 512 * 1024
    # mov.l #0x8a000,r1; mov.b #0x20,2[r1]; mov.b #'R',3[r1]; bra.b .
    PROGRAM = bytes.fromhex('fb1200a00800 f9140220 f9140352 2e00')

    def make_elf(self):
        path = self.scratch_file('rx113.elf')
        ident = b'\x7fELF\x01\x01\x01' + bytes(9)
        # e_entry=0 intentionally: the hardware reset vector must win.
        header = ident + struct.pack('<HHIIIIIHHHHHH',
                                     2, 173, 1, 0, 52, 0, 0,
                                     52, 32, 2, 0, 0, 0)
        code = struct.pack('<IIIIIIII', 1, 116, self.FLASH_BASE,
                           self.FLASH_BASE, len(self.PROGRAM),
                           len(self.PROGRAM), 5, 1)
        vector = struct.pack('<IIIIIIII', 1, 116 + len(self.PROGRAM),
                             0xfffffffc, 0xfffffffc, 4, 4, 4, 1)
        with open(path, 'wb') as image:
            image.write(header + code + vector + self.PROGRAM)
            image.write(struct.pack('<I', self.FLASH_BASE))
        return path

    def check_boot(self, option, path):
        self.vm.set_machine('rx113-r5f51138')
        self.vm.set_console()
        self.vm.add_args(option, path)
        self.vm.launch()
        wait_for_console_pattern(self, 'R')
        time.sleep(3)
        self.assertEqual(self.vm.cmd('query-status')['status'], 'running')
        self.vm.cmd('stop')
        registers = self.vm.cmd('human-monitor-command',
                                command_line='info registers')
        self.assertIn('pc=0xfff8000e', registers.lower())
        self.vm.cmd('system_reset')
        self.vm.cmd('cont')
        wait_for_console_pattern(self, 'R')

    def test_elf(self):
        self.check_boot('-kernel', self.make_elf())

    def test_raw_flash(self):
        path = self.scratch_file('rx113.bin')
        image = bytearray(self.FLASH_SIZE)
        image[:len(self.PROGRAM)] = self.PROGRAM
        struct.pack_into('<I', image, self.FLASH_SIZE - 4, self.FLASH_BASE)
        with open(path, 'wb') as output:
            output.write(image)
        self.check_boot('-bios', path)


if __name__ == '__main__':
    QemuSystemTest.main()
