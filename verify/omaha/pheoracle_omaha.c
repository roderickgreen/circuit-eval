/* Standalone omaha high oracle: the omaha stamps at 4, 5 and 6 hole
 * cards computed WITHOUT the circuit evaluator.
 *
 * Prints the same stamp pair as verify_omaha -k K (see there for the
 * definitions and the colex hole-set indexing, which -p shares), but
 * derives every configuration's 24-bit value from two sources that
 * share no code with this repo's evaluator stack:
 *
 *   ordering   PokerHandEvaluator (third-party/PokerHandEvaluator), its
 *              dedicated evaluate_plo4/5/6_cards evaluators: two
 *              configurations compare the same way iff their PHE ranks
 *              do
 *   encoding   ../holdem/valmap.h, the 7462 class values by dense rank
 *              -- a checked-in table generated from
 *              flow/encoding/mkspec.py (the encoding is defined in
 *              ENCODING.md)
 *
 * The alignment is built the way omporacle5 builds it: all C(52,5)
 * hands through PHE's evaluate_5cards must produce exactly 7462
 * distinct ranks, and sorting them (PHE ranks shrink with strength)
 * aligns them one-to-one with the spec's classes strongest to weakest.
 * The count check makes that the unique order-preserving bijection
 * between two total orders of equal size.  An omaha hand is a 5-card
 * hand, so PHE's plo evaluators return ranks from the same set; a rank
 * outside it is reported and counted as a failure.
 *
 * Validation is then: run this oracle, run verify_omaha -k K over the
 * same hole-set range, compare stamp lines.  Equal stamps mean the
 * circuit evaluator agrees with PHE's omaha ordering and the spec's
 * encoding on every configuration in the range.  PHE evaluates one
 * configuration in tens of nanoseconds, so a full sweep costs roughly
 * 30x the circuit gate's: k 4 is minutes on a workstation, k 6 hours.
 *
 * usage: pheoracle_omaha -k holes [-t threads] [-p first[:last]] [-d]
 *   -k   hole cards, 4..6 (the games PHE evaluates)
 *   -t   worker threads (default: online CPUs)
 *   -p   hole-set index range, colex order, for partial runs
 *   -d   domain-only: no evaluation, compute just the domain stamp
 *
 * Exits 0 and prints the stamps, or 1 on any oracle inconsistency.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "phevaluator/phevaluator.h"
#include "../holdem/valmap.h"

#define KMIN 4
#define KMAX 6
#define NCLASSES 7462
#define RANKMAX 8192            /* PHE ranks are 1..7462 */

static uint64_t binom[53][KMAX + 1];
static int khole;
static long long nboards;       /* C(52-k,5) */
static uint32_t val_by_rank[RANKMAX];   /* 0 = rank never seen at 5 cards */

static long long last_set;
static _Atomic long long next_set;
static _Atomic long long sets_done;
static _Atomic long long bad_total;
static _Atomic unsigned long long stamp_total;
static _Atomic unsigned long long domain_total;
static long long nsets_run;
static long long print_every;
static int domain_only;

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

/* PHE's omaha rank of board a..e with the hole cards h[], by game */
static int plo_rank(const int *h, int a, int b, int c, int d, int e)
{
    switch (khole) {
    case 4:
        return evaluate_plo4_cards(a, b, c, d, e, h[0], h[1], h[2], h[3]);
    case 5:
        return evaluate_plo5_cards(a, b, c, d, e, h[0], h[1], h[2], h[3],
                                   h[4]);
    default:
        return evaluate_plo6_cards(a, b, c, d, e, h[0], h[1], h[2], h[3],
                                   h[4], h[5]);
    }
}

static void *worker(void *arg)
{
    (void)arg;
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
        int rc[52];             /* remaining cards, ascending */
        for (int c = 0; c < 52; c++)
            if (!(hm >> c & 1))
                rc[nr++] = c;

        long long bad = 0;
        uint64_t ssum = 0, dsum = 0;
        for (int a = 0; a < nr - 4; a++)
            for (int b = a + 1; b < nr - 3; b++) {
                uint64_t m2 = 1ull << rc[a] | 1ull << rc[b];
                for (int c = b + 1; c < nr - 2; c++) {
                    uint64_t m3 = m2 | 1ull << rc[c];
                    for (int d = c + 1; d < nr - 1; d++) {
                        uint64_t m4 = m3 | 1ull << rc[d];
                        for (int e = d + 1; e < nr; e++) {
                            uint64_t inner = fmix64(hh ^ (m4 | 1ull << rc[e]));
                            dsum += inner;
                            if (domain_only)
                                continue;
                            int r = plo_rank(hc, rc[a], rc[b], rc[c],
                                             rc[d], rc[e]);
                            uint32_t v = r > 0 && r < RANKMAX
                                         ? val_by_rank[r] : 0;
                            if (!v)
                                bad++;
                            ssum += fmix64(inner ^ v);
                        }
                    }
                }
            }

        atomic_fetch_add(&stamp_total, ssum);
        atomic_fetch_add(&domain_total, dsum);
        atomic_fetch_add(&bad_total, bad);
        long long done = atomic_fetch_add(&sets_done, 1) + 1;
        if (done % print_every == 0 || done == nsets_run)
            fprintf(stderr, "\r  %lld/%lld hole sets", done, nsets_run);
    }
    return NULL;
}

/* val_by_rank from PHE's 5-card ranks and VALMAP: exactly 7462
 * distinct ranks, ascending rank = descending strength = ascending
 * VALMAP index */
static int build_valmap(void)
{
    for (int r = 0; r < NCLASSES; r++)
        if (VALMAP[r] >> 24 || (r && VALMAP[r] >= VALMAP[r - 1])) {
            fprintf(stderr, "pheoracle_omaha: VALMAP broken at rank %d\n",
                    r + 1);
            return 1;
        }
    static char rank_occurs[RANKMAX];
    for (int a = 0; a < 48; a++)
        for (int b = a + 1; b < 49; b++)
            for (int c = b + 1; c < 50; c++)
                for (int d = c + 1; d < 51; d++)
                    for (int e = d + 1; e < 52; e++) {
                        int r = evaluate_5cards(a, b, c, d, e);
                        if (r <= 0 || r >= RANKMAX) {
                            fprintf(stderr, "pheoracle_omaha: 5-card "
                                    "rank %d out of range\n", r);
                            return 1;
                        }
                        rank_occurs[r] = 1;
                    }
    int nranks = 0;
    for (int r = 0; r < RANKMAX; r++)
        if (rank_occurs[r])
            val_by_rank[r] = VALMAP[nranks++];
    if (nranks != NCLASSES) {
        fprintf(stderr, "pheoracle_omaha: %d distinct PHE ranks, want %d\n",
                nranks, NCLASSES);
        return 1;
    }
    return 0;
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
    if (khole < KMIN || khole > KMAX || nthreads < 1) {
    usage:
        fprintf(stderr,
                "usage: pheoracle_omaha -k holes(%d..%d) [-t threads] "
                "[-p first[:last]] [-d]\n", KMIN, KMAX);
        return 2;
    }

    for (int n = 0; n <= 52; n++) {
        binom[n][0] = 1;
        for (int j = 1; j <= KMAX; j++)
            binom[n][j] = n ? binom[n - 1][j - 1] + binom[n - 1][j] : 0;
    }

    long long nsets = (long long)binom[52][khole];
    int r = 52 - khole;
    nboards = (long long)r * (r - 1) * (r - 2) * (r - 3) * (r - 4) / 120;

    if (last_set < 0)
        last_set = nsets - 1;
    if (first < 0 || last_set >= nsets || first > last_set) {
        fprintf(stderr, "pheoracle_omaha: bad -p range (0..%lld)\n",
                nsets - 1);
        return 2;
    }
    next_set = first;
    nsets_run = last_set - first + 1;
    print_every = nsets_run / 500 ? nsets_run / 500 : 1;

    if (!domain_only && build_valmap())
        return 1;

    printf("pheoracle_omaha: PHE plo%d + the encoding spec, "
           "%lld hole sets x %lld boards = %.4g configs, %d threads\n",
           khole, nsets_run, nboards,
           (double)nsets_run * nboards, nthreads);

    double t0 = now();
    pthread_t *tid = malloc(nthreads * sizeof *tid);
    if (!tid) {
        fprintf(stderr, "pheoracle_omaha: out of memory\n");
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
               (unsigned long long)domain_total, khole, first, last_set);
        return 0;
    }
    long long bad = bad_total;
    printf("%.0f configs in %.1f s (%.1f M/s)%s\n",
           nconf, dt, nconf / dt / 1e6,
           bad ? "" : ", every rank in the 5-card set");
    if (bad) {
        printf("%lld configurations returned a rank outside the 5-card "
               "set -- oracle inconsistent\n", bad);
        return 1;
    }
    printf("stamp %016llx domain %016llx (k %d, sets %lld:%lld)\n",
           (unsigned long long)stamp_total,
           (unsigned long long)domain_total, khole, first, last_set);
    return 0;
}
