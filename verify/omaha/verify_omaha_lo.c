/* Bootstrap validation of the omaha eight-or-better low evaluator,
 * exhaustive at each hole-card count, every hole count anchored
 * directly to the rules.
 *
 * The 5-card low is simple enough to serve as the in-process reference
 * at every arity, computed from the rules alone.  For hole set H and
 * board B the low must equal the min over the C(k,2) hole pairs and
 * C(5,3) board triples of the rules-only 5-card low -- qualify iff
 * five distinct ranks in ace..eight (ace low), value = the 8-bit rank
 * mask of circuit_eval.h, whose integer order is the lowball order
 * (0xFF = no low), best = min.  So every hole count is independently
 * proven against first principles; they can run in any order and none
 * trusts another.
 *
 * Per hole set the pair minimum folds into the triple table up front:
 * btab[t] = min over hole pairs of low5(pair + triple t), built once
 * over the C(52-k,3) remaining-card triples (colex indexed, triple
 * i < j < k at C(k,3) + C(j,2) + i), and each board's expected low is
 * then the min of 10 lookups with the pair terms hoisted -- the same
 * shape as verify_omaha's triple table.
 *
 * The circuit side runs the low circuit alone through the plane API
 * (bs_transpose + circuit_eval_omaha_low_raw, the low half of
 * circuit_eval_omaha_hilo), skipping the ~13x larger high circuit.
 *
 * Stamps as everywhere in verify/omaha, value = the 8-bit low:
 *
 *   inner  = fmix64(fmix64(hole_mask) ^ board_mask)
 *   domain = sum of inner                     mod 2^64
 *   stamp  = sum of fmix64(inner ^ low)       mod 2^64
 *
 * usage: verify_omaha_lo -k holes [-t threads] [-p first[:last]] [-d]
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
static _Atomic long long bad_total;
static _Atomic unsigned long long stamp_total;
static _Atomic unsigned long long domain_total;
static long long nsets_run;
static long long print_every;
static int domain_only;

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
                       unsigned got, unsigned want)
{
    pthread_mutex_lock(&print_mu);
    if (bad_printed++ < BAD_PRINT_MAX) {
        printf("MISMATCH");
        print_mask("hole", hole);
        print_mask("board", board);
        printf("  omaha%d-low 0x%02x  rules-min 0x%02x\n",
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

/* the low circuit alone on one batch: the low half of
 * circuit_eval_omaha_hilo (in[116]: the board transpose overwrites the
 * hole's unused planes 52..63 and leaves 104..115 unread) */
static void eval_low(const uint64_t hole[BATCH],
                     const uint64_t board[BATCH], uint32_t low[BATCH])
{
    bs_word in[116], out[8];
    bs_transpose(hole, in);
    bs_transpose(board, in + 52);
    circuit_eval_omaha_low_raw(in, out);
    bs_untranspose(out, circuit_eval_omaha_low_raw_num_outputs, low);
}

typedef struct {
    uint64_t hole[BATCH], board[BATCH];
    uint32_t got[BATCH];
    unsigned char want[BATCH];
} lanes_t;

static long long flush_batch(lanes_t *L, int n, uint64_t hm, uint64_t hh,
                             uint64_t *ssum, uint64_t *dsum)
{
    long long bad = 0;
    uint64_t s = 0, d = 0;
    eval_low(L->hole, L->board, L->got);
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

static void *worker(void *arg)
{
    (void)arg;
    lanes_t *L = malloc(sizeof *L);
    unsigned char *btab = malloc(NTRIPLES);
    if (!L || !btab) {
        fprintf(stderr, "verify_omaha_lo: out of memory\n");
        exit(1);
    }
    /* -d skips the table build but the hoisted lookups still execute
     * (their results are unused); keep the reads defined */
    if (domain_only)
        memset(btab, 0, NTRIPLES);

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

        long long bad = 0;
        uint64_t ssum = 0, dsum = 0;
        int bi = 0;

        if (!domain_only) {
            for (int m = 0; m < BATCH; m++)
                L->hole[m] = hm;
            /* rules reference, pair min folded in up front:
             * btab[t] = best low over all hole pairs for triple t */
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

        for (int a = 0; a < nr - 4; a++)
            for (int b = a + 1; b < nr - 3; b++) {
                int pab = c2[b] + a;
                for (int c = b + 1; c < nr - 2; c++) {
                    int pac = c2[c] + a, pbc = c2[c] + b;
                    unsigned mc = btab[c3[c] + pab];
                    uint64_t m3 = rm[a] | rm[b] | rm[c];
                    for (int dd = c + 1; dd < nr - 1; dd++) {
                        int pad = c2[dd] + a, pbd = c2[dd] + b,
                            pcd = c2[dd] + c;
                        unsigned md = mc, v;
                        v = btab[c3[dd] + pab]; if (v < md) md = v;
                        v = btab[c3[dd] + pac]; if (v < md) md = v;
                        v = btab[c3[dd] + pbc]; if (v < md) md = v;
                        uint64_t m4 = m3 | rm[dd];
                        for (int e = dd + 1; e < nr; e++) {
                            uint64_t bmask = m4 | rm[e];
                            if (domain_only) {
                                dsum += fmix64(hh ^ bmask);
                                continue;
                            }
                            const unsigned char *te = btab + c3[e];
                            unsigned mn = md;
                            v = te[pab]; if (v < mn) mn = v;
                            v = te[pac]; if (v < mn) mn = v;
                            v = te[pad]; if (v < mn) mn = v;
                            v = te[pbc]; if (v < mn) mn = v;
                            v = te[pbd]; if (v < mn) mn = v;
                            v = te[pcd]; if (v < mn) mn = v;
                            L->board[bi] = bmask;
                            L->want[bi] = (unsigned char)mn;
                            if (++bi == BATCH) {
                                bad += flush_batch(L, BATCH, hm, hh,
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
            bad += flush_batch(L, bi, hm, hh, &ssum, &dsum);
        }

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
                "usage: verify_omaha_lo -k holes(2..%d) [-t threads] "
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
        fprintf(stderr, "verify_omaha_lo: bad -p range (0..%lld)\n",
                nsets - 1);
        return 2;
    }
    next_set = first;
    nsets_run = last_set - first + 1;
    print_every = nsets_run / 500 ? nsets_run / 500 : 1;
    printf("verify_omaha_lo: omaha%d low vs rules, %lld hole sets x "
           "%lld boards = %.4g configs, %d threads\n",
           khole, nsets_run, nboards,
           (double)nsets_run * nboards, nthreads);

    double t0 = now();
    pthread_t *tid = malloc(nthreads * sizeof *tid);
    if (!tid) {
        fprintf(stderr, "verify_omaha_lo: out of memory\n");
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
        printf("domain %016llx (k %d low, sets %lld:%lld)\n",
               (unsigned long long)domain_total, khole,
               first, last_set);
        return 0;
    }
    long long bad = bad_total;
    printf("%.0f configs in %.1f s (%.1f M/s): %lld mismatches%s\n",
           nconf, dt, nconf / dt / 1e6, bad, bad ? "" : " -- PASS");
    printf("stamp %016llx domain %016llx (k %d low, sets %lld:%lld)\n",
           (unsigned long long)stamp_total,
           (unsigned long long)domain_total, khole, first, last_set);
    return bad ? 1 : 0;
}
