/* Standalone computation of the omaha domain constants, one per hole
 * count.
 *
 * verify_omaha.c defines, for each hole size k, a 64-bit domain stamp:
 *
 *   domain = sum over every (hole, board) configuration of
 *            fmix64(fmix64(hole_mask) ^ board_mask)  mod 2^64
 *
 * where hole ranges over the C(52,k) hole sets and board over the
 * C(52-k,5) boards from the remaining cards.  The stamp is a constant
 * of the domain at k, independent of any evaluator, and it is the
 * gate for recombining partial validation runs: slice value stamps are
 * accepted only after the slice domain stamps sum to this constant.
 * That makes the constant worth publishing even for hole counts whose
 * full validation has not been run.
 *
 * verify_omaha -d computes the same sum, but through the validator's
 * general scaffolding.  This tool computes only the sum, so the whole
 * job is one fmix64 per configuration, and the inner loop is arranged
 * for hash throughput: board masks are built as an xor chain down the
 * a < b < c < d < e enumeration (the five cards are disjoint bits, so
 * or and xor agree), and the innermost e loop is vectorized where the
 * machine allows.  fmix64 is two 64-bit multiplies plus shifts and
 * xors; AVX-512DQ has a native lane-wise 64-bit multiply (vpmullq),
 * AVX2 synthesizes one from three 32-bit multiplies, and the scalar
 * fallback relies on the 1-per-cycle integer multiplier.  Partial
 * vector tails are kept out of the sum by masking, and the per-thread
 * accumulator stays in vector registers for a whole hole set.
 *
 * Configuration counts per hole count (hole sets x boards):
 *
 *   k    configurations    k    configurations
 *   2    2.8e9             6    2.8e13
 *   3    4.2e10            7    1.6e14
 *   4    4.6e11            8    8.2e14
 *   5    4.0e12
 *
 * Hole sets are indexed 0..C(52,k)-1 in colex order, identical to
 * verify_omaha, so -p slices computed here recombine with (and check
 * against) slices from the validator: disjoint slice domains sum to
 * the full-domain constant.
 *
 * usage: domain_omaha -k holes [-t threads] [-p first[:last]]
 *   -k   hole cards, 2..8
 *   -t   worker threads (default: online CPUs)
 *   -p   hole-set index range, for partial runs (default: whole domain)
 *
 * Output ends with the same line the validator prints:
 *
 *   domain <16 hex digits> (k <k>, sets <first>:<last>)
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__AVX512DQ__) || defined(__AVX2__)
#include <immintrin.h>
#endif

#define KMAX 8
#define RMPAD 8                 /* zero padding: safe tail overreads */

static uint64_t binom[53][KMAX + 1];
static int khole;
static long long nboards;

static long long last_set;
static _Atomic long long next_set;
static _Atomic long long sets_done;
static _Atomic unsigned long long domain_total;
static long long nsets_run;
static long long print_every;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
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

/* One backend per ISA level, picked at compile time.  Each provides:
 *   acc_t            per-thread accumulator, vector-resident
 *   acc_zero         clear it
 *   run_sum          add fmix64(x4 ^ rme[i]) for i in 0..len-1;
 *                    rme may be read past len up to RMPAD entries
 *   acc_fold         collapse to a single uint64_t
 * All sums are mod 2^64, so lane adds wrap harmlessly.
 */
#if defined(__AVX512DQ__)

#define BACKEND "avx512"
typedef struct { __m512i v0, v1; } acc_t;

static inline void acc_zero(acc_t *a)
{
    a->v0 = _mm512_setzero_si512();
    a->v1 = _mm512_setzero_si512();
}

static inline __m512i fmixv(__m512i x)
{
    x = _mm512_xor_si512(x, _mm512_srli_epi64(x, 33));
    x = _mm512_mullo_epi64(x, _mm512_set1_epi64(0xff51afd7ed558ccdull));
    x = _mm512_xor_si512(x, _mm512_srli_epi64(x, 33));
    x = _mm512_mullo_epi64(x, _mm512_set1_epi64(0xc4ceb9fe1a85ec53ull));
    x = _mm512_xor_si512(x, _mm512_srli_epi64(x, 33));
    return x;
}

static inline void run_sum(acc_t *a, uint64_t x4, const uint64_t *rme,
                           int len)
{
    __m512i vb = _mm512_set1_epi64((long long)x4);
    int e = 0;
    for (; e + 8 <= len; e += 8) {
        __m512i x = _mm512_xor_si512(vb,
            _mm512_loadu_si512((const void *)(rme + e)));
        a->v0 = _mm512_add_epi64(a->v0, fmixv(x));
    }
    int rem = len - e;
    if (rem) {
        __m512i x = _mm512_xor_si512(vb,
            _mm512_loadu_si512((const void *)(rme + e)));
        a->v1 = _mm512_mask_add_epi64(a->v1, (__mmask8)((1u << rem) - 1),
                                      a->v1, fmixv(x));
    }
}

static inline uint64_t acc_fold(const acc_t *a)
{
    return _mm512_reduce_add_epi64(_mm512_add_epi64(a->v0, a->v1));
}

#elif defined(__AVX2__)

#define BACKEND "avx2"
typedef struct { __m256i v0, v1; } acc_t;

/* keep the fmix64 output of the padding lanes out of the sum */
static const uint64_t masktab[4][4] __attribute__((aligned(32))) = {
    { 0, 0, 0, 0 },
    { ~0ull, 0, 0, 0 },
    { ~0ull, ~0ull, 0, 0 },
    { ~0ull, ~0ull, ~0ull, 0 },
};

static inline void acc_zero(acc_t *a)
{
    a->v0 = _mm256_setzero_si256();
    a->v1 = _mm256_setzero_si256();
}

/* 64x64 multiply by a constant, low half.  AVX2 has only the 32x32
 * unsigned multiply, so build it from three of those; the high-dword
 * extraction and the final 32-bit shift run as shuffles, which use a
 * different execution port than the multiplies and 33-bit shifts.
 */
static inline __m256i mul64c(__m256i x, __m256i cl, __m256i ch)
{
    __m256i lo = _mm256_mul_epu32(x, cl);
    __m256i m1 = _mm256_mul_epu32(_mm256_shuffle_epi32(x, 0xB1), cl);
    __m256i m2 = _mm256_mul_epu32(x, ch);
    __m256i hi = _mm256_shuffle_epi32(_mm256_add_epi64(m1, m2), 0xB1);
    hi = _mm256_and_si256(hi, _mm256_set1_epi64x(0xFFFFFFFF00000000ull));
    return _mm256_add_epi64(lo, hi);
}

static inline __m256i fmixv(__m256i x)
{
    const __m256i c1l = _mm256_set1_epi64x(0xed558ccd),
                  c1h = _mm256_set1_epi64x(0xff51afd7),
                  c2l = _mm256_set1_epi64x(0x1a85ec53),
                  c2h = _mm256_set1_epi64x(0xc4ceb9fe);
    x = _mm256_xor_si256(x, _mm256_srli_epi64(x, 33));
    x = mul64c(x, c1l, c1h);
    x = _mm256_xor_si256(x, _mm256_srli_epi64(x, 33));
    x = mul64c(x, c2l, c2h);
    x = _mm256_xor_si256(x, _mm256_srli_epi64(x, 33));
    return x;
}

static inline void run_sum(acc_t *a, uint64_t x4, const uint64_t *rme,
                           int len)
{
    __m256i vb = _mm256_set1_epi64x((long long)x4);
    int e = 0;
    for (; e + 4 <= len; e += 4) {
        __m256i x = _mm256_xor_si256(vb,
            _mm256_loadu_si256((const __m256i *)(rme + e)));
        a->v0 = _mm256_add_epi64(a->v0, fmixv(x));
    }
    int rem = len - e;
    if (rem) {
        __m256i x = _mm256_xor_si256(vb,
            _mm256_loadu_si256((const __m256i *)(rme + e)));
        __m256i f = _mm256_and_si256(fmixv(x),
            _mm256_load_si256((const __m256i *)masktab[rem]));
        a->v1 = _mm256_add_epi64(a->v1, f);
    }
}

static inline uint64_t acc_fold(const acc_t *a)
{
    uint64_t t[4];
    _mm256_storeu_si256((__m256i *)t,
                        _mm256_add_epi64(a->v0, a->v1));
    return t[0] + t[1] + t[2] + t[3];
}

#else

#define BACKEND "scalar"
typedef struct { uint64_t s0, s1; } acc_t;

static inline void acc_zero(acc_t *a)
{
    a->s0 = a->s1 = 0;
}

static inline void run_sum(acc_t *a, uint64_t x4, const uint64_t *rme,
                           int len)
{
    uint64_t s0 = 0, s1 = 0;
    int e = 0;
    for (; e + 2 <= len; e += 2) {
        s0 += fmix64(x4 ^ rme[e]);
        s1 += fmix64(x4 ^ rme[e + 1]);
    }
    if (e < len)
        s0 += fmix64(x4 ^ rme[e]);
    a->s0 += s0;
    a->s1 += s1;
}

static inline uint64_t acc_fold(const acc_t *a)
{
    return a->s0 + a->s1;
}

#endif

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
        uint64_t rm[52 + RMPAD];
        for (int c = 0; c < 52; c++)
            if (!(hm >> c & 1))
                rm[nr++] = 1ull << c;
        for (int i = nr; i < nr + RMPAD; i++)
            rm[i] = 0;

        acc_t acc;
        acc_zero(&acc);
        for (int a = 0; a < nr - 4; a++) {
            uint64_t x1 = hh ^ rm[a];
            for (int b = a + 1; b < nr - 3; b++) {
                uint64_t x2 = x1 ^ rm[b];
                for (int c = b + 1; c < nr - 2; c++) {
                    uint64_t x3 = x2 ^ rm[c];
                    for (int d = c + 1; d < nr - 1; d++)
                        run_sum(&acc, x3 ^ rm[d], rm + d + 1,
                                nr - d - 1);
                }
            }
        }
        atomic_fetch_add(&domain_total, acc_fold(&acc));

        long long done = atomic_fetch_add(&sets_done, 1) + 1;
        if (done % print_every == 0 || done == nsets_run)
            fprintf(stderr, "\r  %lld/%lld hole sets", done, nsets_run);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    long long first = 0;
    last_set = -1;
    khole = 0;

    int opt;
    while ((opt = getopt(argc, argv, "k:t:p:")) != -1) {
        switch (opt) {
        case 'k':
            khole = atoi(optarg);
            break;
        case 't':
            nthreads = atoi(optarg);
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
                "usage: domain_omaha -k holes(2..%d) [-t threads] "
                "[-p first[:last]]\n", KMAX);
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
        fprintf(stderr, "domain_omaha: bad -p range (0..%lld)\n",
                nsets - 1);
        return 2;
    }
    next_set = first;
    nsets_run = last_set - first + 1;
    print_every = nsets_run / 500 ? nsets_run / 500 : 1;
    printf("domain_omaha: omaha%d domain constant, "
           "%lld hole sets x %lld boards = %.4g configs, "
           "%d threads (%s)\n",
           khole, nsets_run, nboards,
           (double)nsets_run * nboards, nthreads, BACKEND);

    double t0 = now();
    pthread_t *tid = malloc(nthreads * sizeof *tid);
    if (!tid) {
        fprintf(stderr, "domain_omaha: out of memory\n");
        return 2;
    }
    for (int t = 0; t < nthreads; t++)
        pthread_create(&tid[t], NULL, worker, NULL);
    for (int t = 0; t < nthreads; t++)
        pthread_join(tid[t], NULL);
    double dt = now() - t0;
    fprintf(stderr, "\n");

    double nconf = (double)nsets_run * nboards;
    printf("%.0f configs in %.1f s (%.1f M/s), domain-only\n",
           nconf, dt, nconf / dt / 1e6);
    printf("domain %016llx (k %d, sets %lld:%lld)\n",
           (unsigned long long)domain_total, khole, first, last_set);
    return 0;
}
