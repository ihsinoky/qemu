/*
 * Renesas RX113 R5F51138 machine
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/rx/rx113.h"
#include "system/system.h"
#include "system/reset.h"

#define TYPE_RX113_MACHINE MACHINE_TYPE_NAME("rx113-r5f51138")
OBJECT_DECLARE_SIMPLE_TYPE(RX113MachineState, RX113_MACHINE)

struct RX113MachineState {
    MachineState parent_obj;

    RX113State mcu;
};

static void rx113_init(MachineState *machine)
{
    RX113MachineState *s = RX113_MACHINE(machine);
    uint64_t flash_size, flash_base;

    if (machine->kernel_filename && machine->firmware) {
        error_report("Use either -kernel (ELF) or -bios (raw flash image)");
        exit(EXIT_FAILURE);
    }

    object_initialize_child(OBJECT(machine), "mcu", &s->mcu,
                            TYPE_R5F51138_MCU);
    object_property_set_link(OBJECT(&s->mcu), "main-bus",
                             OBJECT(get_system_memory()), &error_abort);
    qdev_realize(DEVICE(&s->mcu), NULL, &error_fatal);

    flash_size = memory_region_size(&s->mcu.c_flash);
    flash_base = (1ULL << 32) - flash_size;
    if (machine->kernel_filename) {
        uint64_t low, high;
        ssize_t size;

        /* RX reset reads 0xfffffffc; deliberately ignore the ELF entry. */
        size = load_elf(machine->kernel_filename, NULL, NULL, NULL,
                        NULL, &low, &high, NULL, ELFDATA2LSB, EM_RX, 0, 0);
        if (size <= 0) {
            error_report("Could not load RX ELF '%s': %s",
                         machine->kernel_filename, load_elf_strerror(size));
            exit(EXIT_FAILURE);
        }
        if (low < flash_base || high > (1ULL << 32)) {
            error_report("RX113 ELF load segments must fit in code flash");
            exit(EXIT_FAILURE);
        }
    } else if (machine->firmware) {
        if (load_image_targphys(machine->firmware, flash_base,
                                flash_size, NULL) < 0) {
            error_report("Could not load RX113 flash image '%s'",
                         machine->firmware);
            exit(EXIT_FAILURE);
        }
    }
}

static void rx113_reset(MachineState *machine, ResetType type)
{
    RX113MachineState *s = RX113_MACHINE(machine);
    const uint8_t *flash = memory_region_get_ram_ptr(&s->mcu.c_flash);
    uint64_t size = memory_region_size(&s->mcu.c_flash);

    qemu_devices_reset(type);
    /*
     * Read the vector after ROM restoration. The shared CPU reset handler
     * uses rom_ptr(), whose loader buffer is released for ROM regions.
     */
    s->mcu.cpu.env.pc = ldl_le_p(flash + size - 4);
}

static void rx113_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Renesas RX113 (R5F51138, internal flash and RAM)";
    mc->init = rx113_init;
    mc->reset = rx113_reset;
    /* RXv1 execution core; RX113 FPU restrictions are not yet modeled. */
    mc->default_cpu_type = TYPE_RX62N_CPU;
    mc->default_ram_size = 0;
    mc->max_cpus = 1;
}

static const TypeInfo rx113_machine_type = {
    .name = TYPE_RX113_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(RX113MachineState),
    .class_init = rx113_machine_class_init,
};

static void rx113_machine_register_types(void)
{
    type_register_static(&rx113_machine_type);
}

type_init(rx113_machine_register_types)
