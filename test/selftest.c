/* Library smoke test: known deals in, exact expected values out.
 *
 * Expected values are written against the documented encodings alone
 * (circuit_eval.h, ENCODING.md), so this
 * checks the whole shipped path -- mask transpose, input permutation,
 * circuit, output untranspose -- not the circuits' correctness proper (that
 * is the repo's verification sequence, `make verify`).  Deals cycle through
 * all BS_BATCH batch lanes so every lane position is exercised.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "circuit_eval.h"

enum { R2, R3, R4, R5, R6, R7, R8, R9, RT, RJ, RQ, RK, RA };
enum { SP, HE, DI, CL };
#define CD(r, s) ((r) * 4 + (s))

static uint64_t mask(const int *c, int n)
{
    uint64_t m = 0;
    for (int i = 0; i < n; i++)
        m |= 1ull << c[i];
    return m;
}

/* value = cat << 20 | kicker nibbles, left-justified from bit 16 */
static uint32_t pk(int cat, int a, int b, int c, int d, int e)
{
    return (uint32_t)(cat << 20 | a << 16 | b << 12 | c << 8 | d << 4 | e);
}

static int nchecks, nfail;

static void check(const char *game, const char *name, int lane,
                  uint32_t got, uint32_t want)
{
    nchecks++;
    if (got != want && nfail++ < 20)
        printf("FAIL %s %-12s lane %3d: got 0x%06x want 0x%06x\n",
               game, name, lane, got, want);
}

/* ---- holdem: one deal per hand category --------------------------------- */

static const struct {
    const char *name;
    int cards[7];
} hh[] = {
    {"straightflush", {CD(RA,SP), CD(RK,SP), CD(RQ,SP), CD(RJ,SP), CD(RT,SP),
                       CD(R2,HE), CD(R3,DI)}},
    {"quads",         {CD(RA,SP), CD(RA,HE), CD(RA,DI), CD(RA,CL), CD(RK,HE),
                       CD(R7,DI), CD(R2,CL)}},
    {"fullhouse",     {CD(RK,SP), CD(RK,HE), CD(RK,DI), CD(R9,DI), CD(R9,CL),
                       CD(R5,HE), CD(R2,SP)}},
    {"flush",         {CD(RK,SP), CD(RJ,SP), CD(R9,SP), CD(R6,SP), CD(R2,SP),
                       CD(R3,HE), CD(R8,HE)}},
    {"straight",      {CD(RT,SP), CD(RJ,HE), CD(RQ,DI), CD(RK,CL), CD(RA,SP),
                       CD(R8,DI), CD(R3,HE)}},
    {"wheel",         {CD(RA,SP), CD(R2,HE), CD(R3,DI), CD(R4,CL), CD(R5,SP),
                       CD(RQ,HE), CD(R9,DI)}},
    {"trips",         {CD(RQ,SP), CD(RQ,HE), CD(RQ,DI), CD(R9,CL), CD(R7,SP),
                       CD(R4,HE), CD(R2,DI)}},
    {"twopair",       {CD(RA,SP), CD(RA,HE), CD(RK,SP), CD(RK,HE), CD(R7,DI),
                       CD(R5,CL), CD(R2,DI)}},
    {"onepair",       {CD(RA,SP), CD(RA,HE), CD(RK,DI), CD(RQ,CL), CD(RJ,SP),
                       CD(R9,HE), CD(R3,DI)}},
    {"highcard",      {CD(RK,SP), CD(RJ,HE), CD(R8,DI), CD(R6,CL), CD(R4,SP),
                       CD(R3,HE), CD(R2,DI)}},
};
static const int NH = sizeof hh / sizeof hh[0];

static uint32_t holdem_want(int i)
{
    switch (i) {
    case 0: return pk(9, RA, 0, 0, 0, 0);
    case 1: return pk(8, RA, RK, 0, 0, 0);
    case 2: return pk(7, RK, R9, 0, 0, 0);
    case 3: return pk(6, RK, RJ, R9, R6, R2);
    case 4: return pk(5, RA, 0, 0, 0, 0);
    case 5: return pk(5, R5, 0, 0, 0, 0);    /* wheel: the five plays high */
    case 6: return pk(4, RQ, R9, R7, 0, 0);
    case 7: return pk(3, RA, RK, R7, 0, 0);
    case 8: return pk(2, RA, RK, RQ, RJ, 0);
    default: return pk(1, RK, RJ, R8, R6, R4);
    }
}

/* ---- omaha: the use-exactly-2-hole-cards rule, plo4/5/6, hi-lo ---------- */

static const struct {
    const char *name;
    int nh;
    int hole[6];
    int board[5];
    uint32_t low;
} oo[] = {
    /* quads from 2 hole aces + 2 board aces (board pairs the hole) */
    {"quads", 4,
     {CD(RA,SP), CD(RA,HE), CD(R3,CL), CD(R4,DI)},
     {CD(RA,DI), CD(RA,CL), CD(RK,SP), CD(RQ,HE), CD(RJ,DI)},
     0xFF},
    /* board reads AAAKK, but two hole cards MUST play: best is trips aces
     * with the 8 and 7 as kickers (a best-5-of-9 evaluator would take the
     * board's full house) */
    {"use2", 4,
     {CD(R2,CL), CD(R3,DI), CD(R7,SP), CD(R8,HE)},
     {CD(RA,SP), CD(RA,HE), CD(RA,DI), CD(RK,SP), CD(RK,HE)},
     0xFF},
    /* pair of kings high; A-2 in the hole + 3-4-8 on board = 8-7-low */
    {"low", 4,
     {CD(RA,SP), CD(R2,HE), CD(RK,DI), CD(RJ,CL)},
     {CD(R3,DI), CD(R4,CL), CD(R8,SP), CD(RK,HE), CD(RQ,DI)},
     0x8F},
    /* second low shape (A-2-4-5-8); high is still just a pair of kings */
    {"low2", 4,
     {CD(RA,SP), CD(R2,HE), CD(R3,SP), CD(RK,DI)},
     {CD(R4,DI), CD(R5,CL), CD(R8,SP), CD(RK,HE), CD(RQ,DI)},
     0x9B},
    /* broadway off A-T in the hole; only one low board rank -> no low */
    {"nolow", 4,
     {CD(RA,SP), CD(R2,HE), CD(RT,SP), CD(RT,HE)},
     {CD(R3,DI), CD(RK,SP), CD(RQ,HE), CD(RJ,DI), CD(R9,CL)},
     0xFF},
    /* same quads deal at 5 and 6 hole cards (extra cards change nothing) */
    {"plo5", 5,
     {CD(RA,SP), CD(RA,HE), CD(R3,CL), CD(R4,DI), CD(R9,HE)},
     {CD(RA,DI), CD(RA,CL), CD(RK,SP), CD(RQ,HE), CD(RJ,DI)},
     0xFF},
    {"plo6", 6,
     {CD(RA,SP), CD(RA,HE), CD(R3,CL), CD(R4,DI), CD(R9,HE), CD(R6,DI)},
     {CD(RA,DI), CD(RA,CL), CD(RK,SP), CD(RQ,HE), CD(RJ,DI)},
     0xFF},
};
static const int NO = sizeof oo / sizeof oo[0];

static uint32_t omaha_want(int i)
{
    switch (i) {
    case 0: return pk(8, RA, RK, 0, 0, 0);
    case 1: return pk(4, RA, R8, R7, 0, 0);
    case 2: return pk(2, RK, RA, RQ, R8, 0);
    case 3: return pk(2, RK, RA, RQ, R8, 0);
    case 4: return pk(5, RA, 0, 0, 0, 0);
    default: return pk(8, RA, RK, 0, 0, 0);  /* plo5, plo6 */
    }
}

/* ---- comparators: value planes in, per-lane gt/eq bits out -------------- */

/* Transpose two batches of w-bit values into the comparator's input planes
 * (a at [0..w-1], b at [w..2w-1]), run it, and check gt | eq << 1 per lane
 * against scalar integer compare. */
static void check_cmp(const char *game, int w,
                      void (*cmp)(const bs_word *, bs_word *),
                      const uint64_t *a, const uint64_t *b)
{
    static bs_word pa[64], pb[64], cin[48], cout[2];
    static uint32_t res[BS_BATCH];
    bs_transpose(a, pa);
    bs_transpose(b, pb);
    memcpy(cin, pa, w * sizeof(bs_word));
    memcpy(cin + w, pb, w * sizeof(bs_word));
    cmp(cin, cout);
    bs_untranspose(cout, 2, res);
    for (int l = 0; l < BS_BATCH; l++)
        check(game, "pair", l, res[l],
              (uint32_t)((a[l] > b[l]) | (uint32_t)(a[l] == b[l]) << 1));
}

/* ---- fused equity counters: plane-space counts vs the scalar values ----- */

/* Deterministic pseudo-random deals: partial Fisher-Yates over the deck
 * driven by a fixed-seed LCG, so every run sees the same matchups. */
static uint32_t eqrand_state = 0x5eed;

static uint32_t eqrand(void)
{
    eqrand_state = eqrand_state * 1664525u + 1013904223u;
    return eqrand_state >> 16;
}

static void eqdeal(int *c, int n)
{
    int deck[52];
    for (int i = 0; i < 52; i++)
        deck[i] = i;
    for (int i = 0; i < n; i++) {
        int j = i + (int)(eqrand() % (uint32_t)(52 - i));
        int t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
        c[i] = deck[i];
    }
}

static void check_count(const char *game, const char *name, int n,
                        uint64_t got, uint64_t want)
{
    nchecks++;
    if (got != want && nfail++ < 20)
        printf("FAIL %s %-8s n %3d: got %llu want %llu\n", game, name, n,
               (unsigned long long)got, (unsigned long long)want);
}

/* bs_masks_from_cards dispatch matrix: every ncards the vector cores'
 * switch pins to a compile-time constant (4-7), the default runtime-ncards
 * path (1-3, 8), and a stride wide enough to force the vector cores' too-
 * wide fallback to scalar (8, 30: stride + ncards = 38 > both the AVX-512
 * and NEON window) -- each checked against the mask definition directly.
 * The cards buffer is sized to exactly the last byte the header promises
 * not to read past, so an over-read in a vector core's tail-guard window
 * would show up under ASan even though this build does not run under one. */
static void check_masks_matrix(void)
{
    static const struct { int ncards; size_t stride; } cfgs[] = {
        {1, 1}, {2, 3}, {3, 5}, {4, 9}, {5, 9}, {5, 10}, {6, 11}, {7, 7},
        {8, 8}, {8, 30},
    };
    for (size_t k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        int ncards = cfgs[k].ncards;
        size_t stride = cfgs[k].stride;
        size_t total = (size_t)(BS_BATCH - 1) * stride + (size_t)ncards;
        uint8_t *cards = malloc(total);
        uint64_t seed = 0x9E3779B97F4A7C15ull ^ (uint64_t)(ncards * 131 + (int)stride);
        for (size_t i = 0; i < total; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            cards[i] = (uint8_t)((seed >> 33) % 64);
        }
        static uint64_t want[BS_BATCH], got[BS_BATCH];
        for (int l = 0; l < BS_BATCH; l++) {
            uint64_t m = 0;
            for (int j = 0; j < ncards; j++)
                m |= 1ull << cards[(size_t)l * stride + (size_t)j];
            want[l] = m;
        }
        bs_masks_from_cards(cards, ncards, stride, got);
        for (int l = 0; l < BS_BATCH; l++) {
            nchecks++;
            if (got[l] != want[l] && nfail++ < 20)
                printf("FAIL masks-matrix  ncards=%d stride=%zu lane %3d: "
                       "got %016llx want %016llx\n", ncards, stride, l,
                       (unsigned long long)got[l], (unsigned long long)want[l]);
        }
        free(cards);
    }
}

/* The transpose contracts (bsapi.h) at plane level.  The circuit sections
 * below drive pack and unpack only through fixed deals, whose masks never
 * set some bit positions and repeat every few lanes -- a wiring error in
 * an untouched plane, or a lane permutation with the same period, would
 * pass them.  Here dense pseudo-random masks (all 64 bits in play) are
 * checked bit for bit against the layout definition: bs_transpose
 * including planes 52..63 (the masks' high bits, per the contract),
 * bs_transpose_map's write-exactly-52 promise (poison must survive in
 * 52..63), and bs_untranspose at the shipped widths plus contract edges. */
static void check_transposes(void)
{
    static uint64_t masks[BS_BATCH];
    static bs_word planes[64], ref[64];
    static uint32_t vals[BS_BATCH];
    static const int nouts[] = { 2, 8, 13, 24, 32 };
    const int *map = circuit_eval_holdem_raw_card_input;
    uint64_t s = 0xc0ffee0123456789ull;
    bs_word poison;
    memset(&poison, 0xAA, sizeof poison);

    for (int rep = 0; rep < 4; rep++) {
        for (int l = 0; l < BS_BATCH; l++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            uint64_t hi = s >> 32;
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            masks[l] = hi << 32 | s >> 32;
        }

        /* the layout by definition: plane c lane bit l = bit c of mask l */
        uint64_t *rw = (uint64_t *)ref;
        memset(ref, 0, sizeof ref);
        for (int l = 0; l < BS_BATCH; l++)
            for (int c = 0; c < 64; c++)
                if (masks[l] >> c & 1)
                    rw[(size_t)c * BS_LANES + (l >> 6)] |= 1ull << (l & 63);

        memset(planes, 0xAA, sizeof planes);
        bs_transpose(masks, planes);
        for (int c = 0; c < 64; c++) {
            nchecks++;
            if (memcmp(&planes[c], &ref[c], sizeof(bs_word)) && nfail++ < 20)
                printf("FAIL transpose     plane %2d rep %d\n", c, rep);
        }

        memset(planes, 0xAA, sizeof planes);
        bs_transpose_map(masks, map, planes);
        for (int c = 0; c < 52; c++) {
            nchecks++;
            if (memcmp(&planes[map[c]], &ref[c], sizeof(bs_word)) &&
                nfail++ < 20)
                printf("FAIL transpose-map plane %2d rep %d\n", c, rep);
        }
        for (int c = 52; c < 64; c++) {
            nchecks++;
            if (memcmp(&planes[c], &poison, sizeof(bs_word)) && nfail++ < 20)
                printf("FAIL transpose-map wrote plane %2d rep %d\n", c, rep);
        }

        /* ref is the transpose of masks, so untransposing its first nout
         * planes must return the masks' low nout bits */
        for (size_t k = 0; k < sizeof nouts / sizeof nouts[0]; k++) {
            int nout = nouts[k];
            uint32_t keep = nout == 32 ? 0xFFFFFFFFu : (1u << nout) - 1;
            bs_untranspose(ref, nout, vals);
            for (int l = 0; l < BS_BATCH; l++) {
                nchecks++;
                if (vals[l] != (uint32_t)(masks[l] & keep) && nfail++ < 20)
                    printf("FAIL untranspose   nout %2d lane %3d rep %d\n",
                           nout, l, rep);
            }
        }
    }
}

int main(void)
{
    if (circuit_eval_holdem_raw_nb != 1 || circuit_eval_omaha_raw_nb != 1 ||
        circuit_eval_omaha_low_raw_nb != 1 ||
        circuit_eval_holdem_raw_num_outputs != 24 ||
        circuit_eval_omaha_raw_num_outputs != 24 ||
        circuit_eval_omaha_low_raw_num_outputs != 8 ||
        circuit_cmp8_raw_num_outputs != 2 ||
        circuit_cmp24_raw_num_outputs != 2 ||
        circuit_cmp8_raw_num_inputs != 16 ||
        circuit_cmp24_raw_num_inputs != 48) {
        printf("FAIL: circuit metadata mismatch\n");
        return 1;
    }

    static uint64_t hands[BS_BATCH], hole[BS_BATCH], board[BS_BATCH];
    static uint32_t vals[BS_BATCH], high[BS_BATCH], low[BS_BATCH];

    for (int l = 0; l < BS_BATCH; l++)
        hands[l] = mask(hh[l % NH].cards, 7);
    circuit_eval_holdem(hands, vals);
    for (int l = 0; l < BS_BATCH; l++)
        check("holdem", hh[l % NH].name, l, vals[l], holdem_want(l % NH));

    /* the cards-to-masks converter feeding the same call: the batch as
     * card lists, reversed on odd lanes (order inside a hand must not
     * matter), converted and re-evaluated */
    static uint8_t clist[BS_BATCH][7];
    static uint64_t cmask[BS_BATCH];
    for (int l = 0; l < BS_BATCH; l++)
        for (int j = 0; j < 7; j++)
            clist[l][j] = (uint8_t)hh[l % NH].cards[l % 2 ? 6 - j : j];
    bs_masks_from_cards(&clist[0][0], 7, 7, cmask);
    circuit_eval_holdem(cmask, vals);
    for (int l = 0; l < BS_BATCH; l++)
        check("holdem-cards", hh[l % NH].name, l, vals[l], holdem_want(l % NH));

    /* the strided converter underneath it: the same hands embedded in
     * 12-byte records whose pad bytes are 63 -- a pad byte read by
     * mistake would set mask bit 63 and fail the compare */
    static uint8_t rec[BS_BATCH][12];
    static uint64_t rmask[BS_BATCH];
    memset(rec, 63, sizeof rec);
    for (int l = 0; l < BS_BATCH; l++)
        memcpy(rec[l], clist[l], 7);
    bs_masks_from_cards(&rec[0][0], 7, 12, rmask);
    for (int l = 0; l < BS_BATCH; l++) {
        nchecks++;
        if (rmask[l] != hands[l] && nfail++ < 20)
            printf("FAIL cards stride    lane %3d: got %016llx want %016llx\n",
                   l, (unsigned long long)rmask[l],
                   (unsigned long long)hands[l]);
    }

    check_masks_matrix();
    check_transposes();

    for (int l = 0; l < BS_BATCH; l++) {
        hole[l] = mask(oo[l % NO].hole, oo[l % NO].nh);
        board[l] = mask(oo[l % NO].board, 5);
    }
    circuit_eval_omaha(hole, board, vals);
    for (int l = 0; l < BS_BATCH; l++)
        check("omaha", oo[l % NO].name, l, vals[l], omaha_want(l % NO));

    circuit_eval_omaha_hilo(hole, board, high, low);
    for (int l = 0; l < BS_BATCH; l++) {
        check("hilo-high", oo[l % NO].name, l, high[l], omaha_want(l % NO));
        check("hilo-low", oo[l % NO].name, l, low[l], oo[l % NO].low);
    }

    /* comparators: pair every lane's high value with a rotated lane's,
     * forcing equality on every fifth lane so gt, eq and neither all
     * occur; the cmp8 pass reuses the same values folded to 8 bits */
    static uint64_t va[BS_BATCH], vb[BS_BATCH];
    for (int l = 0; l < BS_BATCH; l++) {
        va[l] = high[l];
        vb[l] = (l % 5 == 0) ? high[l] : high[(l + 3) % BS_BATCH];
    }
    check_cmp("cmp24", 24, circuit_cmp24_raw, va, vb);
    for (int l = 0; l < BS_BATCH; l++) {
        va[l] = (va[l] ^ va[l] >> 8 ^ va[l] >> 16) & 0xFF;
        vb[l] = (vb[l] ^ vb[l] >> 8 ^ vb[l] >> 16) & 0xFF;
    }
    check_cmp("cmp8", 8, circuit_cmp8_raw, va, vb);

    /* equity counters, checked against counting the scalar values of the
     * per-game calls over the same batches.  Every 7th lane repeats hand
     * a as hand b -- not a dealable matchup, but each mask alone is
     * well-formed, and it guarantees tie lanes.  Each game is checked at
     * a full batch and at a partial n that is not a multiple of 64, so
     * the tail-lane masking is exercised. */
    static uint64_t ea[BS_BATCH], eb[BS_BATCH], bd[BS_BATCH];
    static uint32_t evb[BS_BATCH], hib[BS_BATCH], lob[BS_BATCH];
    static const int ns[2] = { BS_BATCH, BS_BATCH - 61 };
    uint64_t w, t, lw, lt, nl;

    for (int l = 0; l < BS_BATCH; l++) {
        int c[9];
        eqdeal(c, 9);
        ea[l] = mask(c, 2) | mask(c + 4, 5);
        eb[l] = (l % 7 == 0) ? ea[l] : mask(c + 2, 2) | mask(c + 4, 5);
    }
    circuit_eval_holdem(ea, vals);
    circuit_eval_holdem(eb, evb);
    for (int i = 0; i < 2; i++) {
        uint64_t ew = 0, et = 0;
        for (int l = 0; l < ns[i]; l++) {
            ew += vals[l] > evb[l];
            et += vals[l] == evb[l];
        }
        circuit_equity_holdem(ea, eb, ns[i], &w, &t);
        check_count("equity-holdem", "wins", ns[i], w, ew);
        check_count("equity-holdem", "ties", ns[i], t, et);
    }

    for (int l = 0; l < BS_BATCH; l++) {
        int c[13];
        eqdeal(c, 13);
        ea[l] = mask(c, 4);
        eb[l] = (l % 7 == 0) ? ea[l] : mask(c + 4, 4);
        bd[l] = mask(c + 8, 5);
    }
    circuit_eval_omaha(ea, bd, vals);
    circuit_eval_omaha(eb, bd, evb);
    for (int i = 0; i < 2; i++) {
        uint64_t ew = 0, et = 0;
        for (int l = 0; l < ns[i]; l++) {
            ew += vals[l] > evb[l];
            et += vals[l] == evb[l];
        }
        circuit_equity_omaha(ea, eb, bd, ns[i], &w, &t);
        check_count("equity-omaha", "wins", ns[i], w, ew);
        check_count("equity-omaha", "ties", ns[i], t, et);
    }

    circuit_eval_omaha_hilo(ea, bd, high, low);
    circuit_eval_omaha_hilo(eb, bd, hib, lob);
    for (int i = 0; i < 2; i++) {
        uint64_t ew = 0, et = 0, elw = 0, elt = 0, enl = 0;
        for (int l = 0; l < ns[i]; l++) {
            int hw = high[l] > hib[l], ht = high[l] == hib[l];
            int none = low[l] == 0xFF && lob[l] == 0xFF;
            ew += hw;
            et += ht;
            /* low half of the pot: no-low lanes follow the high result */
            elw += none ? hw : low[l] < lob[l];
            elt += none ? ht : low[l] == lob[l];
            enl += none;
        }
        circuit_equity_omaha_hilo(ea, eb, bd, ns[i], &w, &t, &lw, &lt, &nl);
        check_count("equity-hilo", "hi-wins", ns[i], w, ew);
        check_count("equity-hilo", "hi-ties", ns[i], t, et);
        check_count("equity-hilo", "lo-wins", ns[i], lw, elw);
        check_count("equity-hilo", "lo-ties", ns[i], lt, elt);
        check_count("equity-hilo", "no-low", ns[i], nl, enl);
    }

    if (nfail) {
        printf("selftest: FAIL (%d of %d checks)\n", nfail, nchecks);
        return 1;
    }
    printf("selftest: PASS (%d checks)\n", nchecks);
    return 0;
}
