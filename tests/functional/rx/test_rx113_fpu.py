#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# RX113 has no FPU (R01UH0448EJ0120 section 2.2.2 lists no FPSW and the
# hardware manual defines no FPU instructions), so FPU instructions must
# raise the undefined instruction exception (fixed vector 0xffffffdc, saved
# PC = the faulting instruction). RX62N has an FPU and must keep executing
# them.
#
# Each case runs "prologue; <insn under test>; epilogue" from the flash
# reset entry. The final marker is left in R6:
#   'N' insn executed and fell through   'U' undefined instruction vector
#   'B' vector taken with a wrong saved PC
#   'Z' FPSW reads back 0 after a write  'D' FPSW kept the written value
#   'M' FADD produced a wrong result

import struct
import time

from qemu_test import QemuSystemTest


class RX113FpuRestriction(QemuSystemTest):

    FLASH_BASE = 0xfff80000
    HANDLER_OFF = 0x100
    VECTOR_BASE = 0xffffffdc

    # mov.l #0x1000,r0; mov.l #0x3f800000(1.0f),r2; mov.l #0x40000000,r3
    PROLOGUE = bytes.fromhex('fb0a0010 fb220000803f fb3200000040')
    # mov.l #'N',r6; bra.b .
    EPILOGUE = bytes.fromhex('75464e 2e00')
    # mov.l [r0],r3; mov.l #0xfff80010,r4; cmp r4,r3; bne.b 1f
    # mov.l #'U',r6; bra.b .; 1: mov.l #'B',r6; bra.b .
    HANDLER = bytes.fromhex('ec03 fb4e1000f8 4743 2107 754655 2e00'
                            '754642 2e00')

    INSNS = {
        'fadd #imm': 'fd7222 0000803f',
        'fsub #imm': 'fd7202 0000803f',
        'fmul #imm': 'fd7232 0000803f',
        'fdiv #imm': 'fd7242 0000803f',
        'fcmp #imm': 'fd7212 0000803f',
        'fadd rs': 'fc8b32',
        'fsub rs': 'fc8332',
        'fmul rs': 'fc8f32',
        'fdiv rs': 'fc9332',
        'fcmp rs': 'fc8732',
        'ftoi': 'fc9724',
        'round': 'fc9b24',
        'itof': 'fc4724',
    }
    # Self-contained bodies that report their own result marker.
    # mvtc #1,fpsw; mvfc fpsw,r5; cmp #0,r5; bne.b 1f; mov.l #'Z',r6;
    # bra.b .; 1: mov.l #'D',r6; bra.b .
    FPSW_BODY = bytes.fromhex('fd770301 fd6a35 6105 2107 75465a 2e00'
                              '754644 2e00')
    # fadd #1.0,r2; cmp #2.0,r2; bne.b 1f; mov.l #'N',r6; bra.b .;
    # 1: mov.l #'M',r6; bra.b .
    FADD_BODY = bytes.fromhex('fd7222 0000803f 740200000040 2107 75464e 2e00'
                              '75464d 2e00')

    def make_code(self, body, final):
        code = bytearray(self.HANDLER_OFF + len(self.HANDLER))
        code[:len(self.PROLOGUE)] = self.PROLOGUE
        off = len(self.PROLOGUE)
        code[off:off + len(body)] = body
        if final:
            off += len(body)
            code[off:off + len(self.EPILOGUE)] = self.EPILOGUE
        code[self.HANDLER_OFF:] = self.HANDLER
        return code

    def make_elf(self, body, final):
        path = self.scratch_file('fpu.elf')
        code = self.make_code(body, final)
        vectors = bytearray(0x24)
        struct.pack_into('<I', vectors, 0,
                         self.FLASH_BASE + self.HANDLER_OFF)
        struct.pack_into('<I', vectors, 0x20, self.FLASH_BASE)
        ident = b'\x7fELF\x01\x01\x01' + bytes(9)
        header = ident + struct.pack('<HHIIIIIHHHHHH',
                                     2, 173, 1, self.FLASH_BASE, 52, 0, 0,
                                     52, 32, 2, 0, 0, 0)
        phs = struct.pack('<IIIIIIII', 1, 116, self.FLASH_BASE,
                          self.FLASH_BASE, len(code), len(code), 5, 1)
        phs += struct.pack('<IIIIIIII', 1, 116 + len(code), self.VECTOR_BASE,
                           self.VECTOR_BASE, len(vectors), len(vectors), 4, 1)
        with open(path, 'wb') as image:
            image.write(header + phs + code + vectors)
        return path

    def run_case(self, machine, body, final=True):
        self.vm.set_machine(machine)
        if machine.startswith('gdbsim'):
            # gdbsim loads -kernel as a raw image and starts at its first byte
            path = self.scratch_file('fpu.bin')
            with open(path, 'wb') as image:
                image.write(self.make_code(body, final))
        else:
            path = self.make_elf(body, final)
        self.vm.add_args('-kernel', path, '-no-reboot')
        self.vm.launch()
        marker = None
        for _ in range(50):
            time.sleep(0.2)
            regs = self.vm.cmd('human-monitor-command',
                               command_line='info registers')
            for line in regs.splitlines():
                if 'R6' in line.upper():
                    value = int(line.upper().split('R6=')[1].split()[0], 16)
                    if value:
                        marker = chr(value)
            if marker:
                break
        self.vm.shutdown()
        return marker

    def check_all(self, machine, expect_fpu):
        for name, insn in self.INSNS.items():
            with self.subTest(machine=machine, insn=name):
                got = self.run_case(machine, bytes.fromhex(insn))
                self.assertEqual(got, 'N' if expect_fpu else 'U')
        with self.subTest(machine=machine, insn='fpsw'):
            got = self.run_case(machine, self.FPSW_BODY, final=False)
            self.assertEqual(got, 'D' if expect_fpu else 'Z')
        with self.subTest(machine=machine, insn='fadd result'):
            got = self.run_case(machine, self.FADD_BODY, final=False)
            self.assertEqual(got, 'N' if expect_fpu else 'U')

    def test_rx113_no_fpu(self):
        self.check_all('rx113-r5f51138', False)

    def test_rx62n_fpu(self):
        self.check_all('gdbsim-r5f562n8', True)


if __name__ == '__main__':
    QemuSystemTest.main()
