#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A guest program initializes the RX113 LCDC (R01UH0448EJ0120 section 34.5,
# external resistance division method) and fills the display data registers.
# The segment state is then read from outside the guest through QOM.

import struct

from qemu_test import QemuSystemTest, wait_for_console_pattern


class RX113Lcdc(QemuSystemTest):

    FLASH_BASE = 0xfff80000
    LCDC_BASE = 0xa0800
    NR_SEG = 40
    LCDM0 = 0x00
    LCDM1 = 0x01
    LCDC0 = 0x02
    VLCD = 0x03
    SEG00 = 0x40

    # SEGnn pattern: A-pattern in the low nibble, B-pattern in the high one.
    PATTERN = {0: 0x01, 1: 0x23, 11: 0x4f, 20: 0xa5, 39: 0x88}

    @staticmethod
    def store(offset, value):
        # mov.b #value,offset[r1]
        return bytes([0xf9, 0x14, offset, value])

    def program(self):
        # Release the LCDC module stop first, as on real hardware
        # (MSTPCRD b11 is set at reset). Hand-assembled, verified with
        # rx-elf-objdump:
        #   mov.l #0x803fe,r1; mov.w #0xa503,[r1]   (PRCR: unlock)
        #   mov.l #0x8001c,r1; mov.l #0xfffff7ff,r2; mov.l r2,[r1]
        code = bytes.fromhex('fb1efe0308f81903a5fb1e1c0008fb2afff7e312')
        # mov.l #LCDC_BASE,r1
        code += b'\xfb\x12' + struct.pack('<I', self.LCDC_BASE)
        # 4-time slice (LDTY = 3), 1/3 bias (LBAS = 1), waveform A, MDSET = 0
        code += self.store(self.LCDM0, 0x0d)
        for seg, value in self.PATTERN.items():
            code += self.store(self.SEG00 + seg, value)
        code += self.store(self.LCDC0, 0x11)
        code += self.store(self.LCDM1, 0xc0)    # SCOC = LCDON = 1, A-pattern
        # mov.l #0x8a000,r1; mov.b #0x20,2[r1]; mov.b #'R',3[r1]; bra.b .
        # Unlock PRC1 and release SCI0 module stop before transmitting.
        code += bytes.fromhex(
            'fb1efe0308f81902a5fb1e140008f812ffffff7f'
            'fb1efe0308f81900a5')
        code += bytes.fromhex('fb1200a00800 f9140220 f9140352 2e00')
        return code

    def make_elf(self, code):
        path = self.scratch_file('lcdc.elf')
        ident = b'\x7fELF\x01\x01\x01' + bytes(9)
        header = ident + struct.pack('<HHIIIIIHHHHHH',
                                     2, 173, 1, 0, 52, 0, 0,
                                     52, 32, 2, 0, 0, 0)
        text = struct.pack('<IIIIIIII', 1, 116, self.FLASH_BASE,
                           self.FLASH_BASE, len(code), len(code), 5, 1)
        vector = struct.pack('<IIIIIIII', 1, 116 + len(code),
                             0xfffffffc, 0xfffffffc, 4, 4, 4, 1)
        with open(path, 'wb') as image:
            image.write(header + text + vector + code)
            image.write(struct.pack('<I', self.FLASH_BASE))
        return path

    def qom(self, prop):
        return self.vm.cmd('qom-get', path='/machine/mcu/lcdc',
                           property=prop)

    def test_segment_state(self):
        self.vm.set_machine('rx113-r5f51138')
        self.vm.set_console()
        self.vm.add_args('-kernel', self.make_elf(self.program()))
        self.vm.launch()
        wait_for_console_pattern(self, 'R')
        self.vm.cmd('stop')

        ram = bytes.fromhex(self.qom('display-ram'))
        segments = bytes.fromhex(self.qom('segments'))
        self.assertEqual(len(ram), self.NR_SEG)
        self.assertEqual(len(segments), self.NR_SEG)
        for seg in range(self.NR_SEG):
            data = self.PATTERN.get(seg, 0)
            self.assertEqual(ram[seg], data, f'SEG{seg:02} data')
            self.assertEqual(segments[seg], data & 0xf, f'SEG{seg:02} lit')
        self.assertTrue(self.qom('driving'))
        self.assertEqual(self.qom('time-slices'), 4)
        self.assertEqual(self.qom('pattern'), 'A')

        # The guest's byte writes landed in the device registers as well.
        mem = self.vm.cmd('human-monitor-command',
                          command_line='xp /4xb 0xa0800')
        self.assertIn('0x0d', mem)
        self.assertIn('0xc0', mem)


if __name__ == '__main__':
    QemuSystemTest.main()
