// Publication benchmark harness, holdem.
//
// One workload shape, two hand orders.  A pool of 2^20 seven-card hands is
// generated up front (off the clock), and one benchmark iteration streams
// the whole pool through the evaluator: hands in, one value per hand out,
// every value folded into an accumulator.  The two pools:
//
//   Random       uniformly sampled hands (fixed-seed LCG, so every machine
//                runs the identical workload)
//   Sequential   consecutive 7-card combinations in enumeration order
//                (colex, via Gosper's hack, from the first combination)
//
// Every evaluator starts from the same input: a list of seven card ids
// (0..51, ascending) per hand.  Whatever an evaluator needs beyond that --
// a presence mask, an incremental Hand object, a shifted deck -- it builds
// on the clock, hand by hand.  Nothing is precomputed into any evaluator's
// native form; the card list is the neutral form a caller who just drew a
// hand would actually hold.
//
//   CircuitEval  cards to presence masks via the library's converter
//                (bs_masks_from_cards, vectorized per target), then the
//                mask API (circuit_eval_holdem) -- mask build +
//                pack + unpack all on the clock, batches of BS_BATCH
//                (bsword.h: 512 on AVX-512, 128 on NEON, else 256)
//   TwoPlusTwo   the 7-chained-lookup into the 124 MB HandRanks
//                table (built once: make -C bench ../artifacts/HandRanks.dat)
//   OMPEval      Hand built card by card, HandEvaluator::evaluate
//   PHE          evaluate_7cards(c0..c6)
//
// The pools live in RAM (7 MB of card lists each), so a table-based
// evaluator sees its real access pattern: the random row defeats a
// cache-warm table, the sequential row hands it all the locality it can
// use.  Ours should not care -- that agreement is the point of running
// both.  No cross-hand state anywhere: sequential hands arrive in order
// but each is evaluated from scratch (the exhaustive best-implementation
// walks live in holdem_enum.cc).
//
// items_per_second is hands/s; the ns/hand counter is its inverse.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bench_perf.h"
#include "benchmark/benchmark.h"
#include "circuit_eval.h"
#include "omp/HandEvaluator.h"
#include "phevaluator/phevaluator.h"

typedef uint64_t u64;

static const long NHANDS = 1 << 20;  // hands per pool

static void per_hand(benchmark::State &state, int64_t hands_per_iter) {
  state.SetItemsProcessed(state.iterations() * hands_per_iter);
  state.counters["ns/hand"] = benchmark::Counter(
      (double)state.iterations() * (double)hands_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, hands_per_iter, "hand");
}

// ---- the two pools -------------------------------------------------------
// Card ids are 0..51, 4*rank + suit -- the same deck OMPEval and PHE use
// natively (2+2 is the same deck shifted to 1..52), and the bit positions
// of CircuitEval's presence masks.  Hands are generated as 52-bit masks
// and unpacked to ascending card lists; the masks are not kept.

static void mask_to_cards(u64 m, uint8_t *out) {
  for (int j = 0; j < 7; j++) {
    out[j] = (uint8_t)__builtin_ctzll(m);
    m &= m - 1;
  }
}

// Deterministic 7-card presence masks (partial Fisher-Yates over an LCG,
// same generator as verify/pack/bench_pack.c).
static u64 rng_state;
static u64 sample7() {
  int cards[52];
  u64 m = 0;
  for (int i = 0; i < 52; i++) cards[i] = i;
  for (int i = 0; i < 7; i++) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    int j = i + (int)((rng_state >> 33) % (52 - i));
    int t = cards[i]; cards[i] = cards[j]; cards[j] = t;
    m |= 1ull << cards[i];
  }
  return m;
}

static uint8_t (*random_pool())[7] {
  static uint8_t (*pool)[7];
  if (!pool) {
    pool = new uint8_t[NHANDS][7];
    rng_state = 0x9e3779b97f4a7c15ull;
    for (long i = 0; i < NHANDS; i++) mask_to_cards(sample7(), pool[i]);
  }
  return pool;
}

static uint8_t (*sequential_pool())[7] {
  static uint8_t (*pool)[7];
  if (!pool) {
    pool = new uint8_t[NHANDS][7];
    u64 x = (1ull << 7) - 1; // lowest 7-card combination
    for (long i = 0; i < NHANDS; i++) {
      mask_to_cards(x, pool[i]);
      // Gosper's hack, division replaced by a shift
      u64 u = x & -x;
      u64 v = x + u;
      x = v | ((x ^ v) >> (__builtin_ctzll(x) + 2));
    }
  }
  return pool;
}

// ---- ours ----------------------------------------------------------------

static void run_circuit_eval(benchmark::State &state, const uint8_t (*cards)[7]) {
  const int nbatch = NHANDS / BS_BATCH;
  static u64 masks[BS_BATCH];
  static uint32_t vals[BS_BATCH];
  for (auto _ : state) {
    u64 acc = 0;
    for (int b = 0; b < nbatch; b++) {
      bs_masks_from_cards(&cards[(long)b * BS_BATCH][0], 7, 7, masks);
      circuit_eval_holdem(masks, vals);
      for (int l = 0; l < BS_BATCH; l++) acc += vals[l];
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void CircuitEvalRandom(benchmark::State &state) {
  run_circuit_eval(state, random_pool());
}
BENCHMARK(CircuitEvalRandom);

static void CircuitEvalSequential(benchmark::State &state) {
  run_circuit_eval(state, sequential_pool());
}
BENCHMARK(CircuitEvalSequential);

// ---- TwoPlusTwo ----------------------------------------------------------

static const int TABLE_INTS = 32487834;

static const int *hr_table() {
  static int *HR;
  static bool tried;
  if (!tried) {
    tried = true;
    // run target executes from bench/; the root Makefile's cwd-proof too
    const char *paths[] = {"../artifacts/HandRanks.dat",
                           "artifacts/HandRanks.dat"};
    for (const char *p : paths) {
      FILE *f = fopen(p, "rb");
      if (!f) continue;
      HR = new int[TABLE_INTS];
      if (fread(HR, 4, TABLE_INTS, f) != (size_t)TABLE_INTS) {
        delete[] HR;
        HR = nullptr;
      }
      fclose(f);
      if (HR) break;
    }
  }
  return HR;
}

static void run_2p2(benchmark::State &state, const uint8_t (*cards)[7]) {
  const int *HR = hr_table();
  if (!HR) {
    state.SkipWithError(
        "HandRanks.dat missing: run `make -C bench ../artifacts/HandRanks.dat`");
    return;
  }
  for (auto _ : state) {
    u64 acc = 0;
    for (long i = 0; i < NHANDS; i++) {
      const uint8_t *h = cards[i];
      // their deck is ours shifted to 1..52, hence the +1s
      int p = HR[53 + h[0] + 1];
      p = HR[p + h[1] + 1];
      p = HR[p + h[2] + 1];
      p = HR[p + h[3] + 1];
      p = HR[p + h[4] + 1];
      p = HR[p + h[5] + 1];
      p = HR[p + h[6] + 1];
      acc += (u64)p;
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void TwoPlusTwoRandom(benchmark::State &state) {
  run_2p2(state, random_pool());
}
BENCHMARK(TwoPlusTwoRandom);

static void TwoPlusTwoSequential(benchmark::State &state) {
  run_2p2(state, sequential_pool());
}
BENCHMARK(TwoPlusTwoSequential);

// ---- OMPEval -------------------------------------------------------------

static void run_omp(benchmark::State &state, const uint8_t (*cards)[7]) {
  static omp::HandEvaluator ev;
  for (auto _ : state) {
    u64 acc = 0;
    for (long i = 0; i < NHANDS; i++) {
      const uint8_t *h = cards[i];
      omp::Hand hd = omp::Hand::empty();
      for (int j = 0; j < 7; j++) hd += omp::Hand(h[j]);
      acc += ev.evaluate(hd);
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void OMPEvalRandom(benchmark::State &state) {
  run_omp(state, random_pool());
}
BENCHMARK(OMPEvalRandom);

static void OMPEvalSequential(benchmark::State &state) {
  run_omp(state, sequential_pool());
}
BENCHMARK(OMPEvalSequential);

// ---- PHE -----------------------------------------------------------------

static void run_phe(benchmark::State &state, const uint8_t (*cards)[7]) {
  for (auto _ : state) {
    u64 acc = 0;
    for (long i = 0; i < NHANDS; i++) {
      const uint8_t *h = cards[i];
      acc += (u64)evaluate_7cards(h[0], h[1], h[2], h[3], h[4], h[5], h[6]);
    }
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}

static void PHERandom(benchmark::State &state) {
  run_phe(state, random_pool());
}
BENCHMARK(PHERandom);

static void PHESequential(benchmark::State &state) {
  run_phe(state, sequential_pool());
}
BENCHMARK(PHESequential);

int main(int argc, char **argv) {
  if (circuit_eval_holdem_raw_nb != 1) return 1; // multi-batch not served here
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
