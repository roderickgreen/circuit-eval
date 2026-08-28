/* Bootstrap validation for holdem, exhaustive at each arity.
 *
 * -k 5 produces stamps only: every C(52,5) hand through the deployed
 * evaluator (circuit_eval_holdem), no oracle in-process.  Its
 * counterpart is omporacle5, which prints the same stamp line from
 * OMPEval plus the embedded encoding table and never touches the
 * circuit stack.  The 5-card validation is running both binaries and
 * comparing the two lines: equal stamps mean the circuit agrees with
 * OMPEval's ordering and the spec's encoding on every hand (see
 * omporacle5.cpp for why that alignment is forced, not fitted).
 * OMPEval is a second oracle independent of the PHE gate in
 * verify/holdem/verify_holdem.sh: the two verification paths share no oracle code.
 *
 * -k 6 and -k 7 are a drop-one induction: best-5-of-n means the value
 * at n cards must equal the max over the n ways to drop one card of the
 * value at n-1, both sides through circuit_eval_holdem.  A PASS at 6
 * given 5, and at 7 given 6, carries the oracle anchor up to the 7-card
 * contract.
 *
 * Domain sizes are small (C(52,5/6/7) = 2,598,960 / 20,358,520 /
 * 133,784,560), so every hand size is exhaustive in seconds.
 *
 * usage: verify_holdem -k cards [-t threads] [-p first[:last]] [-d]
 *   -k   hand size: 5 (stamps for omporacle5), 6 or 7 (vs k-1)
 *   -t   worker threads (default: online CPUs)
 *   -p   hand index range in colex order, for partial runs (default: all)
 *   -d   domain-only: no evaluation, compute just the domain stamp
 *
 * The final line carries the same stamp pair as the omaha gates, with
 * the single hand mask as the configuration key:
 *
 *   inner  = fmix64(hand_mask)
 *   domain = sum of inner                     mod 2^64
 *   stamp  = sum of fmix64(inner ^ value)     mod 2^64
 *
 * (fmix64 = the murmur3 64-bit finalizer, value = the circuit output.)
 * Both sums are invariant to traversal order, batch width, thread count,
 * and backend, and slice stamps sum to the full-domain stamps; recombine
 * partial runs only after the domain stamps sum to the published
 * constant.
 *
 * Exits 0 on a clean sweep, 1 on any mismatch.
 */
#include <atomic>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "circuit_eval.h"

#define KMAX 7
#define BATCH BS_BATCH          /* library batch size (bsapi.h) */
#define BLOCK 2048              /* hand indices claimed per work grab */

static uint64_t binom[53][KMAX + 1];
static int kcards;              /* hand size under test */

static long long first_set, last_set;
static std::atomic<long long> next_block;
static std::atomic<long long> sets_done;
static std::atomic<long long> bad_total;
static std::atomic<unsigned long long> stamp_total;
static std::atomic<unsigned long long> domain_total;
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

static void report_bad(uint64_t hand, const char *what,
                       uint32_t got, uint32_t want)
{
    pthread_mutex_lock(&print_mu);
    if (bad_printed++ < BAD_PRINT_MAX) {
        printf("MISMATCH");
        print_mask("hand", hand);
        printf("  %s: 0x%06x vs 0x%06x\n", what, got, want);
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

/* colex successor of an ascending k-subset */
static void next_comb(int *c, int k)
{
    int j = 0;
    while (j + 1 < k && c[j] + 1 == c[j + 1])
        j++;
    c[j]++;
    for (int i = 0; i < j; i++)
        c[i] = i;
}

typedef struct {
    uint64_t hands[BATCH];
    uint64_t sub[KMAX][BATCH];          /* drop-one masks, -k 6/7 */
    uint32_t got[BATCH], want[BATCH], tmp[BATCH];
} lanes_t;

static long long flush_batch(lanes_t *L, int n,
                             uint64_t *ssum, uint64_t *dsum)
{
    long long bad = 0;
    uint64_t s = 0, d = 0;
    circuit_eval_holdem(L->hands, L->got);
    if (kcards > 5) {
        circuit_eval_holdem(L->sub[0], L->want);
        for (int i = 1; i < kcards; i++) {
            circuit_eval_holdem(L->sub[i], L->tmp);
            for (int m = 0; m < BATCH; m++)
                if (L->tmp[m] > L->want[m])
                    L->want[m] = L->tmp[m];
        }
        for (int m = 0; m < n; m++)
            if (L->got[m] != L->want[m]) {
                bad++;
                report_bad(L->hands[m], "holdem vs drop-one max",
                           L->got[m], L->want[m]);
            }
    }
    for (int m = 0; m < n; m++) {
        uint64_t inner = fmix64(L->hands[m]);
        d += inner;
        s += fmix64(inner ^ L->got[m]);
    }
    *ssum += s;
    *dsum += d;
    return bad;
}

static void *worker(void *arg)
{
    (void)arg;
    lanes_t *L = (lanes_t *)malloc(sizeof *L);
    if (!L) {
        fprintf(stderr, "verify_holdem: out of memory\n");
        exit(1);
    }

    for (;;) {
        long long start = first_set + next_block.fetch_add(1) * BLOCK;
        if (start > last_set)
            break;
        long long end = start + BLOCK - 1;
        if (end > last_set)
            end = last_set;

        int c[KMAX];
        unrank(start, kcards, c);

        long long bad = 0;
        uint64_t ssum = 0, dsum = 0;
        int bi = 0;
        for (long long si = start; si <= end; si++) {
            uint64_t hm = 0;
            for (int i = 0; i < kcards; i++)
                hm |= 1ull << c[i];
            if (domain_only) {
                dsum += fmix64(hm);
            } else {
                L->hands[bi] = hm;
                if (kcards > 5)
                    for (int i = 0; i < kcards; i++)
                        L->sub[i][bi] = hm & ~(1ull << c[i]);
                if (++bi == BATCH) {
                    bad += flush_batch(L, BATCH, &ssum, &dsum);
                    bi = 0;
                }
            }
            if (si < end)
                next_comb(c, kcards);
        }
        if (bi) {
            for (int m = bi; m < BATCH; m++) {
                L->hands[m] = L->hands[bi - 1];
                if (kcards > 5)
                    for (int i = 0; i < kcards; i++)
                        L->sub[i][m] = L->sub[i][bi - 1];
            }
            bad += flush_batch(L, bi, &ssum, &dsum);
        }

        stamp_total.fetch_add(ssum);
        domain_total.fetch_add(dsum);
        bad_total.fetch_add(bad);
        long long done = sets_done.fetch_add(end - start + 1) +
                         (end - start + 1);
        if (done / print_every !=
            (done - (end - start + 1)) / print_every || done == nsets_run)
            fprintf(stderr, "\r  %lld/%lld hands", done, nsets_run);
    }
    free(L);
    return NULL;
}

int main(int argc, char **argv)
{
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    long long first = 0;
    last_set = -1;
    kcards = 0;

    int opt;
    while ((opt = getopt(argc, argv, "k:t:p:d")) != -1) {
        switch (opt) {
        case 'k':
            kcards = atoi(optarg);
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
    if (kcards < 5 || kcards > KMAX || nthreads < 1) {
    usage:
        fprintf(stderr, "usage: verify_holdem -k cards(5..%d) "
                        "[-t threads] [-p first[:last]] [-d]\n", KMAX);
        return 2;
    }

    for (int n = 0; n <= 52; n++) {
        binom[n][0] = 1;
        for (int j = 1; j <= KMAX; j++)
            binom[n][j] = n ? binom[n - 1][j - 1] + binom[n - 1][j] : 0;
    }
    long long nsets = (long long)binom[52][kcards];
    if (last_set < 0)
        last_set = nsets - 1;
    if (first < 0 || last_set >= nsets || first > last_set) {
        fprintf(stderr, "verify_holdem: bad -p range (0..%lld)\n",
                nsets - 1);
        return 2;
    }
    first_set = first;
    nsets_run = last_set - first + 1;
    print_every = nsets_run / 100 ? nsets_run / 100 : 1;

    printf("verify_holdem: holdem%d %s, %lld hands, %d threads\n",
           kcards, kcards == 5 ? "stamps (compare with omporacle5)" :
                   kcards == 6 ? "vs holdem5" : "vs holdem6",
           nsets_run, nthreads);

    double t0 = now();
    pthread_t *tid = (pthread_t *)malloc(nthreads * sizeof *tid);
    if (!tid) {
        fprintf(stderr, "verify_holdem: out of memory\n");
        return 2;
    }
    for (int t = 0; t < nthreads; t++)
        pthread_create(&tid[t], NULL, worker, NULL);
    for (int t = 0; t < nthreads; t++)
        pthread_join(tid[t], NULL);
    double dt = now() - t0;
    fprintf(stderr, "\n");

    long long bad = bad_total.load();
    if (domain_only) {
        printf("%lld hands in %.1f s (%.1f M/s), domain-only\n",
               nsets_run, dt, nsets_run / dt / 1e6);
        printf("domain %016llx (k %d, hands %lld:%lld)\n",
               (unsigned long long)domain_total.load(), kcards,
               first, last_set);
        return 0;
    }
    if (kcards == 5)
        printf("%lld hands in %.1f s (%.1f M/s), stamps only -- "
               "validate by comparing with omporacle5\n",
               nsets_run, dt, nsets_run / dt / 1e6);
    else
        printf("%lld hands in %.1f s (%.1f M/s): %lld mismatches%s\n",
               nsets_run, dt, nsets_run / dt / 1e6, bad,
               bad ? "" : " -- PASS");
    printf("stamp %016llx domain %016llx (k %d, hands %lld:%lld)\n",
           (unsigned long long)stamp_total.load(),
           (unsigned long long)domain_total.load(), kcards,
           first, last_set);
    return bad ? 1 : 0;
}
