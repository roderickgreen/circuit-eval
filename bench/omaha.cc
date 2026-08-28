// Publication benchmark harness, omaha (PLO4/5/6 high).
//
// Same methodology as holdem.cc: one workload shape, two hand orders.  A
// pool of 2^20 deals is generated up front (off the clock), and one
// benchmark iteration streams the whole pool: deals in, one value per deal
// out, every value folded into an accumulator.  A deal is k hole cards
// (k = 4, 5, 6) plus a 5-card board, disjoint.  The two pools:
//
//   Random       k+5 distinct cards drawn per deal (fixed-seed LCG); the
//                first k drawn are the hole cards, the last 5 the board --
//                the same distribution as dealing from a shuffled deck
//   Sequential   consecutive (k+5)-card combinations in enumeration order
//                (colex, via Gosper's hack); the lowest k cards of each
//                combination are the hole cards, the highest 5 the board.
//                That split keeps the board nearly constant from deal to
//                deal, which hands a board-major table evaluator the best
//                locality the order can offer.
//
// Every evaluator starts from the same input: a list of 5+k card ids
// (0..51) per deal, the 5 board cards first and then the k hole cards,
// each group ascending.  Whatever an evaluator needs beyond that it builds
// on the clock, deal by deal; nothing is precomputed into any evaluator's
// native form (the same rule as holdem.cc).
//
//   CircuitEval  cards to hole + board presence masks via the library's
//                strided converter (bs_masks_from_cards, vectorized per
//                target), then the mask API (circuit_eval_omaha) --
//                mask build + pack + unpack all on the clock, batches of
//                BS_BATCH (bsword.h: 512 on AVX-512, 128 on NEON, else 256); one
//                circuit serves all three k exactly
//   CircuitEval  circuit_eval_omaha_hilo: the high circuit and the low
//   HiLo         (8-or-better) circuit sharing one packed input -- one
//                transpose pair, two evals, two unpacks, the real hi-lo
//                call pattern; omaha-4 pools only, and no low-only row
//                (the high rows above are the baseline, so the hi-lo rows
//                price the low circuit as an increment)
//   PHE          evaluate_ploK_cards(c1..c5, h1..hK), the dedicated
//                plo4/5/6 evaluators (board first, then hole -- the pool's
//                own order), compiled from source with our flags (PHE has
//                no omaha hi-lo evaluator, so the hi-lo rows are ours only)
//
// No cross-hand state anywhere: sequential deals arrive in order but each
// is evaluated from scratch.  items_per_second is hands/s; the ns/hand
// counter is its inverse.
#include <cstdint>

#include "bench_perf.h"
#include "benchmark/benchmark.h"
#include "circuit_eval.h"
#include "phevaluator/phevaluator.h"

typedef uint64_t u64;

static const long NHANDS = 1 << 20;  // deals per pool

static void per_hand(benchmark::State &state, int64_t hands_per_iter) {
  state.SetItemsProcessed(state.iterations() * hands_per_iter);
  state.counters["ns/hand"] = benchmark::Counter(
      (double)state.iterations() * (double)hands_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, hands_per_iter, "hand");
}

// ---- the pools (one random + one sequential per k) -----------------------
// Card ids are 0..51, 4*rank + suit -- the same deck PHE uses natively, and
// the bit positions of CircuitEval's presence masks.  A pool is NHANDS
// deals of 5+k cards each: 5 board cards, then k hole cards, each group
// ascending (both of PHE's argument groups are order-insensitive).  Deals
// are generated as hole/board mask pairs and unpacked to card lists; the
// masks are not kept.

// same LCG as holdem.cc / verify/pack/bench_pack.c
static u64 rng_state;
static u64 rng_next() {
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return rng_state >> 33;
}

static void extract(u64 m, uint8_t *dst, int n) {
  for (int j = 0; j < n; j++) {
    dst[j] = (uint8_t)__builtin_ctzll(m);
    m &= m - 1;
  }
}

static const uint8_t *random_pool(int k) {
  static uint8_t *pools[3];
  uint8_t *&p = pools[k - 4];
  if (!p) {
    p = new uint8_t[NHANDS * (5 + k)];
    rng_state = 0x9e3779b97f4a7c15ull;
    for (long i = 0; i < NHANDS; i++) {
      int cards[52];
      for (int c = 0; c < 52; c++) cards[c] = c;
      u64 hole = 0, board = 0;
      for (int c = 0; c < k + 5; c++) { // partial Fisher-Yates
        int j = c + (int)(rng_next() % (52 - c));
        int t = cards[c]; cards[c] = cards[j]; cards[j] = t;
        (c < k ? hole : board) |= 1ull << cards[c];
      }
      uint8_t *d = p + i * (5 + k);
      extract(board, d, 5);
      extract(hole, d + 5, k);
    }
  }
  return p;
}

static const uint8_t *sequential_pool(int k) {
  static uint8_t *pools[3];
  uint8_t *&p = pools[k - 4];
  if (!p) {
    p = new uint8_t[NHANDS * (5 + k)];
    u64 x = (1ull << (k + 5)) - 1; // lowest (k+5)-card combination
    for (long i = 0; i < NHANDS; i++) {
      u64 board = 0; // the highest 5 set bits of x
      for (int j = 0; j < 5; j++)
        board |= 1ull << (63 - __builtin_clzll(x ^ board));
      uint8_t *d = p + i * (5 + k);
      extract(board, d, 5);
      extract(x ^ board, d + 5, k);
      // Gosper's hack, division replaced by a shift
      u64 u = x & -x;
      u64 v = x + u;
      x = v | ((x ^ v) >> (__builtin_ctzll(x) + 2));
    }
  }
  return p;
}

// ---- ours ----------------------------------------------------------------

// deal records to hole/board mask batches, via the library's strided
// converter (one call per slice of the 5+k record)
static void batch_masks(const uint8_t *d, int k, u64 *hole, u64 *board) {
  bs_masks_from_cards(d, 5, 5 + k, board);
  bs_masks_from_cards(d + 5, k, 5 + k, hole);
}

static void run_circuit_eval(benchmark::State &state, const uint8_t *cards,
                             int k) {
  const int nbatch = NHANDS / BS_BATCH;
  static u64 hole[BS_BATCH], board[BS_BATCH];
  static uint32_t vals[BS_BATCH];
  for (auto _ : state) {
    u64 acc = 0;
    for (int b = 0; b < nbatch; b++) {
      batch_masks(cards + (long)b * BS_BATCH * (5 + k), k, hole, board);
      circuit_eval_omaha(hole, board, vals);
      for (int l = 0; l < BS_BATCH; l++) acc += vals[l];
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void CircuitEvalOmaha4Random(benchmark::State &state) {
  run_circuit_eval(state, random_pool(4), 4);
}
BENCHMARK(CircuitEvalOmaha4Random);

static void CircuitEvalOmaha4Sequential(benchmark::State &state) {
  run_circuit_eval(state, sequential_pool(4), 4);
}
BENCHMARK(CircuitEvalOmaha4Sequential);

static void CircuitEvalOmaha5Random(benchmark::State &state) {
  run_circuit_eval(state, random_pool(5), 5);
}
BENCHMARK(CircuitEvalOmaha5Random);

static void CircuitEvalOmaha5Sequential(benchmark::State &state) {
  run_circuit_eval(state, sequential_pool(5), 5);
}
BENCHMARK(CircuitEvalOmaha5Sequential);

static void CircuitEvalOmaha6Random(benchmark::State &state) {
  run_circuit_eval(state, random_pool(6), 6);
}
BENCHMARK(CircuitEvalOmaha6Random);

static void CircuitEvalOmaha6Sequential(benchmark::State &state) {
  run_circuit_eval(state, sequential_pool(6), 6);
}
BENCHMARK(CircuitEvalOmaha6Sequential);

// ---- ours, hi-lo ---------------------------------------------------------

static void run_circuit_eval_hilo(benchmark::State &state,
                                  const uint8_t *cards) {
  const int k = 4; // hi-lo rows are omaha-4 only
  const int nbatch = NHANDS / BS_BATCH;
  static u64 hole[BS_BATCH], board[BS_BATCH];
  static uint32_t hv[BS_BATCH], lv[BS_BATCH];
  for (auto _ : state) {
    u64 acc = 0;
    for (int b = 0; b < nbatch; b++) {
      batch_masks(cards + (long)b * BS_BATCH * (5 + k), k, hole, board);
      circuit_eval_omaha_hilo(hole, board, hv, lv);
      for (int l = 0; l < BS_BATCH; l++) acc += hv[l] + lv[l];
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void CircuitEvalOmaha4HiLoRandom(benchmark::State &state) {
  run_circuit_eval_hilo(state, random_pool(4));
}
BENCHMARK(CircuitEvalOmaha4HiLoRandom);

static void CircuitEvalOmaha4HiLoSequential(benchmark::State &state) {
  run_circuit_eval_hilo(state, sequential_pool(4));
}
BENCHMARK(CircuitEvalOmaha4HiLoSequential);

// ---- PHE -----------------------------------------------------------------

static void run_phe4(benchmark::State &state, const uint8_t *cards) {
  for (auto _ : state) {
    u64 acc = 0;
    for (long i = 0; i < NHANDS; i++) {
      const uint8_t *d = cards + i * 9;
      acc += (u64)evaluate_plo4_cards(d[0], d[1], d[2], d[3], d[4],
                                      d[5], d[6], d[7], d[8]);
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void run_phe5(benchmark::State &state, const uint8_t *cards) {
  for (auto _ : state) {
    u64 acc = 0;
    for (long i = 0; i < NHANDS; i++) {
      const uint8_t *d = cards + i * 10;
      acc += (u64)evaluate_plo5_cards(d[0], d[1], d[2], d[3], d[4],
                                      d[5], d[6], d[7], d[8], d[9]);
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void run_phe6(benchmark::State &state, const uint8_t *cards) {
  for (auto _ : state) {
    u64 acc = 0;
    for (long i = 0; i < NHANDS; i++) {
      const uint8_t *d = cards + i * 11;
      acc += (u64)evaluate_plo6_cards(d[0], d[1], d[2], d[3], d[4],
                                      d[5], d[6], d[7], d[8], d[9], d[10]);
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void PHEOmaha4Random(benchmark::State &state) {
  run_phe4(state, random_pool(4));
}
BENCHMARK(PHEOmaha4Random);

static void PHEOmaha4Sequential(benchmark::State &state) {
  run_phe4(state, sequential_pool(4));
}
BENCHMARK(PHEOmaha4Sequential);

static void PHEOmaha5Random(benchmark::State &state) {
  run_phe5(state, random_pool(5));
}
BENCHMARK(PHEOmaha5Random);

static void PHEOmaha5Sequential(benchmark::State &state) {
  run_phe5(state, sequential_pool(5));
}
BENCHMARK(PHEOmaha5Sequential);

static void PHEOmaha6Random(benchmark::State &state) {
  run_phe6(state, random_pool(6));
}
BENCHMARK(PHEOmaha6Random);

static void PHEOmaha6Sequential(benchmark::State &state) {
  run_phe6(state, sequential_pool(6));
}
BENCHMARK(PHEOmaha6Sequential);

int main(int argc, char **argv) {
  if (circuit_eval_omaha_raw_nb != 1 || circuit_eval_omaha_low_raw_nb != 1)
    return 1; // multi-batch not served here
  if (circuit_eval_omaha_raw_num_outputs != 24 ||
      circuit_eval_omaha_low_raw_num_outputs != 8)
    return 1; // the 24-bit high / 8-bit low shapes documented in the header
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
