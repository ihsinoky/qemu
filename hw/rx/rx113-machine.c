/*
 * Renesas RX113 machines
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

#define TYPE_RX113_MACHINE MACHINE_TYPE_NAME("rx113")
OBJECT_DECLARE_SIMPLE_TYPE(RX113MachineState, RX113_MACHINE)

typedef struct RX113MachineClass {
    MachineClass parent_class;
    const char *mcu_type;
} RX113MachineClass;

DECLARE_CLASS_CHECKERS(RX113MachineClass, RX113_MACHINE,
                       TYPE_RX113_MACHINE)

struct RX113MachineState {
    MachineState parent_obj;

    RX113State mcu;
};

static void rx113_init(MachineState *machine)
{
    RX113MachineState *s = RX113_MACHINE(machine);
    RX113MachineClass *rmc = RX113_MACHINE_GET_CLASS(machine);
    uint64_t flash_size, flash_base;

    if (machine->kernel_filename && machine->firmware) {
        error_report("Use either -kernel (ELF) or -bios (raw flash image)");
        exit(EXIT_FAILURE);
    }

    object_initialize_child(OBJECT(machine), "mcu", &s->mcu,
                            rmc->mcu_type);
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

    mc->init = rx113_init;
    mc->reset = rx113_reset;
    /* RXv1 execution core without FPU */
    mc->default_cpu_type = TYPE_RX113_CPU;
    mc->default_ram_size = 0;
    mc->max_cpus = 1;
}

static void rx113_r5f51135_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RX113MachineClass *rmc = RX113_MACHINE_CLASS(oc);

    mc->desc = "Renesas RX113 (R5F51135, internal flash and RAM)";
    rmc->mcu_type = TYPE_R5F51135_MCU;
}

static void rx113_r5f51136_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RX113MachineClass *rmc = RX113_MACHINE_CLASS(oc);

    mc->desc = "Renesas RX113 (R5F51136, internal flash and RAM)";
    rmc->mcu_type = TYPE_R5F51136_MCU;
}

static void rx113_r5f51137_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RX113MachineClass *rmc = RX113_MACHINE_CLASS(oc);

    mc->desc = "Renesas RX113 (R5F51137, internal flash and RAM)";
    rmc->mcu_type = TYPE_R5F51137_MCU;
}

static void rx113_r5f51138_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RX113MachineClass *rmc = RX113_MACHINE_CLASS(oc);

    mc->desc = "Renesas RX113 (R5F51138, internal flash and RAM)";
    rmc->mcu_type = TYPE_R5F51138_MCU;
}

static const TypeInfo rx113_machine_types[] = {
    {
        .name = TYPE_RX113_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(RX113MachineState),
        .class_size = sizeof(RX113MachineClass),
        .class_init = rx113_machine_class_init,
        .abstract = true,
    }, {
        .name = MACHINE_TYPE_NAME("rx113-r5f51135"),
        .parent = TYPE_RX113_MACHINE,
        .class_init = rx113_r5f51135_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("rx113-r5f51136"),
        .parent = TYPE_RX113_MACHINE,
        .class_init = rx113_r5f51136_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("rx113-r5f51137"),
        .parent = TYPE_RX113_MACHINE,
        .class_init = rx113_r5f51137_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("rx113-r5f51138"),
        .parent = TYPE_RX113_MACHINE,
        .class_init = rx113_r5f51138_class_init,
    }
};

DEFINE_TYPES(rx113_machine_types)
