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
 * them in any order.
 *
 * usage: verify_omaha_hilo -k holes [-t threads] [-p first[:last]] [-d]
 *   -k   hole cards to validate, 2..8 (each stands alone)
 *   -t   worker threads (default: online CPUs)
 *   -p   hole-set index range, colex order, for partial runs
 *   -d   domain-only: no evaluation, compute just the domain stamp
 *
 * Exits 0 on a clean sweep, 1 if any configuration mismatched.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "circuit_eval.h"

#define KMAX 8
#define BATCH BS_BATCH          /* library batch size (bsapi.h) */
#define NTRIPLES 19600          /* C(50,3), the largest triple table */
#define NPAIRS_MAX 28           /* C(8,2) */
#define NOLOW 0xFFu

static uint64_t binom[53][KMAX + 1];
static int c2[52], c3[52];      /* C(n,2), C(n,3): triple-table index */
static unsigned lowbit[52];     /* rank's low mask bit, 0x100 = spoiler */
static int khole;               /* hole cards under test */
static long long nboards;       /* C(52-k,5) */

static long long last_set;
static _Atomic long long next_set;
static _Atomic long long sets_done;
static _Atomic long long bad_hi_total;
static _Atomic long long bad_lo_total;
static _Atomic unsigned long long stamp_hi_total;
static _Atomic unsigned long long stamp_lo_total;
static _Atomic unsigned long long domain_total;
static long long nsets_run;
static long long print_every;
static int domain_only;

/* Mismatch printing is capped; the counts are exact regardless. */
#define BAD_PRINT_MAX 20
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static int bad_printed;

static const char rankc[] = "23456789TJQKA";
static const char suitc[] = "cdhs";

static void print_mask(const char *tag, uint64_t m)
{
    printf(" %s", tag);
    for (int c = 0; c < 52; c++)
        if (m >> c & 1)
            printf(" %c%c", rankc[c >> 2], suitc[c & 3]);
}

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

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* murmur3 64-bit finalizer, the stamp's only primitive */
static uint64_t fmix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

/* colex unrank: the r-th k-subset of 0..51, ascending into out[] */
static void unrank(long long r, int k, int *out)
{
    for (int j = k; j >= 1; j--) {
        int v = j - 1;
        while (binom[v + 1][j] <= (uint64_t)r)
            v++;
        out[j - 1] = v;
        r -= binom[v][j];
    }
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
static void build_btab_hi(lanes_t *L, uint32_t *btab, const uint64_t *pm,
                          int np, const uint64_t *rm, int nr)
{
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

    for (;;) {
        long long si = atomic_fetch_add(&next_set, 1);
        if (si > last_set)
            break;

        int hc[KMAX];
        unrank(si, khole, hc);
        uint64_t hm = 0;
        for (int i = 0; i < khole; i++)
            hm |= 1ull << hc[i];
        uint64_t hh = fmix64(hm);

        int nr = 0;
        uint64_t rm[52];
        unsigned lb[52];
        for (int c = 0; c < 52; c++)
            if (!(hm >> c & 1)) {
                rm[nr] = 1ull << c;
                lb[nr] = lowbit[c];
                nr++;
            }

        badcnt_t bad = { 0, 0 };
        uint64_t shi = 0, slo = 0, dsum = 0;

        if (domain_only) {
            for (int a = 0; a < nr - 4; a++)
                for (int b = a + 1; b < nr - 3; b++) {
                    uint64_t m2 = rm[a] | rm[b];
                    for (int c = b + 1; c < nr - 2; c++) {
                        uint64_t m3 = m2 | rm[c];
                        for (int d = c + 1; d < nr - 1; d++) {
                            uint64_t m4 = m3 | rm[d];
                            for (int e = d + 1; e < nr; e++)
                                dsum += fmix64(hh ^ (m4 | rm[e]));
                        }
                    }
                }
            goto tally;
        }

        {
            uint64_t pm[NPAIRS_MAX];
            int np = 0;
            for (int i = 0; i < khole; i++)
                for (int j = i + 1; j < khole; j++)
                    pm[np++] = 1ull << hc[i] | 1ull << hc[j];
            build_btab_hi(L, btab_hi, pm, np, rm, nr);
        }
        build_btab_lo(btab_lo, hc, lb, nr);
        for (int m = 0; m < BATCH; m++)
            L->hole[m] = hm;

        {
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
                                    badcnt_t b2 = eval_batch(L, BATCH,
                                        hm, hh, &shi, &slo, &dsum);
                                    bad.hi += b2.hi;
                                    bad.lo += b2.lo;
                                    bi = 0;
                                }
                            }
                        }
                    }
                }
            if (bi) {
                for (int m = bi; m < BATCH; m++)
                    L->board[m] = L->board[bi - 1];
                badcnt_t b2 = eval_batch(L, bi, hm, hh,
                                         &shi, &slo, &dsum);
                bad.hi += b2.hi;
                bad.lo += b2.lo;
            }
        }

    tally:
        atomic_fetch_add(&stamp_hi_total, shi);
        atomic_fetch_add(&stamp_lo_total, slo);
        atomic_fetch_add(&domain_total, dsum);
        atomic_fetch_add(&bad_hi_total, bad.hi);
        atomic_fetch_add(&bad_lo_total, bad.lo);
        long long done = atomic_fetch_add(&sets_done, 1) + 1;
        if (done % print_every == 0 || done == nsets_run)
            fprintf(stderr, "\r  %lld/%lld hole sets", done, nsets_run);
    }
    free(btab_lo);
    free(btab_hi);
    free(L);
    return NULL;
}

int main(int argc, char **argv)
{
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    long long first = 0;
    last_set = -1;
    khole = 0;

    int opt;
    while ((opt = getopt(argc, argv, "k:t:p:d")) != -1) {
        switch (opt) {
        case 'k':
            khole = atoi(optarg);
            break;
        case 't':
            nthreads = atoi(optarg);
            break;
        case 'd':
            domain_only = 1;
            break;
        case 'p': {
            char *colon = strchr(optarg, ':');
            first = atoll(optarg);
            last_set = colon ? atoll(colon + 1) : first;
            break;
        }
        default:
            goto usage;
        }
    }
    if (khole < 2 || khole > KMAX || nthreads < 1) {
    usage:
        fprintf(stderr,
                "usage: verify_omaha_hilo -k holes(2..%d) [-t threads] "
                "[-p first[:last]] [-d]\n", KMAX);
        return 2;
    }

    for (int n = 0; n <= 52; n++) {
        binom[n][0] = 1;
        for (int j = 1; j <= KMAX; j++)
            binom[n][j] = n ? binom[n - 1][j - 1] + binom[n - 1][j] : 0;
    }
    for (int n = 0; n < 52; n++) {
        c2[n] = (int)binom[n][2];
        c3[n] = (int)binom[n][3];
    }
    for (int c = 0; c < 52; c++) {
        int r = c >> 2;
        lowbit[c] = r == 12 ? 1u : r <= 6 ? 1u << (r + 1) : 0x100u;
    }

    long long nsets = (long long)binom[52][khole];
    int r = 52 - khole;
    nboards = (long long)r * (r - 1) * (r - 2) * (r - 3) * (r - 4) / 120;

    if (last_set < 0)
        last_set = nsets - 1;
    if (first < 0 || last_set >= nsets || first > last_set) {
        fprintf(stderr, "verify_omaha_hilo: bad -p range (0..%lld)\n",
                nsets - 1);
        return 2;
    }
    next_set = first;
    nsets_run = last_set - first + 1;
    print_every = nsets_run / 500 ? nsets_run / 500 : 1;
    printf("verify_omaha_hilo: omaha%d hi-lo vs pair-triple max/min "
           "references, %lld hole sets x %lld boards = %.4g configs, "
           "%d threads\n",
           khole, nsets_run, nboards,
           (double)nsets_run * nboards, nthreads);

    double t0 = now();
    pthread_t *tid = malloc(nthreads * sizeof *tid);
    if (!tid) {
        fprintf(stderr, "verify_omaha_hilo: out of memory\n");
        return 2;
    }
    for (int t = 0; t < nthreads; t++)
        pthread_create(&tid[t], NULL, worker, NULL);
    for (int t = 0; t < nthreads; t++)
        pthread_join(tid[t], NULL);
    double dt = now() - t0;
    fprintf(stderr, "\n");

    double nconf = (double)nsets_run * nboards;
    if (domain_only) {
        printf("%.0f configs in %.1f s (%.1f M/s), domain-only\n",
               nconf, dt, nconf / dt / 1e6);
        printf("domain %016llx (k %d hilo, sets %lld:%lld)\n",
               (unsigned long long)domain_total, khole,
               first, last_set);
        return 0;
    }
    long long bhi = bad_hi_total, blo = bad_lo_total;
    printf("%.0f configs in %.1f s (%.1f M/s): %lld high + %lld low "
           "mismatches%s\n",
           nconf, dt, nconf / dt / 1e6, bhi, blo,
           bhi || blo ? "" : " -- PASS");
    printf("stamp_hi %016llx stamp_lo %016llx domain %016llx "
           "(k %d hilo, sets %lld:%lld)\n",
           (unsigned long long)stamp_hi_total,
           (unsigned long long)stamp_lo_total,
           (unsigned long long)domain_total, khole, first, last_set);
    return bhi || blo ? 1 : 0;
}
