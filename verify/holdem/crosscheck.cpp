// Ordinal agreement check: our reference tables vs TwoPlusTwo vs OMPEval vs
// PokerHandEvaluator. For random hand pairs, all must agree on win/lose/tie.
// usage: crosscheck <HandRanks.dat> [npairs]   (run from repo root)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "omp/HandEvaluator.h"
#include "phevaluator/phevaluator.h"

#define TABLE_INTS 32487834

static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static unsigned rnd()
{
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rng >> 33);
}

// our reference: rank_multiset.bin + flush_mask.bin (class id = strength order)
static long nrk;
static unsigned long long *rkey;
static unsigned short *rcls;
static int nfl;
static unsigned short *fmask, *fcls;

static void load_ref()
{
    FILE *f = fopen("artifacts/rank_multiset.bin", "rb");
    unsigned n;
    fread(&n, 4, 1, f);
    nrk = n;
    rkey = (unsigned long long *)malloc(n * 8);
    rcls = (unsigned short *)malloc(n * 2);
    for (unsigned i = 0; i < n; i++) {
        fread(&rkey[i], 8, 1, f);
        fread(&rcls[i], 2, 1, f);
    }
    fclose(f);
    f = fopen("artifacts/flush_mask.bin", "rb");
    fread(&n, 4, 1, f);
    nfl = n;
    fmask = (unsigned short *)malloc(n * 2);
    fcls = (unsigned short *)malloc(n * 2);
    for (unsigned i = 0; i < n; i++) {
        fread(&fmask[i], 2, 1, f);
        fread(&fcls[i], 2, 1, f);
    }
    fclose(f);
}

static int ref_class(const unsigned char *h)  // cards 0..51, 4*rank+suit
{
    int sc[4] = {0}, rc[13] = {0};
    unsigned sm[4] = {0};
    for (int i = 0; i < 7; i++) {
        int r = h[i] >> 2, s = h[i] & 3;
        sc[s]++;
        rc[r]++;
        sm[s] |= 1u << r;
    }
    for (int s = 0; s < 4; s++)
        if (sc[s] >= 5) {
            for (int i = 0; i < nfl; i++)
                if (fmask[i] == sm[s]) return fcls[i];
            return -1;
        }
    unsigned long long key = 0;
    for (int r = 0; r < 13; r++) key |= (unsigned long long)rc[r] << (3 * r);
    long lo = 0, hi = nrk - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (rkey[mid] < key) lo = mid + 1;
        else if (rkey[mid] > key) hi = mid - 1;
        else return rcls[mid];
    }
    return -1;
}

static int sgn(long d) { return (d > 0) - (d < 0); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s HandRanks.dat [npairs]\n", argv[0]); return 1; }
    long n = argc > 2 ? atol(argv[2]) : 1000000;
    int *HR = (int *)malloc((size_t)TABLE_INTS * 4);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(HR, 4, TABLE_INTS, f) != (size_t)TABLE_INTS) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    fclose(f);
    load_ref();
    omp::HandEvaluator ev;

    long bad12 = 0, bad13 = 0, bad14 = 0;
    for (long i = 0; i < n; i++) {
        unsigned char h[2][7];
        int v_ref[2], v_2p2[2], v_omp[2], v_phe[2];
        for (int a = 0; a < 2; a++) {
            int k = 0;
            while (k < 7) {
                int c = rnd() % 52;
                int dup = 0;
                for (int j = 0; j < k; j++) dup |= (h[a][j] == c);
                if (!dup) h[a][k++] = (unsigned char)c;
            }
            v_ref[a] = ref_class(h[a]);
            int p = HR[53 + h[a][0] + 1];
            for (int j = 1; j < 7; j++) p = HR[p + h[a][j] + 1];
            v_2p2[a] = p;
            omp::Hand hd = omp::Hand::empty();
            for (int j = 0; j < 7; j++) hd += omp::Hand(h[a][j]);
            v_omp[a] = ev.evaluate(hd);
            v_phe[a] = evaluate_7cards(h[a][0], h[a][1], h[a][2], h[a][3],
                                       h[a][4], h[a][5], h[a][6]);
        }
        int s_ref = sgn((long)v_ref[0] - v_ref[1]);
        if (s_ref != sgn((long)v_2p2[0] - v_2p2[1])) bad12++;
        if (s_ref != sgn((long)v_omp[0] - v_omp[1])) bad13++;
        if (s_ref != -sgn((long)v_phe[0] - v_phe[1])) bad14++;  // phe: 1 = best
    }
    printf("%ld pairs: ref-vs-2p2 disagreements %ld, ref-vs-omp %ld, "
           "ref-vs-phe %ld\n", n, bad12, bad13, bad14);
    return (bad12 || bad13 || bad14) ? 1 : 0;
}
