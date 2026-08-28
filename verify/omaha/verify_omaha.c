/* Bootstrap validation of the omaha evaluator, exhaustive at each
 * hole-card count, every hole count anchored directly to the validated
 * 5-card primitive.
 *
 * Omaha uses exactly 2 hole cards and exactly 3 board cards, whatever
 * the hole size, so the value of (hole H, board B) must equal the max
 * over the C(k,2) hole pairs and C(5,3) board triples of the 5-card
 * rank of pair + triple.  The 5-card primitive is the holdem evaluator
 * at 5-card inputs, exhaustively anchored by the verify_holdem /
 * omporacle5 stamp comparison, and the fold below is plain algebra
 * (the max over pairs and triples jointly is the max over triples of
 * the per-triple pair max), so a PASS at k means the shipped
 * omaha evaluator agrees with validated-primitive-plus-omaha-rules on
 * every configuration.  No hole count depends on another; run them in any
 * order.
 *
 * Per hole set the pair max folds into a triple table up front:
 * btab[t] = max over the hole pairs of holdem5(pair + triple t), built
 * once over the C(52-k,3) remaining-card triples (colex indexed,
 * triple i < j < k at C(k,3) + C(j,2) + i, batched BS_BATCH through
 * circuit_eval_holdem).  Each board's expected value is then the max
 * of 10 lookups, the pair terms hoisted out of the inner loop, and the
 * evaluator under test (circuit_eval_omaha) runs once per board,
 * batched.  The build amortizes to under a nanosecond per
 * configuration at every k.  Sizes grow steeply with k:
 *
 *   k    configurations    k    configurations
 *   2    2.8e9             6    2.8e13
 *   3    4.2e10            7    1.6e14
 *   4    4.6e11            8    8.2e14
 *   5    4.0e12
 *
 * Hole sets are indexed 0..C(52,k)-1 in colex order (unranked by the
 * combinatorial number system), so -p partitions a domain into resumable
 * slices for the larger k.
 *
 * usage: verify_omaha -k holes [-t threads] [-p first[:last]] [-d]
 *   -k   hole cards to validate, 2..8 (each stands alone)
 *   -t   worker threads (default: online CPUs)
 *   -p   hole-set index range, for partial runs (default: the whole domain)
 *   -d   domain-only: no evaluation, compute just the domain stamp
 *
 * The final line carries a 64-bit stamp certifying the computed values:
 *
 *   stamp = sum over every (hole, board) configuration of
 *           fmix64(fmix64(fmix64(hole_mask) ^ board_mask) ^ value) mod 2^64
 *
 * where fmix64 is the murmur3 64-bit finalizer and value is the
 * omaha-at-k evaluator output under test.  Each configuration
 * contributes one term keyed only by its card masks, so the stamp is
 * independent of traversal order, batch width, thread count, and
 * backend: any implementation that evaluates every configuration
 * exactly once reproduces it, and stamps of disjoint -p slices sum to
 * the full-domain stamp.  Two runs printing the same stamp computed
 * identical values over the identical domain.
 *
 * Alongside it the run prints a domain stamp, the same sum over the
 * inner (value-free) term alone:
 *
 *   domain = sum of fmix64(fmix64(hole_mask) ^ board_mask)  mod 2^64
 *
 * This is evaluator-independent -- a constant of the domain at k --
 * and -d computes it without running the evaluators at all.  When
 * recombining partial runs (the intended way to spread the big domains
 * over many machines), accept the sum of the slice value stamps only
 * after the slice domain stamps sum to that constant: any gap, overlap,
 * or duplicated slice shifts the domain sum.
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

static uint64_t binom[53][KMAX + 1];
static int c2[52], c3[52];      /* C(n,2), C(n,3): triple-table index */
static int khole;               /* hole cards under test */
static long long nboards;       /* C(52-k,5) */

static long long last_set;
static _Atomic long long next_set;
static _Atomic long long sets_done;
static _Atomic long long bad_total;
static _Atomic unsigned long long stamp_total;
static _Atomic unsigned long long domain_total;
static long long nsets_run;
static long long print_every;
static int domain_only;

/* Mismatch printing is capped; the count is exact regardless. */
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

static void report_bad(uint64_t hole, uint64_t board,
                       uint32_t got, uint32_t want)
{
    pthread_mutex_lock(&print_mu);
    if (bad_printed++ < BAD_PRINT_MAX) {
        printf("MISMATCH");
        print_mask("hole", hole);
        print_mask("board", board);
        printf("  omaha%d 0x%06x  holdem5-max 0x%06x\n",
               khole, got, want);
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
    uint32_t got[BATCH], want[BATCH];
} lanes_t;

static long long eval_batch(lanes_t *L, int n, uint64_t hm, uint64_t hh,
                            uint64_t *ssum, uint64_t *dsum)
{
    long long bad = 0;
    uint64_t s = 0, d = 0;
    circuit_eval_omaha(L->hole, L->board, L->got);
    for (int m = 0; m < n; m++) {
        uint64_t inner = fmix64(hh ^ L->board[m]);
        d += inner;
        s += fmix64(inner ^ L->got[m]);
        if (L->got[m] != L->want[m]) {
            bad++;
            report_bad(hm, L->board[m], L->got[m], L->want[m]);
        }
    }
    *ssum += s;
    *dsum += d;
    return bad;
}

/* btab[t] = max over the hole pairs of holdem5(pair + triple t), one
 * colex pass over the remaining-card triples per pair, batched BATCH
 * (L->board is the 5-card hand scratch here) */
static void build_btab(lanes_t *L, uint32_t *btab, const uint64_t *pm,
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
                        circuit_eval_holdem(L->board, L->got);
                        for (int m = 0; m < BATCH; m++)
                            if (L->got[m] > btab[off + m])
                                btab[off + m] = L->got[m];
                        off += BATCH;
                        bi = 0;
                    }
                }
        if (bi) {
            for (int m = bi; m < BATCH; m++)
                L->board[m] = L->board[bi - 1];
            circuit_eval_holdem(L->board, L->got);
            for (int m = 0; m < bi; m++)
                if (L->got[m] > btab[off + m])
                    btab[off + m] = L->got[m];
        }
    }
}

static void *worker(void *arg)
{
    (void)arg;
    lanes_t *L = malloc(sizeof *L);
    uint32_t *btab = malloc((NTRIPLES + BATCH) * sizeof *btab);
    if (!L || !btab) {
        fprintf(stderr, "verify_omaha: out of memory\n");
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
        for (int c = 0; c < 52; c++)
            if (!(hm >> c & 1))
                rm[nr++] = 1ull << c;

        long long bad = 0;
        uint64_t ssum = 0, dsum = 0;

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
            build_btab(L, btab, pm, np, rm, nr);
        }
        for (int m = 0; m < BATCH; m++)
            L->hole[m] = hm;

        {
            int bi = 0;
            for (int a = 0; a < nr - 4; a++)
                for (int b = a + 1; b < nr - 3; b++) {
                    int pab = c2[b] + a;
                    for (int c = b + 1; c < nr - 2; c++) {
                        int pac = c2[c] + a, pbc = c2[c] + b;
                        uint32_t mc = btab[c3[c] + pab];
                        uint64_t m3 = rm[a] | rm[b] | rm[c];
                        for (int dd = c + 1; dd < nr - 1; dd++) {
                            int pad = c2[dd] + a, pbd = c2[dd] + b,
                                pcd = c2[dd] + c;
                            uint32_t md = mc, v;
                            v = btab[c3[dd] + pab]; if (v > md) md = v;
                            v = btab[c3[dd] + pac]; if (v > md) md = v;
                            v = btab[c3[dd] + pbc]; if (v > md) md = v;
                            uint64_t m4 = m3 | rm[dd];
                            for (int e = dd + 1; e < nr; e++) {
                                const uint32_t *te = btab + c3[e];
                                uint32_t mx = md;
                                v = te[pab]; if (v > mx) mx = v;
                                v = te[pac]; if (v > mx) mx = v;
                                v = te[pad]; if (v > mx) mx = v;
                                v = te[pbc]; if (v > mx) mx = v;
                                v = te[pbd]; if (v > mx) mx = v;
                                v = te[pcd]; if (v > mx) mx = v;
                                L->board[bi] = m4 | rm[e];
                                L->want[bi] = mx;
                                if (++bi == BATCH) {
                                    bad += eval_batch(L, BATCH, hm, hh,
                                                      &ssum, &dsum);
                                    bi = 0;
                                }
                            }
                        }
                    }
                }
            if (bi) {
                for (int m = bi; m < BATCH; m++)
                    L->board[m] = L->board[bi - 1];
                bad += eval_batch(L, bi, hm, hh, &ssum, &dsum);
            }
        }

    tally:
        atomic_fetch_add(&stamp_total, ssum);
        atomic_fetch_add(&domain_total, dsum);
        atomic_fetch_add(&bad_total, bad);
        long long done = atomic_fetch_add(&sets_done, 1) + 1;
        if (done % print_every == 0 || done == nsets_run)
            fprintf(stderr, "\r  %lld/%lld hole sets", done, nsets_run);
    }
    free(btab);
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
                "usage: verify_omaha -k holes(2..%d) [-t threads] "
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

    long long nsets = (long long)binom[52][khole];
    int r = 52 - khole;
    nboards = (long long)r * (r - 1) * (r - 2) * (r - 3) * (r - 4) / 120;

    if (last_set < 0)
        last_set = nsets - 1;
    if (first < 0 || last_set >= nsets || first > last_set) {
        fprintf(stderr, "verify_omaha: bad -p range (0..%lld)\n",
                nsets - 1);
        return 2;
    }
    next_set = first;
    nsets_run = last_set - first + 1;
    print_every = nsets_run / 500 ? nsets_run / 500 : 1;
    printf("verify_omaha: omaha%d vs pair-triple max of holdem5, "
           "%lld hole sets x %lld boards = %.4g configs, %d threads\n",
           khole, nsets_run, nboards,
           (double)nsets_run * nboards, nthreads);

    double t0 = now();
    pthread_t *tid = malloc(nthreads * sizeof *tid);
    if (!tid) {
        fprintf(stderr, "verify_omaha: out of memory\n");
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
        printf("domain %016llx (k %d, sets %lld:%lld)\n",
               (unsigned long long)domain_total, khole,
               first, last_set);
        return 0;
    }
    long long bad = bad_total;
    printf("%.0f configs in %.1f s (%.1f M/s): %lld mismatches%s\n",
           nconf, dt, nconf / dt / 1e6, bad, bad ? "" : " -- PASS");
    printf("stamp %016llx domain %016llx (k %d, sets %lld:%lld)\n",
           (unsigned long long)stamp_total,
           (unsigned long long)domain_total, khole, first, last_set);
    return bad ? 1 : 0;
}
