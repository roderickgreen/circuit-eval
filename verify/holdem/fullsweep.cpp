// Exhaustive C(52,7) validation of the ground-truth artifacts against
// PokerHandEvaluator as an external oracle. Checks:
//  - the table path (flush_mask.bin else rank_multiset.bin) resolves every
//    one of the 133,784,560 hands
//  - class id <-> PHE 7-card rank is a strict order-reversing bijection
//    over all hands (class ascending = stronger; PHE rank 1 = best)
//  - per-category hand counts match the known constants (categories from
//    PHE rank ranges on the 7462 scale)
// usage: fullsweep    (run from repo root; threaded, a few seconds)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "phevaluator/phevaluator.h"

#define NCLASS 4824

static long nrk;
static unsigned long long *rkey;
static unsigned short *rcls;
static unsigned short fclass[1 << 13];

static const char *cat_names[9] = {
    "HighCard", "OnePair", "TwoPair", "ThreeOfAKind", "Straight",
    "Flush", "FullHouse", "FourOfAKind", "StraightFlush",
};
// 7-card hand counts per best category (standard result).
static const long long expected_hands[9] = {
    23294460, 58627800, 31433400, 6461620, 6180020,
    4047644, 3473184, 224848, 41584,
};

static int load_ref(void)
{
    FILE *f = fopen("artifacts/rank_multiset.bin", "rb");
    if (!f) { fprintf(stderr, "cannot open artifacts/rank_multiset.bin\n"); return 1; }
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
    memset(fclass, 0xFF, sizeof fclass);
    f = fopen("artifacts/flush_mask.bin", "rb");
    if (!f) { fprintf(stderr, "cannot open artifacts/flush_mask.bin\n"); return 1; }
    fread(&n, 4, 1, f);
    for (unsigned i = 0; i < n; i++) {
        unsigned short m, c;
        fread(&m, 2, 1, f);
        fread(&c, 2, 1, f);
        fclass[m] = c;
    }
    fclose(f);
    return 0;
}

static int table_class(const int *h)
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
        if (sc[s] >= 5) return fclass[sm[s]] == 0xFFFF ? -1 : fclass[sm[s]];
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

// category index (our numbering) from a PHE rank on the 7462 scale
static int phe_cat(int p)
{
    if (p <= 10) return 8;
    if (p <= 166) return 7;
    if (p <= 322) return 6;
    if (p <= 1599) return 5;
    if (p <= 1609) return 4;
    if (p <= 2467) return 3;
    if (p <= 3325) return 2;
    if (p <= 6185) return 1;
    return 0;
}

struct Local {
    int cls2phe[NCLASS];
    long long cat[9];
    long long total, unresolved, conflicts;
    Local() : cat{}, total(0), unresolved(0), conflicts(0)
    {
        for (int i = 0; i < NCLASS; i++) cls2phe[i] = -1;
    }
};

int main(void)
{
    if (load_ref()) return 1;

    std::vector<std::pair<int, int>> pairs;
    for (int c1 = 0; c1 < 52; c1++)
        for (int c2 = c1 + 1; c2 < 52; c2++) pairs.push_back({c1, c2});

    unsigned nt = std::thread::hardware_concurrency();
    if (!nt) nt = 4;
    std::vector<Local> locals(nt);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < nt; t++) {
        threads.emplace_back([&, t]() {
            Local &L = locals[t];
            int h[7];
            for (size_t p = t; p < pairs.size(); p += nt) {
                h[0] = pairs[p].first;
                h[1] = pairs[p].second;
                for (h[2] = h[1] + 1; h[2] < 52; h[2]++)
                for (h[3] = h[2] + 1; h[3] < 52; h[3]++)
                for (h[4] = h[3] + 1; h[4] < 52; h[4]++)
                for (h[5] = h[4] + 1; h[5] < 52; h[5]++)
                for (h[6] = h[5] + 1; h[6] < 52; h[6]++) {
                    int cls = table_class(h);
                    int phe = evaluate_7cards(h[0], h[1], h[2], h[3],
                                              h[4], h[5], h[6]);
                    L.total++;
                    L.cat[phe_cat(phe)]++;
                    if (cls < 0) { L.unresolved++; continue; }
                    if (L.cls2phe[cls] == -1) L.cls2phe[cls] = phe;
                    else if (L.cls2phe[cls] != phe) L.conflicts++;
                }
            }
        });
    }
    for (auto &th : threads) th.join();

    // merge
    int cls2phe[NCLASS];
    for (int i = 0; i < NCLASS; i++) cls2phe[i] = -1;
    long long cat[9] = {0}, total = 0, unresolved = 0, conflicts = 0;
    for (auto &L : locals) {
        total += L.total;
        unresolved += L.unresolved;
        conflicts += L.conflicts;
        for (int i = 0; i < 9; i++) cat[i] += L.cat[i];
        for (int i = 0; i < NCLASS; i++) {
            if (L.cls2phe[i] == -1) continue;
            if (cls2phe[i] == -1) cls2phe[i] = L.cls2phe[i];
            else if (cls2phe[i] != L.cls2phe[i]) conflicts++;
        }
    }

    int ok = 1;
    printf("%lld hands swept\n", total);
    if (total != 133784560) { printf("MISMATCH: wrong total hand count\n"); ok = 0; }
    if (unresolved) { printf("MISMATCH: %lld hands unresolved by the tables\n", unresolved); ok = 0; }
    if (conflicts) { printf("MISMATCH: %lld class->PHE conflicts\n", conflicts); ok = 0; }

    int unseen = 0, notmono = 0;
    for (int i = 0; i < NCLASS; i++) {
        if (cls2phe[i] == -1) unseen++;
        else if (i && cls2phe[i - 1] != -1 && cls2phe[i - 1] <= cls2phe[i]) notmono++;
    }
    if (unseen) { printf("MISMATCH: %d classes never observed\n", unseen); ok = 0; }
    else printf("all %d classes observed\n", NCLASS);
    if (notmono) { printf("MISMATCH: class->PHE not strictly order-reversing at %d steps\n", notmono); ok = 0; }
    else printf("class id <-> PHE rank strictly order-reversing (exact bijection)\n");

    printf("\nhands by best category (PHE):\n");
    for (int c = 0; c < 9; c++) {
        const char *flag = cat[c] == expected_hands[c] ? "ok" : "MISMATCH";
        printf("  %-14s %12lld  %s\n", cat_names[c], cat[c], flag);
        if (cat[c] != expected_hands[c]) ok = 0;
    }

    printf("\n%s\n", ok ? "PASS: artifacts exact vs PHE over all C(52,7) hands"
                        : "FAIL");
    return ok ? 0 : 1;
}
