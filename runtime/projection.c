/* LHX Hit Squad CD native equivalent of its generated projection routine.
 *
 * Guarded by tools/lift.py's exact unpacked-image SHA-256. Image 0x18886
 * constructs the function at 0x186ca (relative CS 0x0fd1, IP 0x89ba) from
 * templates at 0x187f6, 0x1880b, 0x1881c and 0x1882a. No runtime instruction
 * decoding is used here. Another executable revision needs fresh analysis.
 */
#include "lifted.h"
#include <limits.h>

/* The original six-byte shortcut is MOV DH,DL / MOV DL,AH / MOV AH,AL.
 * It preserves AL, so replacing it with `value << 8` changes some quotients.
 * These unsigned operations also preserve the original 32-bit wraparound.
 */
static void left_one(CPU *cpu, uint16_t *ax, uint16_t *dx)
{
    unsigned carry = *ax >> 15;
    *ax = (uint16_t)(*ax << 1);
    set_szp16(cpu, *ax);
    unsigned next_carry = *dx >> 15;
    *dx = (uint16_t)((*dx << 1) | carry);
    cpu->flags &= ~(FLAG_CF | FLAG_OF);
    if (next_carry)
        cpu->flags |= FLAG_CF;
    if ((*dx >> 15) != next_carry)
        cpu->flags |= FLAG_OF;
}

static void right_one(CPU *cpu, uint16_t *ax, uint16_t *dx)
{
    unsigned carry = *dx & 1;
    *dx = (uint16_t)((*dx >> 1) | (*dx & 0x8000));
    set_szp16(cpu, *dx);
    unsigned next_carry = *ax & 1;
    unsigned overflow = (*ax >> 15) != carry;
    *ax = (uint16_t)((*ax >> 1) | (carry << 15));
    cpu->flags &= ~(FLAG_CF | FLAG_OF);
    if (next_carry)
        cpu->flags |= FLAG_CF;
    if (overflow)
        cpu->flags |= FLAG_OF;
}

static uint32_t scaled_numerator(CPU *cpu, uint16_t value, unsigned shift)
{
    uint16_t ax = value, dx = value & 0x8000 ? 0xffff : 0;
    if (shift < 6) {
        for (unsigned i = 0; i < shift; i++)
            left_one(cpu, &ax, &dx);
    } else {
        dx = (uint16_t)((dx & 255) << 8 | ax >> 8);
        ax = (uint16_t)((ax & 255) * 257);
        if (shift < 8) {
            for (unsigned i = shift; i < 8; i++)
                right_one(cpu, &ax, &dx);
        } else {
            for (unsigned i = 8; i < shift; i++)
                left_one(cpu, &ax, &dx);
        }
    }
    return ((uint32_t)dx << 16) | ax;
}

static int64_t signed_numerator(uint32_t bits)
{
    return bits & 0x80000000u ? (int64_t)bits - 0x100000000ll : bits;
}

/* IDIV rounds toward zero. LHX's installed INT 0 handler (0x18530) handles
 * overflow at these two specific division sites by returning +/-0x7f00;
 * unlike an ordinary IDIV, that fault leaves DX holding the numerator high
 * word. The handled-error marker is DS:07c0.
 */
static uint16_t divide_component(CPU *cpu, uint16_t value, unsigned shift,
                                 int16_t divisor)
{
    uint32_t bits = scaled_numerator(cpu, value, shift);
    int64_t numerator = signed_numerator(bits);
    cpu->dx = (uint16_t)(bits >> 16);
    if (divisor != 0) {
        int64_t quotient = numerator / divisor;
        if (quotient >= INT16_MIN && quotient <= INT16_MAX) {
            cpu->dx = (uint16_t)(numerator % divisor);
            return (uint16_t)quotient;
        }
    }
    mem_write8(cpu, cpu->ds, 0x07c0, 0);
    int negative = (int16_t)value < 0;
    if (divisor < 0)
        negative = !negative;
    return (uint16_t)(negative ? -0x7f00 : 0x7f00);
}

void lhx_project(CPU *cpu)
{
    /* The generated routine's first branch uses the fully compiled slow
     * fixed-point path when the engine requests it. It owns the same near
     * return frame and consumes it itself.
     */
    flags_cmp8(cpu, mem_read8(cpu, cpu->ds, 0xf1f8), 0);
    if (!zf(cpu)) {
        recomp_dispatch(cpu, (uint16_t)(g_load_seg + 0x0fd1), 0x8cf7);
        return;
    }

    unsigned shift = mem_read16(cpu, cpu->ds, 0xf02f);
    if (shift == 0 || shift > 37) {
        recomp_unhandled(cpu, cpu->cs, cpu->ip,
                         "projection shift exceeds original generated-code buffer");
        return;
    }
    uint16_t point = cpu->ax;
    uint16_t x = mem_read16(cpu, cpu->ds, (uint16_t)(point + 2));
    uint16_t y = mem_read16(cpu, cpu->ds, (uint16_t)(point + 6));
    int16_t z = (int16_t)mem_read16(cpu, cpu->ds, (uint16_t)(point + 10));
    uint16_t center_x = mem_read16(cpu, cpu->ds, 0xeb52);
    uint16_t center_y = mem_read16(cpu, cpu->ds, 0xeb54);

    uint16_t quotient_y = divide_component(cpu, y, shift, z);
    cpu->cx = flags_sub16(cpu, center_y, quotient_y);
    if (of(cpu))
        cpu->cx = 0x7f00;

    uint16_t quotient_x = divide_component(cpu, x, shift, z);
    if (z == 0) {
        /* The second divide-by-zero case redirects IRET to the generated
         * POP DI / POP SI / RET epilogue. Its handler returns both coordinates
         * directly, instead of executing the final ADD of center X.
         */
        cpu->ax = (int16_t)x > 0 ? 10000 : (int16_t)x < 0 ? (uint16_t)-10000 : center_x;
        cpu->cx = (int16_t)y > 0 ? 10000 : (int16_t)y < 0 ? (uint16_t)-10000 : center_y;
    } else {
        cpu->ax = flags_add16(cpu, quotient_x, center_x);
        if (of(cpu))
            cpu->ax = 0x7f00;
    }
    /* SI and DI are preserved by the original PUSH/POP pair. The host C
     * caller provides control flow; only its guest near-call word is removed.
     */
    cpu->sp = (uint16_t)(cpu->sp + 2);
}
