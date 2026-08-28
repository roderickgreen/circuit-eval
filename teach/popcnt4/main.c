/* Exhaustive test of the bitsliced popcnt4, driven through the pack/unpack
 * wrappers in pack.c.
 *
 * popcnt4 has only 16 possible inputs and one bitsliced call runs 64
 * evaluations, so driving lane i with x = i & 15 covers the whole truth
 * table four times over in a single call.  Packing those values is also a
 * familiar sight: plane b is the bit-b column of the numbers 0..15
 * repeating, i.e. the truth-table constants 0xAAAA..., 0xCCCC...,
 * 0xF0F0..., 0xFF00... .
 */
#include <stdint.h>
#include <stdio.h>
#include "pack.h"

void popcnt4(const uint64_t *in, uint64_t *out);

int main(void) {
    uint8_t x[64], count[64], count2[64];
    uint64_t in[4], out[3];

    for (int lane = 0; lane < 64; lane++)
        x[lane] = lane & 15;

    popcnt4_pack(x, in);
    printf("input planes: x0=%016llx x1=%016llx\n"
           "              x2=%016llx x3=%016llx\n\n",
           (unsigned long long)in[0], (unsigned long long)in[1],
           (unsigned long long)in[2], (unsigned long long)in[3]);

    popcnt4(in, out);
    popcnt4_unpack(out, count);

    int fail = 0;
    printf(" x3 x2 x1 x0 | y2 y1 y0 | count\n");
    printf("-------------+----------+------\n");
    for (int lane = 0; lane < 16; lane++)
        printf("  %d  %d  %d  %d |  %d  %d  %d |   %d\n",
               lane >> 3 & 1, lane >> 2 & 1, lane >> 1 & 1, lane & 1,
               count[lane] >> 2 & 1, count[lane] >> 1 & 1, count[lane] & 1,
               count[lane]);
    for (int lane = 0; lane < 64; lane++)
        if (count[lane] != __builtin_popcount(x[lane])) {
            printf("lane %d: got %d want %d\n", lane, count[lane],
                   __builtin_popcount(x[lane]));
            fail = 1;
        }

    /* the one-call wrapper is the same three steps bundled */
    popcnt4_batch(x, count2);
    for (int lane = 0; lane < 64; lane++)
        if (count2[lane] != count[lane])
            fail = 1;

    printf(fail ? "FAIL\n" : "PASS: all 64 lanes correct\n");
    return fail;
}
