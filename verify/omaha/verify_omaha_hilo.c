/* Bootstrap validation of the omaha hi-lo evaluator pair, both sides
 * in one pass, exhaustive at each hole-card count, every hole count
 * anchored as in the single-side gates.
 *
 * This merges verify_omaha and verify_omaha_lo.  The high reference is
 * unchanged: btab_hi[t] = max over the C(k,2) hole pairs of
 * holdem5(pair + triple t), the 5-card primitive exhaustively anchored
 * by the verify_holdem / omporacle5 stamp comparison, so a board's
 * expected high is the max of 10 lookups.  The low reference is
 * unchanged too: btab_lo[t] = min over the hole pairs of the
 * rules-only 5-card low (qualify iff five distinct ranks in ace..eight
 * ace low, value = the 8-bit rank mask of circuit_eval.h, 0xFF = no
 * low), expected low = min of 10 lookups.  Both tables are indexed by
 * the same colex triple order (triple i < j < k at
 * C(k,3) + C(j,2) + i), so one board enumeration serves both sides,
 * and the evaluator under test is circuit_eval_omaha_hilo, which runs
 * the high and low circuits off one shared input transpose.  That
 * sharing -- one enumeration, one batch fill, one transpose -- is the
 * point of the merge: validating both sides here costs less than the
 * two single-side sweeps run separately.
 *
 * Stamps are defined per side exactly as in the single-side gates, and
 * the domain stamp is common to all of verify/omaha:
 *
 *   inner    = fmix64(fmix64(hole_mask) ^ board_mask)
 *   domain   = sum of inner                     mod 2^64
 *   stamp_hi = sum of fmix64(inner ^ high)      mod 2^64
 *   stamp_lo = sum of fmix64(inner ^ low)       mod 2^64
 *
 * so stamp_hi is bit-comparable with verify_omaha output, stamp_lo
 * with verify_omaha_lo, and slices recombine by the same rule: accept
 * the sum of slice value stamps only after the slice domain stamps sum
 * to the domain constant for that k.  Hole counts are independent; run
 * them in any order.  -b sweeps 3- or 4-card boards instead, each side
 * folded over the C(b,3) board triples as in its single-side gate.
 *
 * usage: verify_omaha_hilo -k holes [-b board] [-t threads] [-p first[:last]] [-d]
 *   -k   hole cards to validate, 2..8 (each stands alone)
 *   -b   board cards, 3..5 (default 5; each stands alone)
 *   -t   worker threads (default: online CPUs)
 *   -p   hole-set index range, colex order, for partial runs
 *   -d   domain-only: no evaluation, compute just the domain stamp
 *
 * Exits 0 on a clean sweep, 1 if any configuration mismatched.
 */
#include "omaha_sweep.h"
#include "circuit_eval.h"

#define BATCH BS_BATCH          /* library batch size (bsapi.h) */

static _Atomic long long bad_hi_total;
static _Atomic long long bad_lo_total;
static _Atomic unsigned long long stamp_hi_total;
static _Atomic unsigned long long stamp_lo_total;
static _Atomic unsigned long long domain_total;

/* Mismatch printing is capped; the count is exact regardless. */
#define BAD_PRINT_MAX 20
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static int bad_printed;

static void report_bad(const char *side, uint64_t hole, uint64_t board,
                       uint32_t got, uint32_t want)
{
    pthread_mutex_lock(&print_mu);
    if (bad_printed++ < BAD_PRINT_MAX) {
        printf("MISMATCH");
        print_mask("hole", hole);
        print_mask("board", board);
        printf("  omaha%d-%s 0x%06x  reference 0x%06x\n",
               khole, side, got, want);
    }
    pthread_mutex_unlock(&print_mu);
}

typedef struct {
    uint64_t hole[BATCH];       /* the k-card hole, every lane */
    uint64_t board[BATCH];      /* doubles as 5-card hand scratch */
    uint32_t got_hi[BATCH], got_lo[BATCH];
    uint32_t want_hi[BATCH];
    unsigned char want_lo[BATCH];
} lanes_t;

typedef struct {
    long long hi, lo;
} badcnt_t;

static badcnt_t eval_batch(lanes_t *L, int n, uint64_t hm, uint64_t hh,
                           uint64_t *shi, uint64_t *slo, uint64_t *dsum)
{
    badcnt_t bad = { 0, 0 };
    uint64_t sh = 0, sl = 0, d = 0;
    circuit_eval_omaha_hilo(L->hole, L->board, L->got_hi, L->got_lo);
    for (int m = 0; m < n; m++) {
        uint64_t inner = fmix64(hh ^ L->board[m]);
        d += inner;
        sh += fmix64(inner ^ L->got_hi[m]);
        sl += fmix64(inner ^ L->got_lo[m]);
        if (L->got_hi[m] != L->want_hi[m]) {
            bad.hi++;
            report_bad("high", hm, L->board[m],
                       L->got_hi[m], L->want_hi[m]);
        }
        if (L->got_lo[m] != L->want_lo[m]) {
            bad.lo++;
            report_bad("low", hm, L->board[m],
                       L->got_lo[m], L->want_lo[m]);
        }
    }
    *shi += sh;
    *slo += sl;
    *dsum += d;
    return bad;
}

/* btab_hi[t] = max over the hole pairs of holdem5(pair + triple t),
 * one colex pass over the remaining-card triples per pair, batched
 * BATCH (L->board is the 5-card hand scratch here) */
static void build_btab_hi(lanes_t *L, uint32_t *btab, const int *hc,
                          const uint64_t *rm, int nr)
{
    uint64_t pm[NPAIRS_MAX];
    int np = 0;
    for (int i = 0; i < khole; i++)
        for (int j = i + 1; j < khole; j++)
            pm[np++] = 1ull << hc[i] | 1ull << hc[j];

    memset(btab, 0, c3[nr] * sizeof *btab);
    for (int p = 0; p < np; p++) {
        int bi = 0, off = 0;
        for (int k = 2; k < nr; k++)
            for (int j = 1; j < k; j++)
                for (int i = 0; i < j; i++) {
                    L->board[bi++] = pm[p] | rm[i] | rm[j] | rm[k];
                    if (bi == BATCH) {
                        circuit_eval_holdem(L->board, L->got_hi);
                        for (int m = 0; m < BATCH; m++)
                            if (L->got_hi[m] > btab[off + m])
                                btab[off + m] = L->got_hi[m];
                        off += BATCH;
                        bi = 0;
                    }
                }
        if (bi) {
            for (int m = bi; m < BATCH; m++)
                L->board[m] = L->board[bi - 1];
            circuit_eval_holdem(L->board, L->got_hi);
            for (int m = 0; m < bi; m++)
                if (L->got_hi[m] > btab[off + m])
                    btab[off + m] = L->got_hi[m];
        }
    }
}

/* btab_lo[t] = min over the hole pairs of the rules-only 5-card low
 * for pair + triple t, same colex triple order as the high table */
static void build_btab_lo(unsigned char *btab, const int *hc,
                          const unsigned *lb, int nr)
{
    unsigned hb[NPAIRS_MAX];
    int np = 0;
    for (int i = 0; i < khole; i++)
        for (int j = i + 1; j < khole; j++)
            hb[np++] = lowbit[hc[i]] | lowbit[hc[j]];
    int t = 0;
    for (int k = 2; k < nr; k++)
        for (int j = 1; j < k; j++)
            for (int i = 0; i < j; i++) {
                unsigned b3 = lb[i] | lb[j] | lb[k];
                unsigned best = NOLOW;
                for (int p = 0; p < np; p++) {
                    unsigned b = hb[p] | b3;
                    if (!(b & 0x100) && b < best &&
                        __builtin_popcount(b) == 5)
                        best = b;
                }
                btab[t++] = (unsigned char)best;
            }
}

#define FLUSH_BATCH(n)                                                  \
    do {                                                                \
        badcnt_t b2 = eval_batch(L, (n), hm, hh, shi, slo, dsum);       \
        bad.hi += b2.hi;                                                \
        bad.lo += b2.lo;                                                \
    } while (0)

/* 5-card boards: every 5-subset of the remaining cards, expected high
 * the max and expected low the min over its 10 triples of the two
 * tables, the pair terms hoisted */
static badcnt_t sweep5(lanes_t *L, const uint32_t *btab_hi,
                       const unsigned char *btab_lo,
                       const uint64_t *rm, int nr, uint64_t hm,
                       uint64_t hh, uint64_t *shi, uint64_t *slo,
                       uint64_t *dsum)
{
    badcnt_t bad = { 0, 0 };
    int bi = 0;
    for (int a = 0; a < nr - 4; a++)
        for (int b = a + 1; b < nr - 3; b++) {
            int pab = c2[b] + a;
            for (int c = b + 1; c < nr - 2; c++) {
                int pac = c2[c] + a, pbc = c2[c] + b;
                uint32_t hc3 = btab_hi[c3[c] + pab];
                unsigned lc3 = btab_lo[c3[c] + pab];
                uint64_t m3 = rm[a] | rm[b] | rm[c];
                for (int dd = c + 1; dd < nr - 1; dd++) {
                    int pad = c2[dd] + a, pbd = c2[dd] + b,
                        pcd = c2[dd] + c;
                    const uint32_t *hd = btab_hi + c3[dd];
                    const unsigned char *ld = btab_lo + c3[dd];
                    uint32_t hmx = hc3, v;
                    v = hd[pab]; if (v > hmx) hmx = v;
                    v = hd[pac]; if (v > hmx) hmx = v;
                    v = hd[pbc]; if (v > hmx) hmx = v;
                    unsigned lmn = lc3, w;
                    w = ld[pab]; if (w < lmn) lmn = w;
                    w = ld[pac]; if (w < lmn) lmn = w;
                    w = ld[pbc]; if (w < lmn) lmn = w;
                    uint64_t m4 = m3 | rm[dd];
                    for (int e = dd + 1; e < nr; e++) {
                        const uint32_t *he = btab_hi + c3[e];
                        const unsigned char *le = btab_lo + c3[e];
                        uint32_t hx = hmx;
                        v = he[pab]; if (v > hx) hx = v;
                        v = he[pac]; if (v > hx) hx = v;
                        v = he[pad]; if (v > hx) hx = v;
                        v = he[pbc]; if (v > hx) hx = v;
                        v = he[pbd]; if (v > hx) hx = v;
                        v = he[pcd]; if (v > hx) hx = v;
                        unsigned lx = lmn;
                        w = le[pab]; if (w < lx) lx = w;
                        w = le[pac]; if (w < lx) lx = w;
                        w = le[pad]; if (w < lx) lx = w;
                        w = le[pbc]; if (w < lx) lx = w;
                        w = le[pbd]; if (w < lx) lx = w;
                        w = le[pcd]; if (w < lx) lx = w;
                        L->board[bi] = m4 | rm[e];
                        L->want_hi[bi] = hx;
                        L->want_lo[bi] = (unsigned char)lx;
                        if (++bi == BATCH) {
                            FLUSH_BATCH(BATCH);
                            bi = 0;
                        }
                    }
                }
            }
        }
    if (bi) {
        for (int m = bi; m < BATCH; m++)
            L->board[m] = L->board[bi - 1];
        FLUSH_BATCH(bi);
    }
    return bad;
}

/* 3- and 4-card boards: every b-subset of the remaining cards, expected
 * high the max and expected low the min over its C(b,3) triples of the
 * two tables */
static badcnt_t sweep_short(lanes_t *L, const uint32_t *btab_hi,
                            const unsigned char *btab_lo,
                            const uint64_t *rm, int nr, uint64_t hm,
                            uint64_t hh, uint64_t *shi, uint64_t *slo,
                            uint64_t *dsum)
{
    badcnt_t bad = { 0, 0 };
    int bi = 0;
    for (int a = 0; a < nr - 2; a++)
        for (int b = a + 1; b < nr - 1; b++) {
            int pab = c2[b] + a;
            for (int c = b + 1; c < nr; c++) {
                uint64_t m3 = rm[a] | rm[b] | rm[c];
                if (nboard == 3) {
                    L->board[bi] = m3;
                    L->want_hi[bi] = btab_hi[c3[c] + pab];
                    L->want_lo[bi] = btab_lo[c3[c] + pab];
                    if (++bi == BATCH) {
                        FLUSH_BATCH(BATCH);
                        bi = 0;
                    }
                    continue;
                }
                int pac = c2[c] + a, pbc = c2[c] + b;
                for (int dd = c + 1; dd < nr; dd++) {
                    const uint32_t *hd = btab_hi + c3[dd];
                    const unsigned char *ld = btab_lo + c3[dd];
                    uint32_t hx = btab_hi[c3[c] + pab], v;
                    v = hd[pab]; if (v > hx) hx = v;
                    v = hd[pac]; if (v > hx) hx = v;
                    v = hd[pbc]; if (v > hx) hx = v;
                    unsigned lx = btab_lo[c3[c] + pab], w;
                    w = ld[pab]; if (w < lx) lx = w;
                    w = ld[pac]; if (w < lx) lx = w;
                    w = ld[pbc]; if (w < lx) lx = w;
                    L->board[bi] = m3 | rm[dd];
                    L->want_hi[bi] = hx;
                    L->want_lo[bi] = (unsigned char)lx;
                    if (++bi == BATCH) {
                        FLUSH_BATCH(BATCH);
                        bi = 0;
                    }
                }
            }
        }
    if (bi) {
        for (int m = bi; m < BATCH; m++)
            L->board[m] = L->board[bi - 1];
        FLUSH_BATCH(bi);
    }
    return bad;
}

/* -d: the domain term alone over every board of the hole set */
static uint64_t domain_sum(const uint64_t *rm, int nr, uint64_t hh)
{
    uint64_t dsum = 0;
    for (int a = 0; a < nr - nboard + 1; a++)
        for (int b = a + 1; b < nr - nboard + 2; b++)
            for (int c = b + 1; c < nr - nboard + 3; c++) {
                uint64_t m3 = rm[a] | rm[b] | rm[c];
                if (nboard == 3) {
                    dsum += fmix64(hh ^ m3);
                    continue;
                }
                for (int d = c + 1; d < nr - nboard + 4; d++) {
                    uint64_t m4 = m3 | rm[d];
                    if (nboard == 4) {
                        dsum += fmix64(hh ^ m4);
                        continue;
                    }
                    for (int e = d + 1; e < nr; e++)
                        dsum += fmix64(hh ^ (m4 | rm[e]));
                }
            }
    return dsum;
}

static void *worker(void *arg)
{
    (void)arg;
    lanes_t *L = malloc(sizeof *L);
    uint32_t *btab_hi = malloc((NTRIPLES + BATCH) * sizeof *btab_hi);
    unsigned char *btab_lo = malloc(NTRIPLES);
    if (!L || !btab_hi || !btab_lo) {
        fprintf(stderr, "verify_omaha_hilo: out of memory\n");
        exit(1);
    }

    for (long long si; (si = take_set()) >= 0;) {
        int hc[KMAX];
        uint64_t hm, rm[52 + RMPAD];
        unsigned lb[52];
        int nr = hole_set(si, hc, &hm, rm, lb);
        uint64_t hh = fmix64(hm);

        badcnt_t bad = { 0, 0 };
        uint64_t shi = 0, slo = 0, dsum = 0;
        if (domain_only) {
            dsum = domain_sum(rm, nr, hh);
        } else {
            build_btab_hi(L, btab_hi, hc, rm, nr);
            build_btab_lo(btab_lo, hc, lb, nr);
            for (int m = 0; m < BATCH; m++)
                L->hole[m] = hm;
            bad = nboard == 5
                ? sweep5(L, btab_hi, btab_lo, rm, nr, hm, hh,
                         &shi, &slo, &dsum)
                : sweep_short(L, btab_hi, btab_lo, rm, nr, hm, hh,
                              &shi, &slo, &dsum);
        }

        atomic_fetch_add(&stamp_hi_total, shi);
        atomic_fetch_add(&stamp_lo_total, slo);
        atomic_fetch_add(&domain_total, dsum);
        atomic_fetch_add(&bad_hi_total, bad.hi);
        atomic_fetch_add(&bad_lo_total, bad.lo);
        finish_set();
    }
    free(btab_lo);
    free(btab_hi);
    free(L);
    return NULL;
}

int main(int argc, char **argv)
{
    int rc = parse_args(argc, argv, "verify_omaha_hilo", 1);
    if (rc)
        return rc;
    printf("verify_omaha_hilo: omaha%d hi-lo vs pair-triple max/min "
           "references, %lld hole sets x %lld %d-card boards = %.4g "
           "configs, %d threads\n",
           khole, nsets_run, nboards, nboard,
           (double)nsets_run * nboards, nthreads);

    double dt = run_workers(worker, "verify_omaha_hilo");

    double nconf = (double)nsets_run * nboards;
    if (domain_only) {
        printf("%.0f configs in %.1f s (%.1f M/s), domain-only\n",
               nconf, dt, nconf / dt / 1e6);
        printf("domain %016llx (k %d b %d hilo, sets %lld:%lld)\n",
               (unsigned long long)domain_total, khole, nboard,
               first_set, last_set);
        return 0;
    }
    long long bhi = bad_hi_total, blo = bad_lo_total;
    printf("%.0f configs in %.1f s (%.1f M/s): %lld high + %lld low "
           "mismatches%s\n",
           nconf, dt, nconf / dt / 1e6, bhi, blo,
           bhi || blo ? "" : " -- PASS");
    printf("stamp_hi %016llx stamp_lo %016llx domain %016llx "
           "(k %d b %d hilo, sets %lld:%lld)\n",
           (unsigned long long)stamp_hi_total,
           (unsigned long long)stamp_lo_total,
           (unsigned long long)domain_total, khole, nboard,
           first_set, last_set);
    return bhi || blo ? 1 : 0;
}
