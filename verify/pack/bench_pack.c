// Throughput for the transpose API (bsapi.h), phase by phase, so
// the overhead budget around the circuit is visible: mask build
// (bs_masks_from_cards), transpose pack, circuit alone, untranspose, and
// the one-call bs_eval_hands end to end.  Links against any circuit
// object: with bs_card_input it runs the 7-card holdem shape, without it
// (104 positional planes) the omaha shape.
//
// usage: bench_pack [nbatch] [repeats]   (BS_BATCH hands per batch)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "bsapi.h"

typedef unsigned long long u64;

extern void bs_eval(const bs_word *in, bs_word *out);
extern const int bs_num_outputs;
extern const int bs_card_input[52] __attribute__((weak));

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static u64 rng = 0x9e3779b97f4a7c15ull;
static unsigned rnd(void)
{
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rng >> 33);
}

int main(int argc, char **argv)
{
    long nbatch = argc > 1 ? atol(argv[1]) : 1024;
    int repeats = argc > 2 ? atoi(argv[2]) : 20;
    if (nbatch < 1 || repeats < 1) { fprintf(stderr, "bad args\n"); return 1; }
    int omaha = !bs_card_input;
    int nin = omaha ? 104 : 52;         /* planes per batch */
    int nout = bs_num_outputs;
    int nh = omaha ? 4 : 7, nd = omaha ? 9 : 7;   /* hole + board cards */
    long n = nbatch * BS_BATCH;
    if (nout > 32) { fprintf(stderr, "nout %d > 32\n", nout); return 1; }

    /* a deal is nd card ids, the hole cards first -- the record layout
     * bs_masks_from_cards converts in place */
    unsigned char *deals = malloc((size_t)n * nd);
    for (long i = 0; i < n; i++) {
        unsigned char *d = deals + i * nd;
        int k = 0;
        while (k < nd) {
            int c = rnd() % 52;
            int dup = 0;
            for (int j = 0; j < k; j++) dup |= (d[j] == c);
            if (!dup) d[k++] = (unsigned char)c;
        }
    }

    /* presence masks: holdem one per hand; omaha hole + board separately */
    uint64_t *hole = malloc(n * 8), *board = omaha ? malloc(n * 8) : NULL;
    /* board transpose writes 64 planes, spilling 12 words past the last
     * batch's 104 (overwritten by the next batch's hole planes otherwise) */
    bs_word *in = aligned_alloc(sizeof(bs_word),
                                ((size_t)nbatch * nin + 12) * sizeof(bs_word));
    bs_word *out = aligned_alloc(sizeof(bs_word),
                                 (size_t)nbatch * nout * sizeof(bs_word));
    uint32_t *vals = malloc(n * 4);

#define TIME(label, ...)                                          \
    do {                                                          \
        double best = 1e30;                                       \
        for (int r = 0; r < repeats; r++) {                       \
            double t0 = now();                                    \
            __VA_ARGS__                                           \
            double dt = now() - t0;                               \
            if (dt < best) best = dt;                             \
        }                                                         \
        printf("%-26s %7.3f ns/hand\n", label, best / n * 1e9);   \
    } while (0)

    TIME("mask build (from cards)", {
        for (long b = 0; b < nbatch; b++) {
            const unsigned char *rec = deals + (size_t)b * BS_BATCH * nd;
            bs_masks_from_cards(rec, nh, nd, hole + b * BS_BATCH);
            if (omaha)
                bs_masks_from_cards(rec + nh, 5, nd, board + b * BS_BATCH);
        }
    });

    TIME("transpose pack", {
        for (long b = 0; b < nbatch; b++) {
            if (omaha) {
                bs_transpose(hole + b * BS_BATCH, in + b * nin);
                bs_transpose(board + b * BS_BATCH, in + b * nin + 52);
            } else {
                bs_transpose_map(hole + b * BS_BATCH, bs_card_input,
                                 in + b * nin);
            }
        }
    });

    TIME("bs_eval (circuit alone)", {
        for (long b = 0; b < nbatch; b++)
            bs_eval(in + b * nin, out + b * nout);
    });

    TIME("untranspose", {
        for (long b = 0; b < nbatch; b++)
            bs_untranspose(out + b * nout, nout, vals + b * BS_BATCH);
    });

    TIME("bs_eval_hands (one call)", {
        for (long b = 0; b < nbatch; b++) {
            if (omaha)
                bs_eval_hands2(hole + b * BS_BATCH, board + b * BS_BATCH,
                               vals + b * BS_BATCH);
            else
                bs_eval_hands(hole + b * BS_BATCH, vals + b * BS_BATCH);
        }
    });

    return 0;
}
