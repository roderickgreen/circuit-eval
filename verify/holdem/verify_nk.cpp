// Validate the holdem circuit on 5- and 6-card hands.
//
// The circuit was designed and verified for 7-card inputs (exactly 7 bits set
// in the 52-bit card-presence vector). The conjecture is that it "just works"
// for fewer cards: set 5 or 6 bits and it returns the value of the best 5-card
// hand available among them. This checks that conjecture against PHE, which
// has native evaluate_5cards / evaluate_6cards oracles.
//
// Our output is an order-isomorphic 24-bit *value* (not a dense class id), so
// the right test is ordinal: over the hands examined, our value and PHE's rank
// must be related by a strictly decreasing bijection. Concretely, collecting
// (phe, ours) over every hand, we require
//   (a) consistency: equal phe => equal ours, and equal ours => equal phe
//       (neither evaluator splits or merges a class the other doesn't), and
//   (b) monotonicity: sorting distinct classes by phe ascending (1 = best)
//       makes ours strictly descending (bigger ours = stronger hand).
// A single hand that breaks either rule is a real disagreement. For nh=5 this
// runs over all C(52,5)=2,598,960 hands, so a clean pass is a proof of ordinal
// isomorphism with PHE's 5-card evaluator; nh=6 can go exhaustive (20,358,520)
// or sample; nh=7 reproduces the known result as a self-check of the harness.
//
// usage: verify_nk <nh> [maxhands]        nh in {5,6,7}
//   maxhands omitted or 0 -> exhaustive enumeration of C(52,nh)
//   maxhands > 0          -> that many random distinct-card hands
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
typedef unsigned long long u64;

static int NH, nouts;

static int phe_eval(const unsigned char *h)
{
    if (NH == 5) return evaluate_5cards(h[0], h[1], h[2], h[3], h[4]);
    if (NH == 6) return evaluate_6cards(h[0], h[1], h[2], h[3], h[4], h[5]);
    return evaluate_7cards(h[0], h[1], h[2], h[3], h[4], h[5], h[6]);
}

// ---- the ordinal-isomorphism accumulator --------------------------------
static std::map<int, unsigned> phe2ours;   // phe rank -> the one value we give
static std::map<unsigned, int> ours2phe;   // our value -> the one rank phe gives
static long split_bad, merge_bad, checked;

static void observe(int phe, unsigned ours)
{
    checked++;
    auto a = phe2ours.find(phe);
    if (a == phe2ours.end()) phe2ours[phe] = ours;
    else if (a->second != ours) split_bad++;      // phe ties, we split
    auto b = ours2phe.find(ours);
    if (b == ours2phe.end()) ours2phe[ours] = phe;
    else if (b->second != phe) merge_bad++;        // we tie, phe splits
}

// ---- batch a group of hands through the bitsliced circuit ---------------
static lanes in[52];       // nb=1: 52 card planes, 256 lanes
static lanes *out;
static unsigned char hbuf[256][7];
static int nlane;

static void flush_batch()
{
    if (!nlane) return;
    memset(in, 0, sizeof(in));
    for (int l = 0; l < nlane; l++)
        for (int j = 0; j < NH; j++)
            in[bs_card_input_holdem[hbuf[l][j]]].u[l / 64] |= 1ull << (l % 64);
    bs_eval_holdem(&in[0].v, &out[0].v);
    for (int l = 0; l < nlane; l++) {
        unsigned ours = 0;
        for (int k = 0; k < nouts; k++)
            ours |= (unsigned)(out[k].u[l / 64] >> (l % 64) & 1) << k;
        observe(phe_eval(hbuf[l]), ours);
    }
    nlane = 0;
}

static void submit(const unsigned char *h)
{
    memcpy(hbuf[nlane++], h, NH);
    if (nlane == 256) flush_batch();
}

// exhaustive: every strictly-increasing nh-tuple of cards 0..51
static void enumerate()
{
    unsigned char h[7];
    int idx[7];
    for (int i = 0; i < NH; i++) idx[i] = i;
    for (;;) {
        for (int i = 0; i < NH; i++) h[i] = idx[i];
        submit(h);
        int p = NH - 1;
        while (p >= 0 && idx[p] == 52 - NH + p) p--;
        if (p < 0) break;
        idx[p]++;
        for (int i = p + 1; i < NH; i++) idx[i] = idx[i - 1] + 1;
    }
    flush_batch();
}

static void sample(long m)
{
    u64 rng = 0x9e3779b97f4a7c15ull;
    unsigned char h[7];
    for (long t = 0; t < m; t++) {
        int k = 0;
        while (k < NH) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            int c = (int)((rng >> 33) % 52);
            int dup = 0;
            for (int j = 0; j < k; j++) dup |= (h[j] == c);
            if (!dup) h[k++] = (unsigned char)c;
        }
        submit(h);
    }
    flush_batch();
}

int main(int argc, char **argv)
{
    NH = argc > 1 ? atoi(argv[1]) : 5;
    long maxh = argc > 2 ? atol(argv[2]) : 0;
    if (NH < 5 || NH > 7) { fprintf(stderr, "nh must be 5, 6 or 7\n"); return 1; }
    nouts = bs_num_outputs_holdem;
    out = (lanes *)aligned_alloc(32, nouts * sizeof(lanes));

    if (maxh > 0) sample(maxh);
    else enumerate();

    // monotonicity: distinct classes, phe ascending -> ours strictly descending
    long mono_bad = 0;
    bool first = true;
    unsigned prev = 0;
    for (auto &kv : phe2ours) {           // std::map iterates by phe ascending
        if (!first && kv.second >= prev) mono_bad++;
        prev = kv.second;
        first = false;
    }

    long bad = split_bad + merge_bad + mono_bad;
    printf("nh=%d  hands=%ld  distinct classes: phe=%zu ours=%zu  "
           "split=%ld merge=%ld nonmonotone=%ld  =>  %s\n",
           NH, checked, phe2ours.size(), ours2phe.size(),
           split_bad, merge_bad, mono_bad, bad ? "FAIL" : "OK");
    return bad ? 1 : 0;
}
