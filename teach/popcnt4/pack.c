/* Readable pack/unpack for the bitsliced popcnt4 -- see pack.h.
 *
 * Both directions are the same operation: a bit-matrix transpose.  The
 * caller's array is 64 rows (lanes) of a few bits each; the kernel wants
 * the columns of that matrix, one uint64_t per input bit.  At this size
 * the obvious double loop -- pick a bit, drop it where it belongs -- is
 * all it takes.
 *
 * The production front end (lib/bspack/) does exactly the
 * same job for the poker circuits: 256 hands of 52 bits each, in and out
 * on every 3 ns evaluation.  There the per-bit loop would cost more than
 * the evaluator itself, so the transpose runs as six stages of masked
 * swaps that move 32 bits per instruction.  Read that file after this
 * one: every line of it is an optimization of the loops below.
 */
#include "pack.h"

void popcnt4(const uint64_t *in, uint64_t *out);

/* planes[b] bit l = bit b of x[l]: column b of the input matrix */
void popcnt4_pack(const uint8_t x[64], uint64_t planes[4])
{
    for (int b = 0; b < 4; b++) {
        uint64_t w = 0;
        for (int lane = 0; lane < 64; lane++)
            w |= (uint64_t)(x[lane] >> b & 1) << lane;
        planes[b] = w;
    }
}

/* counts[l] = bit l of each output plane, reassembled into a number */
void popcnt4_unpack(const uint64_t planes[3], uint8_t counts[64])
{
    for (int lane = 0; lane < 64; lane++)
        counts[lane] = (uint8_t)((planes[0] >> lane & 1)
                               | (planes[1] >> lane & 1) << 1
                               | (planes[2] >> lane & 1) << 2);
}

/* 64 popcounts in one call, lane bookkeeping hidden */
void popcnt4_batch(const uint8_t x[64], uint8_t counts[64])
{
    uint64_t in[4], out[3];
    popcnt4_pack(x, in);
    popcnt4(in, out);
    popcnt4_unpack(out, counts);
}
