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
 * shape as verify_omaha's triple table.  -b sweeps 3- or 4-card boards
 * instead, the expected low the min over the C(b,3) board triples.
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
 * usage: verify_omaha_lo -k holes [-b board] [-t threads] [-p first[:last]] [-d]
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

static _Atomic long long bad_total;
static _Atomic unsigned long long stamp_total;
static _Atomic unsigned long long domain_total;

/* Mismatch printing is capped; the count is exact regardless. */
#define BAD_PRINT_MAX 20
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static int bad_printed;

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

/* rules reference, pair min folded in up front:
 * btab[t] = best low over all hole pairs for triple t */
static void build_btab(unsigned char *btab, const int *hc,
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

/* 5-card boards: every 5-subset of the remaining cards, expected low
 * the min over its 10 triples of btab, the pair terms hoisted */
static long long sweep5(lanes_t *L, const unsigned char *btab,
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
                        const unsigned char *te = btab + c3[e];
                        unsigned mn = md;
                        v = te[pab]; if (v < mn) mn = v;
                        v = te[pac]; if (v < mn) mn = v;
                        v = te[pad]; if (v < mn) mn = v;
                        v = te[pbc]; if (v < mn) mn = v;
                        v = te[pbd]; if (v < mn) mn = v;
                        v = te[pcd]; if (v < mn) mn = v;
                        L->board[bi] = m4 | rm[e];
                        L->want[bi] = (unsigned char)mn;
                        if (++bi == BATCH) {
                            bad += flush_batch(L, BATCH, hm, hh, ssum, dsum);
                            bi = 0;
                        }
                    }
                }
            }
        }
    if (bi) {
        for (int m = bi; m < BATCH; m++)
            L->board[m] = L->board[bi - 1];
        bad += flush_batch(L, bi, hm, hh, ssum, dsum);
    }
    return bad;
}

/* 3- and 4-card boards: every b-subset of the remaining cards, expected
 * low the min over its C(b,3) triples of btab */
static long long sweep_short(lanes_t *L, const unsigned char *btab,
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
                        bad += flush_batch(L, BATCH, hm, hh, ssum, dsum);
                        bi = 0;
                    }
                    continue;
                }
                int pac = c2[c] + a, pbc = c2[c] + b;
                for (int dd = c + 1; dd < nr; dd++) {
                    const unsigned char *td = btab + c3[dd];
                    unsigned mn = btab[c3[c] + pab], v;
                    v = td[pab]; if (v < mn) mn = v;
                    v = td[pac]; if (v < mn) mn = v;
                    v = td[pbc]; if (v < mn) mn = v;
                    L->board[bi] = m3 | rm[dd];
                    L->want[bi] = (unsigned char)mn;
                    if (++bi == BATCH) {
                        bad += flush_batch(L, BATCH, hm, hh, ssum, dsum);
                        bi = 0;
                    }
                }
            }
        }
    if (bi) {
        for (int m = bi; m < BATCH; m++)
            L->board[m] = L->board[bi - 1];
        bad += flush_batch(L, bi, hm, hh, ssum, dsum);
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
    unsigned char *btab = malloc(NTRIPLES);
    if (!L || !btab) {
        fprintf(stderr, "verify_omaha_lo: out of memory\n");
        exit(1);
    }

    for (long long si; (si = take_set()) >= 0;) {
        int hc[KMAX];
        uint64_t hm, rm[52 + RMPAD];
        unsigned lb[52];
        int nr = hole_set(si, hc, &hm, rm, lb);
        uint64_t hh = fmix64(hm);

        long long bad = 0;
        uint64_t ssum = 0, dsum = 0;
        if (domain_only) {
            dsum = domain_sum(rm, nr, hh);
        } else {
            build_btab(btab, hc, lb, nr);
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
    int rc = parse_args(argc, argv, "verify_omaha_lo", 1);
    if (rc)
        return rc;
    printf("verify_omaha_lo: omaha%d low vs rules, %lld hole sets x "
           "%lld %d-card boards = %.4g configs, %d threads\n",
           khole, nsets_run, nboards, nboard,
           (double)nsets_run * nboards, nthreads);

    double dt = run_workers(worker, "verify_omaha_lo");

    double nconf = (double)nsets_run * nboards;
    if (domain_only) {
        printf("%.0f configs in %.1f s (%.1f M/s), domain-only\n",
               nconf, dt, nconf / dt / 1e6);
        printf("domain %016llx (k %d b %d low, sets %lld:%lld)\n",
               (unsigned long long)domain_total, khole, nboard,
               first_set, last_set);
        return 0;
    }
    long long bad = bad_total;
    printf("%.0f configs in %.1f s (%.1f M/s): %lld mismatches%s\n",
           nconf, dt, nconf / dt / 1e6, bad, bad ? "" : " -- PASS");
    printf("stamp %016llx domain %016llx (k %d b %d low, sets %lld:%lld)\n",
           (unsigned long long)stamp_total,
           (unsigned long long)domain_total, khole, nboard,
           first_set, last_set);
    return bad ? 1 : 0;
}
