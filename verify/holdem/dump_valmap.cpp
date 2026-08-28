// Dump the value->rank table from the linked holdem circuit + PHE.
//
// Enumerates all C(52,5) hands through the holdem circuit, pairing our 24-bit
// order-isomorphic value with PHE's dense rank (1..7462, 1 = best -- the
// classic Cactus Kev scale).  5-card hands realize all 7462 classes, so the
// dump is the complete function. verify_holdem.sh diffs it against
// flow/encoding/mkspec.py -- that equality is the value-encoding contract.
//
// Output (stdout): one "<value-hex> <rank-decimal>" line per class, sorted by
// rank ascending. Sanity-enforced here: exactly 7462 rows, bijective, value
// strictly descending as rank ascends (the ordinal-isomorphism invariant).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "phevaluator/phevaluator.h"
extern "C" {
typedef unsigned long long word __attribute__((vector_size(32)));
extern void bs_eval_holdem(const word *in, word *out);
extern const int bs_card_input_holdem[52];
extern const int bs_num_outputs_holdem;
}
typedef union { word v; unsigned long long u[4]; } lanes;

static int nouts;
static lanes in[52];
static lanes *out;
static unsigned char hbuf[256][5];
static int nlane;
static std::map<int, unsigned> rank2val;
static long dup_bad;

static void flush_batch(void)
{
    if (!nlane) return;
    memset(in, 0, sizeof(in));
    for (int l = 0; l < nlane; l++)
        for (int j = 0; j < 5; j++)
            in[bs_card_input_holdem[hbuf[l][j]]].u[l / 64] |= 1ull << (l % 64);
    bs_eval_holdem(&in[0].v, &out[0].v);
    for (int l = 0; l < nlane; l++) {
        unsigned ours = 0;
        for (int k = 0; k < nouts; k++)
            ours |= (unsigned)(out[k].u[l / 64] >> (l % 64) & 1) << k;
        int phe = evaluate_5cards(hbuf[l][0], hbuf[l][1], hbuf[l][2],
                                  hbuf[l][3], hbuf[l][4]);
        auto it = rank2val.find(phe);
        if (it == rank2val.end()) rank2val[phe] = ours;
        else if (it->second != ours) dup_bad++;
    }
    nlane = 0;
}

int main(void)
{
    nouts = bs_num_outputs_holdem;
    out = (lanes *)aligned_alloc(32, nouts * sizeof(lanes));

    int idx[5];
    for (int i = 0; i < 5; i++) idx[i] = i;
    for (;;) {
        for (int i = 0; i < 5; i++) hbuf[nlane][i] = (unsigned char)idx[i];
        if (++nlane == 256) flush_batch();
        int p = 4;
        while (p >= 0 && idx[p] == 47 + p) p--;
        if (p < 0) break;
        idx[p]++;
        for (int i = p + 1; i < 5; i++) idx[i] = idx[i - 1] + 1;
    }
    flush_batch();

    long mono_bad = 0;
    unsigned prev = 0;
    bool first = true;
    for (auto &kv : rank2val) {
        if (!first && kv.second >= prev) mono_bad++;
        prev = kv.second;
        first = false;
    }
    if (rank2val.size() != 7462 || dup_bad || mono_bad) {
        fprintf(stderr, "BAD TABLE: classes=%zu dup=%ld nonmonotone=%ld\n",
                rank2val.size(), dup_bad, mono_bad);
        return 1;
    }
    for (auto &kv : rank2val)
        printf("%06x %d\n", kv.second, kv.first);
    fprintf(stderr, "7462 classes, bijective, monotone: OK\n");
    return 0;
}
