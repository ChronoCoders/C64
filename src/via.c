#include "via.h"

#include <string.h>

// Register offsets (6522 datasheet, table 1). $1 accesses port A with the CA1/CA2
// handshake, $F accesses the same port without it.
#define R_ORB 0x0u
#define R_ORA 0x1u
#define R_DDRB 0x2u
#define R_DDRA 0x3u
#define R_T1CL 0x4u
#define R_T1CH 0x5u
#define R_T1LL 0x6u
#define R_T1LH 0x7u
#define R_T2CL 0x8u
#define R_T2CH 0x9u
#define R_SR 0xAu
#define R_ACR 0xBu
#define R_PCR 0xCu
#define R_IFR 0xDu
#define R_IER 0xEu
#define R_ORA_NH 0xFu

// ACR: T1 control in bits 6-7 (bit 7 = PB7 output, bit 6 = free-run), T2 control
// in bit 5 (0 = timed one-shot, 1 = count PB6 pulses). PCR: CA1 edge in bit 0,
// CB1 edge in bit 4 (1 = positive/rising, 0 = negative/falling).
#define ACR_T1_PB7 0x80u
#define ACR_T1_FREERUN 0x40u
#define ACR_T2_PULSE 0x20u
#define PCR_CA1_POS 0x01u
#define PCR_CB1_POS 0x10u

// Bit 7 of the IFR is a summary: set when any enabled flag (IFR & IER, bits 0-6)
// is set, and never written independently.
static void via_update_irq(VIA6522 *v) {
    if ((v->ifr & v->ier & 0x7Fu) != 0) {
        v->ifr |= VIA_IRQ_ANY;
    } else {
        v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_ANY);
    }
}

void via_reset(VIA6522 *v) {
    memset(v, 0, sizeof(*v));
    // The counters are undefined at reset; start them high so an unconfigured
    // timer does not underflow immediately.
    v->t1c = 0xFFFFu;
    v->t1l = 0xFFFFu;
    v->t2c = 0xFFFFu;
    // Handshake inputs and port pins idle high (open-collector lines are pulled up).
    v->ca1 = true;
    v->cb1 = true;
    v->pa_in = 0xFFu;
    v->pb_in = 0xFFu;
}

uint8_t via_read(VIA6522 *v, uint8_t reg) {
    switch (reg & 0x0Fu) {
        case R_ORB:
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_CB1);  // reading port B clears CB1
            via_update_irq(v);
            return (uint8_t)((v->orb & v->ddrb) | (v->pb_in & (uint8_t)~v->ddrb));
        case R_ORA:
            v->ifr = (uint8_t)(v->ifr & ~(VIA_IRQ_CA1 | VIA_IRQ_CA2));
            via_update_irq(v);
            return (uint8_t)((v->ora & v->ddra) | (v->pa_in & (uint8_t)~v->ddra));
        case R_ORA_NH:
            return (uint8_t)((v->ora & v->ddra) | (v->pa_in & (uint8_t)~v->ddra));
        case R_DDRB:
            return v->ddrb;
        case R_DDRA:
            return v->ddra;
        case R_T1CL:
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_T1);  // reading T1 low clears T1
            via_update_irq(v);
            return (uint8_t)(v->t1c & 0xFFu);
        case R_T1CH:
            return (uint8_t)(v->t1c >> 8);
        case R_T1LL:
            return (uint8_t)(v->t1l & 0xFFu);
        case R_T1LH:
            return (uint8_t)(v->t1l >> 8);
        case R_T2CL:
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_T2);  // reading T2 low clears T2
            via_update_irq(v);
            return (uint8_t)(v->t2c & 0xFFu);
        case R_T2CH:
            return (uint8_t)(v->t2c >> 8);
        case R_SR:
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_SR);
            via_update_irq(v);
            return v->sr;
        case R_ACR:
            return v->acr;
        case R_PCR:
            return v->pcr;
        case R_IFR:
            return v->ifr;  // bit 7 already reflects the summary
        case R_IER:
            return (uint8_t)(v->ier | 0x80u);  // bit 7 always reads 1
        default:
            return 0;
    }
}

void via_write(VIA6522 *v, uint8_t reg, uint8_t val) {
    switch (reg & 0x0Fu) {
        case R_ORB:
            v->orb = val;
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_CB1);  // writing port B clears CB1
            via_update_irq(v);
            break;
        case R_ORA:
            v->ora = val;
            v->ifr = (uint8_t)(v->ifr & ~(VIA_IRQ_CA1 | VIA_IRQ_CA2));
            via_update_irq(v);
            break;
        case R_ORA_NH:
            v->ora = val;
            break;
        case R_DDRB:
            v->ddrb = val;
            break;
        case R_DDRA:
            v->ddra = val;
            break;
        case R_T1CL:
        case R_T1LL:
            v->t1l = (uint16_t)((v->t1l & 0xFF00u) | val);  // both write the low latch
            break;
        case R_T1CH:
            // Latch high, load the counter, clear T1, start; PB7 goes low.
            v->t1l = (uint16_t)((v->t1l & 0x00FFu) | ((uint16_t)val << 8));
            v->t1c = v->t1l;
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_T1);
            v->t1_undf_pending = false;
            if (v->acr & ACR_T1_PB7) {
                v->pb7 = false;
            }
            via_update_irq(v);
            break;
        case R_T1LH:
            v->t1l = (uint16_t)((v->t1l & 0x00FFu) | ((uint16_t)val << 8));
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_T1);
            via_update_irq(v);
            break;
        case R_T2CL:
            v->t2l_lo = val;  // T2 has only a low latch
            break;
        case R_T2CH:
            v->t2c = (uint16_t)(((uint16_t)val << 8) | v->t2l_lo);
            v->ifr = (uint8_t)(v->ifr & ~VIA_IRQ_T2);
            v->t2_undf_pending = false;
            via_update_irq(v);
            break;
        case R_SR:
            v->sr = val;
            break;
        case R_ACR:
            v->acr = val;
            break;
        case R_PCR:
            v->pcr = val;
            break;
        case R_IFR:
            v->ifr = (uint8_t)(v->ifr & ~(val & 0x7Fu));  // write 1 to a bit clears it
            via_update_irq(v);
            break;
        case R_IER:
            if (val & 0x80u) {
                v->ier |= (uint8_t)(val & 0x7Fu);
            } else {
                v->ier = (uint8_t)(v->ier & ~(val & 0x7Fu));
            }
            via_update_irq(v);
            break;
        default:
            break;
    }
}

void via_step(VIA6522 *v) {
    // Timer 1 counts down every phi2. On underflow (passing through 0) the T1 flag
    // is set; in free-run it reloads from the latch and toggles PB7 each time, in
    // one-shot it raises the flag once and drives PB7 high.
    if (v->t1c == 0) {
        v->t1c = 0xFFFFu;
        if (v->acr & ACR_T1_FREERUN) {
            v->ifr |= VIA_IRQ_T1;
            v->t1c = v->t1l;
            v->pb7 = !v->pb7;
        } else if (!v->t1_undf_pending) {
            v->ifr |= VIA_IRQ_T1;
            v->t1_undf_pending = true;
            v->pb7 = true;
        }
    } else {
        v->t1c = (uint16_t)(v->t1c - 1);
    }

    // Timer 2 timed one-shot (ACR bit 5 = 0): counts down phi2 and raises the T2
    // flag once at underflow. Pulse counting on PB6 (bit 5 = 1) is not driven here.
    if ((v->acr & ACR_T2_PULSE) == 0) {
        if (v->t2c == 0) {
            v->t2c = 0xFFFFu;
            if (!v->t2_undf_pending) {
                v->ifr |= VIA_IRQ_T2;
                v->t2_undf_pending = true;
            }
        } else {
            v->t2c = (uint16_t)(v->t2c - 1);
        }
    }

    via_update_irq(v);
}

void via_set_ca1(VIA6522 *v, bool level) {
    bool active = (v->pcr & PCR_CA1_POS) ? (!v->ca1 && level) : (v->ca1 && !level);
    if (active) {
        v->ifr |= VIA_IRQ_CA1;
        via_update_irq(v);
    }
    v->ca1 = level;
}

void via_set_cb1(VIA6522 *v, bool level) {
    bool active = (v->pcr & PCR_CB1_POS) ? (!v->cb1 && level) : (v->cb1 && !level);
    if (active) {
        v->ifr |= VIA_IRQ_CB1;
        via_update_irq(v);
    }
    v->cb1 = level;
}

bool via_irq(const VIA6522 *v) {
    return (v->ifr & v->ier & 0x7Fu) != 0;
}
