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
 * -b sweeps 3- or 4-card boards instead (a flop or a turn): the same
 * triple table, and the expected value is the max over the C(b,3)
 * board triples, one lookup at b 3 and four at b 4.  Those domains are
 * C(52,k) x C(52-k,b) configurations, about 100x and 9x smaller than
 * the 5-card board's, but the triple table is built per hole set
 * regardless, so the wall time falls by less.
 *
 * Hole sets are indexed 0..C(52,k)-1 in colex order (unranked by the
 * combinatorial number system), so -p partitions a domain into resumable
 * slices for the larger k.
 *
 * usage: verify_omaha -k holes [-b board] [-t threads] [-p first[:last]] [-d]
 *   -k   hole cards to validate, 2..8 (each stands alone)
 *   -b   board cards, 3..5 (default 5; each stands alone)
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
#include "omaha_sweep.h"
#include "circuit_eval.h"

#define BATCH BS_BATCH          /* library batch size (bsapi.h) */

static _Atomic long long bad_total;
static _Atomic unsigned long long stamp_total;
static _Atomic unsigned long long domain_total;

/* Mismatch printing is capped; the count is exact regardless. */
#define BAD_PRINT_MAX 20
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static int bad_printed;

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
static void build_btab(lanes_t *L, uint32_t *btab, const int *hc,
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

/* 5-card boards: every 5-subset of the remaining cards, expected value
 * the max over its 10 triples of btab, the pair terms hoisted */
static long long sweep5(lanes_t *L, const uint32_t *btab,
                        const uint64_t *rm, int nr, uint64_t hm,
                        uint64_t hh, uint64_t *ssum, uint64_t *dsum)
{
    long long bad = 0;
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
                            bad += eval_batch(L, BATCH, hm, hh, ssum, dsum);
                            bi = 0;
                        }
                    }
                }
            }
        }
    if (bi) {
        for (int m = bi; m < BATCH; m++)
            L->board[m] = L->board[bi - 1];
        bad += eval_batch(L, bi, hm, hh, ssum, dsum);
    }
    return bad;
}

/* 3- and 4-card boards: every b-subset of the remaining cards, expected
 * value the max over its C(b,3) triples of btab */
static long long sweep_short(lanes_t *L, const uint32_t *btab,
                             const uint64_t *rm, int nr, uint64_t hm,
                             uint64_t hh, uint64_t *ssum, uint64_t *dsum)
{
    long long bad = 0;
    int bi = 0;
    for (int a = 0; a < nr - 2; a++)
        for (int b = a + 1; b < nr - 1; b++) {
            int pab = c2[b] + a;
            for (int c = b + 1; c < nr; c++) {
                uint64_t m3 = rm[a] | rm[b] | rm[c];
                if (nboard == 3) {
                    L->board[bi] = m3;
                    L->want[bi] = btab[c3[c] + pab];
                    if (++bi == BATCH) {
                        bad += eval_batch(L, BATCH, hm, hh, ssum, dsum);
                        bi = 0;
                    }
                    continue;
                }
                int pac = c2[c] + a, pbc = c2[c] + b;
                for (int dd = c + 1; dd < nr; dd++) {
                    const uint32_t *td = btab + c3[dd];
                    uint32_t mx = btab[c3[c] + pab], v;
                    v = td[pab]; if (v > mx) mx = v;
                    v = td[pac]; if (v > mx) mx = v;
                    v = td[pbc]; if (v > mx) mx = v;
                    L->board[bi] = m3 | rm[dd];
                    L->want[bi] = mx;
                    if (++bi == BATCH) {
                        bad += eval_batch(L, BATCH, hm, hh, ssum, dsum);
                        bi = 0;
                    }
                }
            }
        }
    if (bi) {
        for (int m = bi; m < BATCH; m++)
            L->board[m] = L->board[bi - 1];
        bad += eval_batch(L, bi, hm, hh, ssum, dsum);
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
    uint32_t *btab = malloc((NTRIPLES + BATCH) * sizeof *btab);
    if (!L || !btab) {
        fprintf(stderr, "verify_omaha: out of memory\n");
        exit(1);
    }

    for (long long si; (si = take_set()) >= 0;) {
        int hc[KMAX];
        uint64_t hm, rm[52 + RMPAD];
        int nr = hole_set(si, hc, &hm, rm, NULL);
        uint64_t hh = fmix64(hm);

        long long bad = 0;
        uint64_t ssum = 0, dsum = 0;
        if (domain_only) {
            dsum = domain_sum(rm, nr, hh);
        } else {
            build_btab(L, btab, hc, rm, nr);
            for (int m = 0; m < BATCH; m++)
                L->hole[m] = hm;
            bad = nboard == 5
                ? sweep5(L, btab, rm, nr, hm, hh, &ssum, &dsum)
                : sweep_short(L, btab, rm, nr, hm, hh, &ssum, &dsum);
        }

        atomic_fetch_add(&stamp_total, ssum);
        atomic_fetch_add(&domain_total, dsum);
        atomic_fetch_add(&bad_total, bad);
        finish_set();
    }
    free(btab);
    free(L);
    return NULL;
}

int main(int argc, char **argv)
{
    int rc = parse_args(argc, argv, "verify_omaha", 1);
    if (rc)
        return rc;
    printf("verify_omaha: omaha%d vs pair-triple max of holdem5, "
           "%lld hole sets x %lld %d-card boards = %.4g configs, "
           "%d threads\n",
           khole, nsets_run, nboards, nboard,
           (double)nsets_run * nboards, nthreads);

    double dt = run_workers(worker, "verify_omaha");

    double nconf = (double)nsets_run * nboards;
    if (domain_only) {
        printf("%.0f configs in %.1f s (%.1f M/s), domain-only\n",
               nconf, dt, nconf / dt / 1e6);
        printf("domain %016llx (k %d b %d, sets %lld:%lld)\n",
               (unsigned long long)domain_total, khole, nboard,
               first_set, last_set);
        return 0;
    }
    long long bad = bad_total;
    printf("%.0f configs in %.1f s (%.1f M/s): %lld mismatches%s\n",
           nconf, dt, nconf / dt / 1e6, bad, bad ? "" : " -- PASS");
    printf("stamp %016llx domain %016llx (k %d b %d, sets %lld:%lld)\n",
           (unsigned long long)stamp_total,
           (unsigned long long)domain_total, khole, nboard,
           first_set, last_set);
    return bad ? 1 : 0;
}
