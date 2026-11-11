/*
 * Renesas Serial Communication Interface
 *
 * Datasheet: RX62N Group, RX621 Group User's Manual: Hardware
 *            (Rev.1.40 R01UH0033EJ0140)
 *
 * Copyright (c) 2019 Yoshinori Sato
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/renesas_sci.h"
#include "migration/vmstate.h"

/* SCI register map */
REG8(SMR, 0)
  FIELD(SMR, CKS,  0, 2)
  FIELD(SMR, MP,   2, 1)
  FIELD(SMR, STOP, 3, 1)
  FIELD(SMR, PM,   4, 1)
  FIELD(SMR, PE,   5, 1)
  FIELD(SMR, CHR,  6, 1)
  FIELD(SMR, CM,   7, 1)
REG8(BRR, 1)
REG8(SCR, 2)
  FIELD(SCR, CKE,  0, 2)
  FIELD(SCR, TEIE, 2, 1)
  FIELD(SCR, MPIE, 3, 1)
  FIELD(SCR, RE,   4, 1)
  FIELD(SCR, TE,   5, 1)
  FIELD(SCR, RIE,  6, 1)
  FIELD(SCR, TIE,  7, 1)
REG8(TDR, 3)
REG8(SSR, 4)
  FIELD(SSR, MPBT, 0, 1)
  FIELD(SSR, MPB,  1, 1)
  FIELD(SSR, TEND, 2, 1)
  FIELD(SSR, ERR,  3, 3)
    FIELD(SSR, PER,  3, 1)
    FIELD(SSR, FER,  4, 1)
    FIELD(SSR, ORER, 5, 1)
  FIELD(SSR, RDRF, 6, 1)
  FIELD(SSR, TDRE, 7, 1)
REG8(RDR, 5)
REG8(SCMR, 6)
  FIELD(SCMR, SMIF, 0, 1)
  FIELD(SCMR, SINV, 2, 1)
  FIELD(SCMR, SDIR, 3, 1)
  FIELD(SCMR, BCP2, 7, 1)
REG8(SEMR, 7)
  FIELD(SEMR, ACS0, 0, 1)
  FIELD(SEMR, ABCS, 4, 1)

static void sci_clock_sync(RSCIState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = now - s->clock_tick;

    if (!s->module_stopped) {
        s->tx_remaining = elapsed > s->tx_remaining / s->input_freq ? 0 :
                          s->tx_remaining - elapsed * s->input_freq;
        s->rx_remaining = elapsed > s->rx_remaining / s->input_freq ? 0 :
                          s->rx_remaining - elapsed * s->input_freq;
        if (s->has_output && s->tx_active) {
            s->tx_bit_remaining =
                elapsed > s->tx_bit_remaining / s->input_freq ? 0 :
                s->tx_bit_remaining - elapsed * s->input_freq;
        }
    }
    s->clock_tick = now;
}

static int can_receive(void *opaque)
{
    RSCIState *sci = RSCI(opaque);
    if (sci->module_stopped) {
        return 0;
    }
    if (sci->rx_next > qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        return 0;
    } else {
        return FIELD_EX8(sci->scr, SCR, RE);
    }
}

static void receive(void *opaque, const uint8_t *buf, int size)
{
    RSCIState *sci = RSCI(opaque);
    if (sci->module_stopped) {
        return;
    }
    sci->rx_next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + sci->trtime;
    if (sci->clock_managed) {
        sci_clock_sync(sci);
        sci->rx_remaining = sci->trtime * sci->input_freq;
        timer_mod(&sci->rx_timer, sci->rx_next);
    }
    if (FIELD_EX8(sci->ssr, SSR, RDRF) || size > 1) {
        sci->ssr = FIELD_DP8(sci->ssr, SSR, ORER, 1);
        qemu_irq_pulse(sci->event[ERI]);
        if (FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_set_irq(sci->irq[ERI], 1);
        }
    } else {
        sci->rdr = buf[0];
        sci->ssr = FIELD_DP8(sci->ssr, SSR, RDRF, 1);
        qemu_irq_pulse(sci->event[RXI]);
        if (FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_irq_pulse(sci->irq[RXI]);
        }
    }
}

static void sci_txd_push(RSCIState *s)
{
    if (!s->has_output) {
        return;
    }
    qemu_set_irq(s->txd, s->tx_level);
    qemu_set_irq(s->txd_oe, FIELD_EX8(s->scr, SCR, TE) &&
                            !s->module_stopped);
}

/*
 * In clock synchronous mode SCR.CKE bit 1 selects the clock source
 * (section 28.2.6): 0 selects the internal baud-rate generator and the
 * SCK pin is a clock output; 1 selects an external clock input.
 */
static bool sci_sync_internal_clock(RSCIState *s)
{
    return !(FIELD_EX8(s->scr, SCR, CKE) & 2);
}

static void sci_sck_push(RSCIState *s)
{
    if (!s->has_output) {
        return;
    }
    qemu_set_irq(s->sck, s->sck_level);
    qemu_set_irq(s->sck_oe, FIELD_EX8(s->scr, SCR, TE) &&
                            FIELD_EX8(s->smr, SMR, CM) &&
                            sci_sync_internal_clock(s) &&
                            !s->module_stopped);
}

static unsigned sci_tx_data_bits(RSCIState *s)
{
    return 8 - FIELD_EX8(s->smr, SMR, CHR);
}

static unsigned sci_tx_frame_bits(RSCIState *s)
{
    unsigned bits = 1 + sci_tx_data_bits(s);

    if (FIELD_EX8(s->smr, SMR, PE) || FIELD_EX8(s->smr, SMR, MP)) {
        bits++;
    }
    return bits + 1 + FIELD_EX8(s->smr, SMR, STOP);
}

static bool sci_tx_bit_value(RSCIState *s, unsigned pos)
{
    unsigned data_bits = sci_tx_data_bits(s);

    if (pos == 0) {
        return false; /* start bit */
    }
    if (pos <= data_bits) {
        unsigned idx = FIELD_EX8(s->scmr, SCMR, SDIR) ?
                       data_bits - pos : pos - 1;
        bool value = (s->tx_shift >> idx) & 1;

        /* The manual's SCMR note: SINV inverts the data bits only. */
        return FIELD_EX8(s->scmr, SCMR, SINV) ? !value : value;
    }
    if (pos == data_bits + 1 &&
        (FIELD_EX8(s->smr, SMR, PE) || FIELD_EX8(s->smr, SMR, MP))) {
        if (FIELD_EX8(s->smr, SMR, MP)) {
            return FIELD_EX8(s->ssr, SSR, MPBT);
        }
        return FIELD_EX8(s->smr, SMR, PM) ?
               !__builtin_parity(s->tx_shift) :
               __builtin_parity(s->tx_shift);
    }
    return true; /* stop bit(s) */
}

static int64_t sci_tx_bit_duration(RSCIState *s, unsigned bit)
{
    return s->trtime * (bit + 1) / s->tx_slots -
           s->trtime * bit / s->tx_slots;
}

/*
 * The model's own slot count, mirroring update_trtime(): data bits plus
 * parity plus STOP+1. The start bit is not counted there, so one model
 * slot is exactly one bit time of the baud generator as this model
 * computes it (32 * BRR * 2^(2*CKS) / input clock, the Issue #10 baud
 * formula quirk included), while a serialized frame has one more bit
 * than the slot count. Frame deadlines therefore accumulate one bit
 * beyond trtime; TEND keeps the model's own timing.
 */
static unsigned sci_tx_slots(RSCIState *s)
{
    return sci_tx_data_bits(s) + FIELD_EX8(s->smr, SMR, PE) +
           FIELD_EX8(s->smr, SMR, STOP) + 1;
}

static void sci_tx_arm_bit(RSCIState *s)
{
    if (s->clock_managed) {
        s->tx_bit_remaining = sci_tx_bit_duration(s, s->tx_bit) *
                              s->input_freq;
    }
    timer_mod(&s->tx_bit_timer, s->tx_deadline);
}

/*
 * Clock-synchronous transmission (section 28.5): a frame is eight data
 * bits with no start, parity or stop bit. TXD changes on the falling
 * edges of SCK and holds the last bit after the frame; SCK idles high
 * and pulses once per bit, so the engine steps in half-bit phases.
 * One bit lasts one model slot, as on the asynchronous path.
 */
static bool sci_tx_sync_bit_value(RSCIState *s, unsigned pos)
{
    unsigned idx = FIELD_EX8(s->scmr, SCMR, SDIR) ? 7 - pos : pos;
    bool value = (s->tx_shift >> idx) & 1;

    return FIELD_EX8(s->scmr, SCMR, SINV) ? !value : value;
}

static void sci_tx_sync_arm(RSCIState *s, int64_t duration)
{
    if (s->clock_managed) {
        s->tx_bit_remaining = duration * s->input_freq;
    }
    timer_mod(&s->tx_bit_timer, s->tx_deadline);
}

static void sci_tx_sync_frame_start_at(RSCIState *s, uint8_t byte,
                                       int64_t start);

static void sci_tx_sync_expire(RSCIState *s)
{
    int64_t half = s->tx_sync_bit_ns / 2;

    if (s->tx_sync_rise) {
        /* Mid-bit: SCK rises; receivers sample on this edge. */
        s->sck_level = true;
        sci_sck_push(s);
        s->tx_sync_rise = false;
        s->tx_deadline += s->tx_sync_bit_ns - half;
        sci_tx_sync_arm(s, s->tx_sync_bit_ns - half);
        return;
    }
    s->tx_bit++;
    if (s->tx_bit >= s->tx_frame_bits) {
        s->tx_active = false;
        s->tx_sync = false;
        /* TXD holds the last bit and SCK stays high (section 28.5). */
        if (s->tx_pending_valid) {
            s->tx_pending_valid = false;
            sci_tx_sync_frame_start_at(s, s->tx_pending_shift,
                                       s->tx_deadline);
        }
        return;
    }
    /* The falling edge presents the next bit. */
    s->sck_level = false;
    s->tx_level = sci_tx_sync_bit_value(s, s->tx_bit);
    sci_sck_push(s);
    sci_txd_push(s);
    s->tx_sync_rise = true;
    s->tx_deadline += half;
    sci_tx_sync_arm(s, half);
}

static void sci_tx_sync_frame_start_at(RSCIState *s, uint8_t byte,
                                       int64_t start)
{
    s->tx_shift = byte;
    s->tx_frame_bits = 8;
    s->tx_slots = sci_tx_slots(s);
    s->tx_bit = 0;
    s->tx_active = true;
    s->tx_sync = true;
    s->tx_sync_bit_ns = s->tx_slots ? s->trtime / s->tx_slots : 0;
    s->tx_level = sci_tx_sync_bit_value(s, 0);
    s->sck_level = false; /* the frame opens with a falling edge */
    sci_txd_push(s);
    sci_sck_push(s);
    s->tx_sync_rise = true;
    s->tx_deadline = start + s->tx_sync_bit_ns / 2;
    sci_tx_sync_arm(s, s->tx_sync_bit_ns / 2);
}

static void sci_tx_sync_frame_start(RSCIState *s)
{
    /* Same pending-slot rule as the asynchronous path (see below). */
    if (s->tx_active) {
        s->tx_pending_shift = s->tdr;
        s->tx_pending_valid = true;
        return;
    }
    sci_tx_sync_frame_start_at(s, s->tdr,
                               qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void sci_tx_frame_start_at(RSCIState *s, uint8_t byte, int64_t start);

static void sci_tx_bit_expire(void *opaque)
{
    RSCIState *s = opaque;

    if (!s->tx_active) {
        return;
    }
    if (s->tx_sync) {
        sci_tx_sync_expire(s);
        return;
    }
    s->tx_bit++;
    if (s->tx_bit >= s->tx_frame_bits) {
        s->tx_active = false;
        s->tx_level = true; /* idle */
        sci_txd_push(s);
        if (s->tx_pending_valid) {
            /* The queued byte's frame follows with no idle gap. */
            s->tx_pending_valid = false;
            sci_tx_frame_start_at(s, s->tx_pending_shift, s->tx_deadline);
        }
        return;
    }
    s->tx_level = sci_tx_bit_value(s, s->tx_bit);
    sci_txd_push(s);
    /*
     * Chain from the previous deadline, not the callback time: under a
     * stepped clock a late callback must not stretch the frame.
     */
    s->tx_deadline += sci_tx_bit_duration(s, s->tx_bit);
    sci_tx_arm_bit(s);
}

static void sci_tx_frame_start_at(RSCIState *s, uint8_t byte, int64_t start)
{
    s->tx_shift = byte;
    s->tx_frame_bits = sci_tx_frame_bits(s);
    s->tx_slots = sci_tx_slots(s);
    s->tx_bit = 0;
    s->tx_active = true;
    s->tx_level = false; /* start bit */
    sci_txd_push(s);
    s->tx_deadline = start + sci_tx_bit_duration(s, 0);
    sci_tx_arm_bit(s);
}

static void sci_tx_frame_start(RSCIState *s)
{
    /*
     * send_byte() runs one model slot before the pad frame would end
     * (the model's trtime omits the start bit), so a byte handed over
     * while the previous frame is still on the pad waits in the pending
     * slot and follows with no idle gap, like a TSR behind a busy TDR
     * transfer on hardware.
     */
    if (s->tx_active) {
        s->tx_pending_shift = s->tdr;
        s->tx_pending_valid = true;
        return;
    }
    sci_tx_frame_start_at(s, s->tdr, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void send_byte(RSCIState *sci)
{
    if (sci->clock_managed) {
        sci_clock_sync(sci);
        sci->tx_remaining = sci->trtime * sci->input_freq;
    }
    if (qemu_chr_fe_backend_connected(&sci->chr)) {
        qemu_chr_fe_write_all(&sci->chr, &sci->tdr, 1);
    }
    timer_mod(&sci->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + sci->trtime);
    sci->ssr = FIELD_DP8(sci->ssr, SSR, TEND, 0);
    sci->ssr = FIELD_DP8(sci->ssr, SSR, TDRE, 1);
    qemu_set_irq(sci->irq[TEI], 0);
    qemu_irq_pulse(sci->event[TXI]);
    if (FIELD_EX8(sci->scr, SCR, TIE)) {
        qemu_irq_pulse(sci->irq[TXI]);
    }
    /*
     * The waveform rides the model's existing transfer timing: a frame
     * starts exactly when send_byte() runs (TDR write while idle, or the
     * queued byte at the previous frame's deadline). A synchronous
     * frame on an external clock advances on SCK input edges, which are
     * not modelled, so it is not serialized here.
     */
    if (sci->has_output) {
        if (!FIELD_EX8(sci->smr, SMR, CM)) {
            sci_tx_frame_start(sci);
        } else if (sci_sync_internal_clock(sci)) {
            sci_tx_sync_frame_start(sci);
        }
    }
}

static void txend(void *opaque)
{
    RSCIState *sci = RSCI(opaque);
    if (!FIELD_EX8(sci->ssr, SSR, TDRE)) {
        send_byte(sci);
    } else {
        sci->ssr = FIELD_DP8(sci->ssr, SSR, TEND, 1);
        qemu_irq_pulse(sci->event[TEI]);
        if (FIELD_EX8(sci->scr, SCR, TEIE)) {
            qemu_set_irq(sci->irq[TEI], 1);
        }
    }
}

static void update_trtime(RSCIState *sci)
{
    /* char per bits */
    sci->trtime = 8 - FIELD_EX8(sci->smr, SMR, CHR);
    sci->trtime += FIELD_EX8(sci->smr, SMR, PE);
    sci->trtime += FIELD_EX8(sci->smr, SMR, STOP) + 1;
    /* x bit transmit time (32 * divrate * brr) / base freq */
    sci->trtime *= 32 * sci->brr;
    sci->trtime *= 1 << (2 * FIELD_EX8(sci->smr, SMR, CKS));
    sci->trtime *= NANOSECONDS_PER_SECOND;
    sci->trtime /= sci->input_freq;
}

static bool sci_is_tr_enabled(RSCIState *sci)
{
    return FIELD_EX8(sci->scr, SCR, TE) || FIELD_EX8(sci->scr, SCR, RE);
}

static void sci_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    RSCIState *sci = RSCI(opaque);

    if (sci->module_stopped) {
        return;
    }
    switch (offset) {
    case A_SMR:
        if (!sci_is_tr_enabled(sci)) {
            sci->smr = val;
            update_trtime(sci);
        }
        break;
    case A_BRR:
        if (!sci_is_tr_enabled(sci)) {
            sci->brr = val;
            update_trtime(sci);
        }
        break;
    case A_SCR:
        if (!(sci->scr & R_SCR_TE_MASK) && (val & R_SCR_TE_MASK)) {
            qemu_irq_pulse(sci->event[TXI]);
        }
        sci->scr = val;
        if (FIELD_EX8(sci->scr, SCR, TE)) {
            sci->ssr = FIELD_DP8(sci->ssr, SSR, TDRE, 1);
            sci->ssr = FIELD_DP8(sci->ssr, SSR, TEND, 1);
            if (FIELD_EX8(sci->scr, SCR, TIE)) {
                qemu_irq_pulse(sci->irq[TXI]);
            }
        }
        if (!FIELD_EX8(sci->scr, SCR, TEIE)) {
            qemu_set_irq(sci->irq[TEI], 0);
        }
        if (!FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_set_irq(sci->irq[ERI], 0);
        }
        if (sci->has_output) {
            if (!FIELD_EX8(sci->scr, SCR, TE) && sci->tx_active) {
                sci->tx_active = false;
                sci->tx_sync = false;
                sci->tx_pending_valid = false;
                sci->tx_level = true;
                sci->sck_level = true;
                timer_del(&sci->tx_bit_timer);
            }
            sci_txd_push(sci);
            sci_sck_push(sci);
        }
        break;
    case A_TDR:
        sci->tdr = val;
        if (FIELD_EX8(sci->ssr, SSR, TEND)) {
            send_byte(sci);
        } else {
            sci->ssr = FIELD_DP8(sci->ssr, SSR, TDRE, 0);
        }
        break;
    case A_SSR:
        sci->ssr = FIELD_DP8(sci->ssr, SSR, MPBT,
                             FIELD_EX8(val, SSR, MPBT));
        sci->ssr = FIELD_DP8(sci->ssr, SSR, ERR,
                             FIELD_EX8(val, SSR, ERR) & 0x07);
        if (FIELD_EX8(sci->read_ssr, SSR, ERR) &&
            FIELD_EX8(sci->ssr, SSR, ERR) == 0) {
            qemu_set_irq(sci->irq[ERI], 0);
        }
        break;
    case A_RDR:
        qemu_log_mask(LOG_GUEST_ERROR, "reneas_sci: RDR is read only.\n");
        break;
    case A_SCMR:
        sci->scmr = val; break;
    case A_SEMR: /* SEMR */
        sci->semr = val; break;
    default:
        qemu_log_mask(LOG_UNIMP, "renesas_sci: Register 0x%" HWADDR_PRIX " "
                                 "not implemented\n",
                      offset);
    }
}

static uint64_t sci_read(void *opaque, hwaddr offset, unsigned size)
{
    RSCIState *sci = RSCI(opaque);

    if (sci->module_stopped) {
        return 0;
    }
    switch (offset) {
    case A_SMR:
        return sci->smr;
    case A_BRR:
        return sci->brr;
    case A_SCR:
        return sci->scr;
    case A_TDR:
        return sci->tdr;
    case A_SSR:
        sci->read_ssr = sci->ssr;
        return sci->ssr;
    case A_RDR:
        sci->ssr = FIELD_DP8(sci->ssr, SSR, RDRF, 0);
        return sci->rdr;
    case A_SCMR:
        return sci->scmr;
    case A_SEMR:
        return sci->semr;
    default:
        qemu_log_mask(LOG_UNIMP, "renesas_sci: Register 0x%" HWADDR_PRIX
                      " not implemented.\n", offset);
    }
    return UINT64_MAX;
}

static const MemoryRegionOps sci_ops = {
    .write = sci_write,
    .read  = sci_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.max_access_size = 1,
    .valid.max_access_size = 1,
};

/* Wake a backend that had been throttled by the managed receive clock. */
static void sci_rx_ready(void *opaque)
{
    RSCIState *s = opaque;

    if (!s->module_stopped) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

void renesas_sci_set_clock(RSCIState *s, uint64_t freq, bool stopped)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    assert(freq && freq <= NANOSECONDS_PER_SECOND);
    if (!s->legacy_input_freq) {
        s->legacy_input_freq = s->input_freq;
    }
    if (s->clock_managed && s->input_freq == freq &&
        s->module_stopped == stopped) {
        return;
    }
    if (s->clock_managed) {
        sci_clock_sync(s);
    } else {
        s->tx_remaining = timer_pending(&s->timer) ?
            MAX(0, timer_expire_time_ns(&s->timer) - now) * s->input_freq : 0;
        s->rx_remaining = MAX(0, s->rx_next - now) * s->input_freq;
        s->tx_bit_remaining = timer_pending(&s->tx_bit_timer) ?
            MAX(0, timer_expire_time_ns(&s->tx_bit_timer) - now) *
            s->input_freq : 0;
        s->clock_tick = now;
    }
    if (!s->module_stopped) {
        s->tx_pending = timer_pending(&s->timer);
    }
    timer_del(&s->timer);
    timer_del(&s->rx_timer);
    timer_del(&s->tx_bit_timer);
    s->clock_managed = true;
    s->input_freq = freq;
    s->module_stopped = stopped;
    update_trtime(s);
    if (!stopped) {
        s->rx_next = now + DIV_ROUND_UP(s->rx_remaining, freq);
        if (s->tx_pending) {
            timer_mod(&s->timer, now + DIV_ROUND_UP(s->tx_remaining, freq));
        }
        timer_mod(&s->rx_timer, s->rx_next);
        if (s->has_output && s->tx_active) {
            s->tx_deadline = now + DIV_ROUND_UP(s->tx_bit_remaining, freq);
            timer_mod(&s->tx_bit_timer, s->tx_deadline);
        }
    }
    sci_txd_push(s);
    sci_sck_push(s);
}

static void rsci_reset(DeviceState *dev)
{
    RSCIState *sci = RSCI(dev);

    if (sci->clock_managed) {
        timer_del(&sci->timer);
        timer_del(&sci->rx_timer);
        sci->clock_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        sci->tx_pending = false;
        sci->tx_remaining = sci->rx_remaining = 0;
    }
    timer_del(&sci->tx_bit_timer);
    sci->tx_active = false;
    sci->tx_sync = false;
    sci->tx_level = true;
    sci->sck_level = true;
    sci->tx_bit = 0;
    sci->smr = sci->scr = 0x00;
    sci->brr = 0xff;
    sci->tdr = 0xff;
    sci->rdr = 0x00;
    sci->ssr = 0x84;
    sci->scmr = 0x00;
    sci->semr = 0x00;
    sci->rx_next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (sci->clock_managed) {
        update_trtime(sci);
    }
    sci_txd_push(sci);
    sci_sck_push(sci);
}

static void sci_event(void *opaque, QEMUChrEvent event)
{
    RSCIState *sci = RSCI(opaque);
    if (!sci->module_stopped && event == CHR_EVENT_BREAK) {
        sci->ssr = FIELD_DP8(sci->ssr, SSR, FER, 1);
        qemu_irq_pulse(sci->event[ERI]);
        if (FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_set_irq(sci->irq[ERI], 1);
        }
    }
}

static void rsci_realize(DeviceState *dev, Error **errp)
{
    RSCIState *sci = RSCI(dev);

    if (sci->input_freq == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas_sci: input-freq property must be set.");
        return;
    }
    qemu_chr_fe_set_handlers(&sci->chr, can_receive, receive,
                             sci_event, NULL, sci, NULL, true);
}

static void rsci_init(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    RSCIState *sci = RSCI(obj);
    int i;

    memory_region_init_io(&sci->memory, OBJECT(sci), &sci_ops,
                          sci, "renesas-sci", 0x8);
    sysbus_init_mmio(d, &sci->memory);

    for (i = 0; i < SCI_NR_IRQ; i++) {
        sysbus_init_irq(d, &sci->irq[i]);
    }
    qdev_init_gpio_out_named(DEVICE(obj), sci->event, "event", SCI_NR_IRQ);
    qdev_init_gpio_out_named(DEVICE(obj), &sci->txd, "txd", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &sci->txd_oe, "txd-oe", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &sci->sck, "sck", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &sci->sck_oe, "sck-oe", 1);
    timer_init_ns(&sci->timer, QEMU_CLOCK_VIRTUAL, txend, sci);
    timer_init_ns(&sci->tx_bit_timer, QEMU_CLOCK_VIRTUAL,
                  sci_tx_bit_expire, sci);
    timer_init_ns(&sci->rx_timer, QEMU_CLOCK_VIRTUAL, sci_rx_ready, sci);
}

static int sci_pre_load(void *opaque)
{
    RSCIState *s = opaque;

    /* An absent clock subsection selects the original realization clock. */
    if (s->legacy_input_freq) {
        s->input_freq = s->legacy_input_freq;
    }
    s->clock_managed = false;
    s->module_stopped = false;
    timer_del(&s->rx_timer);
    return 0;
}

static int sci_clock_post_load(void *opaque, int version_id)
{
    RSCIState *s = opaque;

    if (!s->clock_managed || !s->input_freq ||
        s->input_freq > NANOSECONDS_PER_SECOND) {
        return -EINVAL;
    }
    if (s->tx_remaining < 0 || s->rx_remaining < 0) {
        return -EINVAL;
    }
    return 0;
}

static bool sci_clock_needed(void *opaque)
{
    RSCIState *s = opaque;

    return s->clock_managed;
}

static const VMStateDescription vmstate_sci_clock = {
    .name = "renesas-sci/clock",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sci_clock_needed,
    .post_load = sci_clock_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(clock_managed, RSCIState),
        VMSTATE_BOOL(module_stopped, RSCIState),
        VMSTATE_UINT64(input_freq, RSCIState),
        VMSTATE_INT64(clock_tick, RSCIState),
        VMSTATE_INT64(tx_remaining, RSCIState),
        VMSTATE_INT64(rx_remaining, RSCIState),
        VMSTATE_BOOL(tx_pending, RSCIState),
        VMSTATE_TIMER(rx_timer, RSCIState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sci_output_needed(void *opaque)
{
    RSCIState *s = opaque;

    return s->has_output;
}

static int sci_output_post_load(void *opaque, int version_id)
{
    RSCIState *s = opaque;

    sci_txd_push(s);
    sci_sck_push(s);
    return 0;
}

static const VMStateDescription vmstate_sci_output = {
    .name = "renesas-sci/output",
    .version_id = 2,
    .minimum_version_id = 1,
    .needed = sci_output_needed,
    .post_load = sci_output_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(tx_shift, RSCIState),
        VMSTATE_UINT8(tx_bit, RSCIState),
        VMSTATE_UINT8(tx_frame_bits, RSCIState),
        VMSTATE_UINT8(tx_slots, RSCIState),
        VMSTATE_BOOL(tx_pending_valid, RSCIState),
        VMSTATE_UINT8(tx_pending_shift, RSCIState),
        VMSTATE_BOOL(tx_active, RSCIState),
        VMSTATE_BOOL(tx_level, RSCIState),
        VMSTATE_INT64(tx_bit_remaining, RSCIState),
        VMSTATE_INT64(tx_deadline, RSCIState),
        VMSTATE_TIMER(tx_bit_timer, RSCIState),
        VMSTATE_BOOL_V(tx_sync, RSCIState, 2),
        VMSTATE_BOOL_V(tx_sync_rise, RSCIState, 2),
        VMSTATE_INT64_V(tx_sync_bit_ns, RSCIState, 2),
        VMSTATE_BOOL_V(sck_level, RSCIState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rsci = {
    .name = "renesas-sci",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = sci_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(trtime, RSCIState),
        VMSTATE_INT64(rx_next, RSCIState),
        VMSTATE_UINT8(smr, RSCIState),
        VMSTATE_UINT8(brr, RSCIState),
        VMSTATE_UINT8(scr, RSCIState),
        VMSTATE_UINT8(tdr, RSCIState),
        VMSTATE_UINT8(ssr, RSCIState),
        VMSTATE_UINT8(rdr, RSCIState),
        VMSTATE_UINT8(scmr, RSCIState),
        VMSTATE_UINT8(semr, RSCIState),
        VMSTATE_UINT8(read_ssr, RSCIState),
        VMSTATE_TIMER(timer, RSCIState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const[]) {
        &vmstate_sci_clock,
        &vmstate_sci_output,
        NULL
    },
};

static const Property rsci_properties[] = {
    DEFINE_PROP_UINT64("input-freq", RSCIState, input_freq, 0),
    DEFINE_PROP_BOOL("has-output", RSCIState, has_output, false),
    DEFINE_PROP_CHR("chardev", RSCIState, chr),
};

static void rsci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rsci_realize;
    dc->vmsd = &vmstate_rsci;
    device_class_set_legacy_reset(dc, rsci_reset);
    device_class_set_props(dc, rsci_properties);
}

static const TypeInfo rsci_info = {
    .name = TYPE_RENESAS_SCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RSCIState),
    .instance_init = rsci_init,
    .class_init = rsci_class_init,
};

static void rsci_register_types(void)
{
    type_register_static(&rsci_info);
}

type_init(rsci_register_types)
