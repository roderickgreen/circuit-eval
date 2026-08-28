/* Pack/unpack API for the bitsliced popcnt4 -- the teaching-sized
 * version of include/bsapi.h.
 *
 * The bitsliced kernel does not take one input at a time: input word b
 * carries bit b of all 64 evaluations, one evaluation per bit position
 * ("lane").  These wrappers convert between that plane layout and the
 * familiar one-value-per-lane arrays, so a caller never has to think about
 * lanes at all:
 *
 *   popcnt4_pack    values -> planes     (like bs_transpose)
 *   popcnt4_unpack  planes -> values     (like bs_untranspose)
 *   popcnt4_batch   pack + eval + unpack (like bs_eval_hands)
 */
#pragma once
#include <stdint.h>

void popcnt4_pack(const uint8_t x[64], uint64_t planes[4]);
void popcnt4_unpack(const uint64_t planes[3], uint8_t counts[64]);
void popcnt4_batch(const uint8_t x[64], uint8_t counts[64]);
