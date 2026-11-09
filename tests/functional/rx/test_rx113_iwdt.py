#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import re
import struct
import time

from qemu_test import QemuSystemTest


class RX113IwdtSleep(QemuSystemTest):
    """Execute RX WAIT and check the OFS0 sleep count-stop selection."""

    def check_sleep(self, stop):
        image = bytearray(512 * 1024)
        # WAIT; BRA.B . -- no peripheral or shared CPU test hooks.
        image[:4] = bytes.fromhex('7f96 2e00')
        # Auto-start, /1, 128 cycles, full window, WUNI output.
        ofs = 0xffffef01 if stop else 0xffffaf01
        struct.pack_into('<I', image, len(image) - 0x74, ofs)
        struct.pack_into('<I', image, len(image) - 4, 0xfff80000)
        path = self.scratch_file('iwdt-sleep.bin')
        with open(path, 'wb') as output:
            output.write(image)
        self.vm.set_machine('rx113-r5f51138')
        self.vm.add_args('-bios', path, '-icount', 'shift=0,sleep=off')
        self.vm.launch()
        time.sleep(0.2)  # many nominal 8.53 ms periods with the CPU in WAIT
        self.vm.cmd('stop')
        registers = self.vm.cmd('human-monitor-command',
                                command_line='info registers')
        self.assertIn('pc=0xfff80002', registers.lower())
        status = self.vm.cmd('human-monitor-command',
                            command_line='xp /1hx 0x88034')
        value = int(re.findall(r'0x([0-9a-fA-F]+)', status)[-1], 16)
        if stop:
            self.assertEqual(value & 0xc000, 0)
            self.vm.cmd('cont')
            time.sleep(0.1)
            self.vm.cmd('stop')
            later = self.vm.cmd('human-monitor-command',
                                command_line='xp /1hx 0x88034')
            self.assertEqual(int(re.findall(r'0x([0-9a-fA-F]+)', later)[-1],
                                 16), value)
        else:
            self.assertEqual(value & 0xc000, 0x4000)

    def test_sleep_stop(self):
        self.check_sleep(True)

    def test_sleep_continue(self):
        self.check_sleep(False)


if __name__ == '__main__':
    QemuSystemTest.main()
