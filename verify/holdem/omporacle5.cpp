/* Standalone 5-card oracle: the holdem5 stamps computed WITHOUT the
 * circuit evaluator.
 *
 * Prints the same stamp pair as verify_holdem -k 5 (see there for the
 * definitions), but derives every hand's 24-bit value from two sources
 * that share no code with this repo's evaluator stack:
 *
 *   ordering   OMPEval (third-party/OMPEval), an independent evaluator:
 *              two hands compare the same way iff their OMPEval ranks do
 *   encoding   valmap.h, the 7462 class values by dense rank -- a
 *              checked-in table generated from flow/encoding/mkspec.py
 *              (the encoding is defined in ENCODING.md)
 *
 * Pass one runs all C(52,5) hands through OMPEval and collects the
 * distinct ranks; there must be exactly 7462, and sorting them ascending
 * (OMPEval ranks grow with strength) aligns them one-to-one with the
 * spec's classes sorted weakest to strongest.  That alignment is forced,
 * not fitted: it is the unique order-preserving bijection between two
 * total orders of equal size, so if OMPEval and the spec disagreed on
 * any class boundary the distinct count could not be 7462.  Pass two
 * stamps every hand with its mapped value.
 *
 * Validation is then: run this oracle, run verify_holdem -k 5, compare
 * stamp lines.  Equal stamps mean the circuit evaluator agrees with
 * OMPEval's ordering and the spec's encoding on every one of the
 * 2,598,960 hands; the checks above (6, 7, then omaha) chain off
 * that.
 *
 * Deliberately minimal: single-threaded, no options, a few seconds.
 *
 * usage: omporacle5
 * Exits 0 and prints the stamps, or 1 on any oracle inconsistency.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omp/HandEvaluator.h"
#include "valmap.h"

#define NCLASSES 7462
#define NHANDS 2598960          /* C(52,5) */

static uint64_t fmix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

int main(void)
{
    /* mirror mkspec.py's own assertions on the embedded table: 24-bit
     * values, strictly decreasing with rank */
    static_assert(sizeof VALMAP / sizeof VALMAP[0] == NCLASSES,
                  "VALMAP must have 7462 entries");
    for (int r = 0; r < NCLASSES; r++)
        if (VALMAP[r] >> 24 || (r && VALMAP[r] >= VALMAP[r - 1])) {
            fprintf(stderr, "omporacle5: VALMAP broken at rank %d\n",
                    r + 1);
            return 1;
        }

    omp::HandEvaluator ev;

    /* pass one: the set of OMPEval ranks that occur across all hands */
    static char rank_occurs[1 << 16];
    for (int a = 0; a < 48; a++)
        for (int b = a + 1; b < 49; b++)
            for (int c = b + 1; c < 50; c++)
                for (int d = c + 1; d < 51; d++) {
                    omp::Hand h4 = omp::Hand::empty();
                    h4 += omp::Hand(a);
                    h4 += omp::Hand(b);
                    h4 += omp::Hand(c);
                    h4 += omp::Hand(d);
                    for (int e = d + 1; e < 52; e++)
                        rank_occurs[ev.evaluate(h4 + omp::Hand(e))] = 1;
                }

    /* ascending OMPEval rank = ascending strength = descending spec
     * rank; the count check makes this alignment the unique
     * order-preserving bijection */
    static uint32_t val_by_omp[1 << 16];
    int nranks = 0;
    for (int o = 0; o < 1 << 16; o++)
        if (rank_occurs[o]) {
            nranks++;
            val_by_omp[o] = VALMAP[NCLASSES - nranks];
        }
    if (nranks != NCLASSES) {
        fprintf(stderr, "omporacle5: %d distinct OMPEval ranks, "
                        "want %d\n", nranks, NCLASSES);
        return 1;
    }

    /* pass two: stamp every hand with its mapped value */
    uint64_t stamp = 0, domain = 0;
    for (int a = 0; a < 48; a++)
        for (int b = a + 1; b < 49; b++)
            for (int c = b + 1; c < 50; c++)
                for (int d = c + 1; d < 51; d++) {
                    omp::Hand h4 = omp::Hand::empty();
                    h4 += omp::Hand(a);
                    h4 += omp::Hand(b);
                    h4 += omp::Hand(c);
                    h4 += omp::Hand(d);
                    uint64_t m4 = 1ull << a | 1ull << b |
                                  1ull << c | 1ull << d;
                    for (int e = d + 1; e < 52; e++) {
                        uint32_t val =
                            val_by_omp[ev.evaluate(h4 + omp::Hand(e))];
                        uint64_t inner = fmix64(m4 | 1ull << e);
                        domain += inner;
                        stamp += fmix64(inner ^ val);
                    }
                }

    printf("omporacle5: %d hands, %d classes\n", NHANDS, NCLASSES);
    printf("stamp %016llx domain %016llx (k 5, hands 0:%d)\n",
           (unsigned long long)stamp, (unsigned long long)domain,
           NHANDS - 1);
    return 0;
}
