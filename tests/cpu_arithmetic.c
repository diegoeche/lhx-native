#include "cpu.h"
#include <stdio.h>

/* Exhaust all byte inputs and carry-in values. The game uses carry chains
 * for its fixed-point flight arithmetic, so an error here can look plausible
 * on screen while changing the simulation. Reference results use host ints. */
int main(void)
{
    CPU cpu = {0};
    for (unsigned subtract = 0; subtract < 2; ++subtract)
    for (unsigned a = 0; a < 256; ++a)
    for (unsigned b = 0; b < 256; ++b)
    for (unsigned carry = 0; carry < 2; ++carry) {
        int sa = a < 128 ? (int)a : (int)a - 256;
        int sb = b < 128 ? (int)b : (int)b - 256;
        int signed_result = subtract ? sa - sb - (int)carry : sa + sb + (int)carry;
        int wide = subtract ? (int)a - (int)b - (int)carry : (int)a + (int)b + (int)carry;
        unsigned expected = (unsigned)wide & 255;
        unsigned bits = 0;
        for (unsigned i = 0; i < 8; ++i) bits += (expected >> i) & 1;
        unsigned expected_flags = (wide < 0 || wide > 255 ? FLAG_CF : 0)
            | (signed_result < -128 || signed_result > 127 ? FLAG_OF : 0)
            | (((a ^ b ^ expected) & 16) ? FLAG_AF : 0)
            | (expected == 0 ? FLAG_ZF : 0)
            | (expected & 128 ? FLAG_SF : 0)
            | (bits % 2 == 0 ? FLAG_PF : 0);
        cpu.flags = FLAG_IF | (carry ? FLAG_CF : 0);
        unsigned got = subtract ? flags_sbb(&cpu, a, b, 8) : flags_adc(&cpu, a, b, 8);
        if (got != expected || cpu.flags != (expected_flags | FLAG_IF)) {
            fprintf(stderr, "%s %02x,%02x carry=%u: %02x/%04x expected %02x/%04x\n",
                subtract ? "SBB" : "ADC", a, b, carry, got, cpu.flags,
                expected, expected_flags | FLAG_IF);
            return 1;
        }
    }
    puts("All 262144 byte carry-chain cases passed");
    return 0;
}
