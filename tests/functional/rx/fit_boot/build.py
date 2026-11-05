#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build an RX113 probe with genuine, unmodified Renesas R_BSP v7.71.

Downloads a pinned BSD-3-Clause package from Renesas' official repository.
Requires rx-elf-gcc and binutils on PATH; all output stays in the given dir.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import urllib.request
import zipfile

REV = 'bcc409d331b0bff68d4b09bf6fc41376f75d42bc'
URL = (f'https://raw.githubusercontent.com/renesas/rx-driver-package/{REV}/'
       'FITModules/r_bsp_v7.71.zip')
SHA256 = 'f00271622c11722e3de7bb73c7c426cd0f89a2390f880d909368187891971fe0'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--hoco', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    archive = out / 'r_bsp.zip'
    if not archive.exists():
        urllib.request.urlretrieve(URL, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('R_BSP archive SHA-256 mismatch: ' + str(archive))
    with zipfile.ZipFile(archive) as package:
        package.extractall(out / 'fit')
    bsp = out / 'fit/r_bsp'
    board = bsp / 'board/generic_rx113'
    config = (board / 'r_bsp_config_reference.h').read_text()
    settings = {'BSP_CFG_IO_LIB_ENABLE': 0,
                'BSP_CFG_HOCO_OSCILLATE_ENABLE': int(args.hoco)}
    if args.hoco:
        settings['BSP_CFG_CLOCK_SOURCE'] = 1
        settings['BSP_CFG_USB_CLOCK_SOURCE'] = 0
    for key, value in settings.items():
        config, count = re.subn(r'(#define\s+' + key + r'\s+)\([^\n]*\)',
                                rf'\g<1>({value})', config)
        assert count == 1, key
    (out / 'r_bsp_config.h').write_text(config)
    # Select the board without editing the package's platform selector.
    (out / 'platform.h').write_text(
        '#ifndef PLATFORM_H\n#define PLATFORM_H\n'
        '#include "board/generic_rx113/r_bsp.h"\n#endif\n')
    # No libc is linked or used. R_BSP includes stdio.h unconditionally;
    # this empty test-only header supplies no functions or fake behavior.
    (out / 'stdio.h').write_text('/* No stdio in this freestanding probe. */\n')
    # fsp_common_api.h includes assert.h but never uses assert().
    (out / 'assert.h').write_text('/* No assert calls in the probe. */\n')
    sources = [bsp / 'mcu/all' / name for name in (
        'reset_program.S', 'resetprg.c', 'r_bsp_cpu.c',
        'r_bsp_interrupts.c', 'r_bsp_mcu_startup.c', 'r_bsp_common.c',
        'r_rx_intrinsic_functions.c', 'mcu_locks.c')]
    sources += sorted((bsp / 'mcu/rx113').glob('*.c'))
    sources += [board / 'hwsetup.c', here / 'main.c']
    flags = ['-mcpu=rx100', '-mlittle-endian-data', '-mnofpu',
             '-ffreestanding', '-Os', '-g', '-ffunction-sections',
             '-fdata-sections', '-I' + str(out), '-I' + str(bsp),
             '-I' + str(board)]
    objects = []
    for source in sources:
        obj = out / (source.stem + '.o')
        subprocess.run(['rx-elf-gcc', *flags, '-c', str(source),
                        '-o', str(obj)], check=True)
        objects.append(str(obj))
    subprocess.run(['rx-elf-gcc', *flags, '-nostdlib',
                    '-Wl,--gc-sections', '-Wl,-Map=' + str(out / 'boot.map'),
                    '-T', str(here / 'link.ld'), *objects, '-lgcc',
                    '-o', str(out / 'boot.elf')], check=True)
    with (out / 'boot.dis').open('w') as output:
        subprocess.run(['rx-elf-objdump', '-d', str(out / 'boot.elf')],
                        stdout=output, check=True)
    with (out / 'boot.nm').open('w') as output:
        subprocess.run(['rx-elf-nm', '-n', str(out / 'boot.elf')],
                        stdout=output, check=True)
    print(out / 'boot.elf')


if __name__ == '__main__':
    main()
