/* Scaffolding shared by the omaha gates (verify_omaha, verify_omaha_lo,
 * verify_omaha_hilo) and domain_omaha: the card tables, the hole-set
 * indexing, the stamp primitive, the command line, the work queue and
 * thread pool, and the card-mask printer for mismatch reports.
 * Nothing here reads an evaluator or a reference; each gate's
 * argument, reference fold, and board enumeration live in its own
 * file.
 *
 * Every gate walks the same domain the same way.  Hole sets are indexed
 * 0..C(52,k)-1 in colex order and unranked by the combinatorial number
 * system, so a -p slice names the same hole sets in every tool and
 * slices recombine across tools.  The stamp primitive is the murmur3
 * 64-bit finalizer; each gate defines its stamps as sums of fmix64
 * terms as described in its own header, and the sums are mod 2^64.
 *
 * Command line, common to all four tools:
 *   -k   hole cards, 2..8
 *   -b   board cards, 3..5 (default 5)
 *   -t   worker threads (default: online CPUs)
 *   -p   hole-set index range first[:last], colex order (default: all)
 *   -d   domain-only (the gates; domain_omaha is domain-only by nature)
 *
 * This is a header of static definitions: include it once per tool.
 */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KMAX 8                  /* largest -k */
#define RMPAD 8                 /* zero padding after the remaining cards */
#define NTRIPLES 19600          /* C(50,3), the largest triple table */
#define NPAIRS_MAX 28           /* C(8,2) */
#define NOLOW 0xFFu             /* the low's "no qualifying low" value */

/* ---- tables ------------------------------------------------------------ */

static uint64_t binom[53][KMAX + 1];
static int c2[52], c3[52];      /* C(n,2), C(n,3): triple-table index */
static unsigned lowbit[52];     /* card's low-rank bit, 0x100 = spoiler */

static inline void init_tables(void)
{
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
}

/* murmur3 64-bit finalizer, the stamp's only primitive */
static inline uint64_t fmix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

/* colex unrank: the r-th k-subset of 0..51, ascending into out[] */
static inline void unrank(long long r, int k, int *out)
{
    for (int j = k; j >= 1; j--) {
        int v = j - 1;
        while (binom[v + 1][j] <= (uint64_t)r)
            v++;
        out[j - 1] = v;
        r -= binom[v][j];
    }
}

static inline double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* ---- command line ------------------------------------------------------ */

static int khole;               /* hole cards under test */
static int nboard;              /* board cards, 3..5 */
static int nthreads;
static int domain_only;
static long long first_set, last_set;
static long long nsets_run;     /* hole sets in the -p range */
static long long nboards;       /* C(52-k,nboard) */

/* Parses the options above, fills the tables, and sizes the run.
 * Returns 0 to proceed or the process exit status after printing a
 * usage or range error.  with_d says whether -d is accepted. */
static inline int parse_args(int argc, char **argv, const char *prog,
                             int with_d)
{
    nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    khole = 0;
    nboard = 5;
    domain_only = 0;
    first_set = 0;
    last_set = -1;

    int opt;
    while ((opt = getopt(argc, argv, with_d ? "k:b:t:p:d" : "k:b:t:p:"))
           != -1) {
        switch (opt) {
        case 'k':
            khole = atoi(optarg);
            break;
        case 'b':
            nboard = atoi(optarg);
            break;
        case 't':
            nthreads = atoi(optarg);
            break;
        case 'd':
            domain_only = 1;
            break;
        case 'p': {
            char *colon = strchr(optarg, ':');
            first_set = atoll(optarg);
            last_set = colon ? atoll(colon + 1) : first_set;
            break;
        }
        default:
            goto usage;
        }
    }
    if (khole < 2 || khole > KMAX || nboard < 3 || nboard > 5 ||
        nthreads < 1) {
    usage:
        fprintf(stderr,
                "usage: %s -k holes(2..%d) [-b board(3..5)] [-t threads] "
                "[-p first[:last]]%s\n", prog, KMAX, with_d ? " [-d]" : "");
        return 2;
    }

    init_tables();
    long long nsets = (long long)binom[52][khole];
    nboards = (long long)binom[52 - khole][nboard];
    if (last_set < 0)
        last_set = nsets - 1;
    if (first_set < 0 || last_set >= nsets || first_set > last_set) {
        fprintf(stderr, "%s: bad -p range (0..%lld)\n", prog, nsets - 1);
        return 2;
    }
    nsets_run = last_set - first_set + 1;
    return 0;
}

/* ---- hole sets and the work queue --------------------------------------- */

/* Expands hole set si: its cards ascending into hc[], its mask into
 * *hm, and the 52-k remaining cards as single-bit masks into rm[]
 * (zero-padded RMPAD entries past the end; declare rm[52 + RMPAD]).
 * lb[], if given, gets each remaining card's lowbit.  Returns the
 * remaining-card count. */
static inline int hole_set(long long si, int *hc, uint64_t *hm,
                           uint64_t *rm, unsigned *lb)
{
    unrank(si, khole, hc);
    uint64_t m = 0;
    for (int i = 0; i < khole; i++)
        m |= 1ull << hc[i];
    *hm = m;
    int nr = 0;
    for (int c = 0; c < 52; c++)
        if (!(m >> c & 1)) {
            rm[nr] = 1ull << c;
            if (lb)
                lb[nr] = lowbit[c];
            nr++;
        }
    for (int i = nr; i < nr + RMPAD; i++)
        rm[i] = 0;
    return nr;
}

static _Atomic long long next_set;
static _Atomic long long sets_done;
static long long print_every;

/* the next hole-set index for a worker, or -1 when the range is done */
static inline long long take_set(void)
{
    long long si = atomic_fetch_add(&next_set, 1);
    return si > last_set ? -1 : si;
}

/* a worker finished one hole set: progress to stderr */
static inline void finish_set(void)
{
    long long done = atomic_fetch_add(&sets_done, 1) + 1;
    if (done % print_every == 0 || done == nsets_run)
        fprintf(stderr, "\r  %lld/%lld hole sets", done, nsets_run);
}

/* Runs worker on nthreads threads over the -p range and returns the
 * wall time in seconds.  Exits 2 if the threads cannot be created. */
static inline double run_workers(void *(*worker)(void *), const char *prog)
{
    next_set = first_set;
    sets_done = 0;
    print_every = nsets_run / 500 ? nsets_run / 500 : 1;
    double t0 = now();
    pthread_t *tid = malloc(nthreads * sizeof *tid);
    if (!tid) {
        fprintf(stderr, "%s: out of memory\n", prog);
        exit(2);
    }
    for (int t = 0; t < nthreads; t++)
        pthread_create(&tid[t], NULL, worker, NULL);
    for (int t = 0; t < nthreads; t++)
        pthread_join(tid[t], NULL);
    free(tid);
    fprintf(stderr, "\n");
    return now() - t0;
}

/* ---- mismatch printing ------------------------------------------------- */

static inline void print_mask(const char *tag, uint64_t m)
{
    static const char rankc[] = "23456789TJQKA";
    static const char suitc[] = "cdhs";
    printf(" %s", tag);
    for (int c = 0; c < 52; c++)
        if (m >> c & 1)
            printf(" %c%c", rankc[c >> 2], suitc[c & 3]);
}
