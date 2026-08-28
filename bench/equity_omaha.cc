// Publication benchmark harness, omaha heads-up equity (PLO4/5/6 high,
// plus omaha-4 hi-lo).
//
// Same methodology as omaha.cc: one workload shape, two orders.  A pool of
// 2^20 heads-up matchups is generated up front (off the clock); a matchup
// is two k-card holdings (k = 4, 5, 6) plus a shared 5-card board, all
// 2k+5 cards distinct.  One benchmark iteration streams the whole pool
// through the fused equity call: both players' hole masks and the shared
// board in, the batch's outcome counts out, counts folded into
// accumulators.  The two pools:
//
//   Random       2k+5 distinct cards drawn per matchup (fixed-seed LCG);
//                the first k drawn are player a's hole cards, the next k
//                player b's, the last five the board
//   Sequential   consecutive (2k+5)-card combinations in enumeration
//                order (colex, via Gosper's hack); the lowest k cards are
//                player a's, the next k player b's, the highest five the
//                board.  That split keeps the board nearly constant from
//                matchup to matchup -- the access pattern of a real
//                equity enumeration.
//
// Two call shapes, mirroring omaha.cc's eval rows:
//
//   CircuitEquity        circuit_equity_omaha, high only; one circuit
//                        serves all three k exactly
//   CircuitEquityHiLo    circuit_equity_omaha_hilo: high and low
//                        comparisons from one packed input, low-half-of-
//                        the-pot counts (scoop folded in); omaha-4 pools
//                        only, so the high rows above are the baseline
//                        and these price the low side as an increment
//
// The comparison that matters is against omaha.cc's CircuitEval rows: a
// matchup evaluates each circuit twice (once per player), so twice the
// eval's ns/hand is break-even.  Anything under that is what the fusion
// saves -- the comparator reads the value planes directly, so per-hand
// values are never transposed back out, and the shared board is packed
// once instead of twice.  items_per_second is matchups/s; the ns/matchup
// counter is its inverse.
#include <cstdint>

#include "bench_perf.h"
#include "benchmark/benchmark.h"
#include "circuit_eval.h"

typedef uint64_t u64;

static const int NBATCH = (1 << 20) / BS_BATCH;  // 2^20 matchups per pool
static const long NMATCH = (long)NBATCH * BS_BATCH;

static void per_matchup(benchmark::State &state, int64_t matchups_per_iter) {
  state.SetItemsProcessed(state.iterations() * matchups_per_iter);
  state.counters["ns/matchup"] = benchmark::Counter(
      (double)state.iterations() * (double)matchups_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, matchups_per_iter, "matchup");
}

// ---- the pools (one random + one sequential per k) -----------------------
// Hole and board presence masks (bit 4*rank+suit, the repo's card ids), in
// the call shape circuit_equity_omaha consumes: two hole arrays, one
// shared board array.

struct Pool {
  u64 (*hole_a)[BS_BATCH];
  u64 (*hole_b)[BS_BATCH];
  u64 (*board)[BS_BATCH];
};

static void alloc(Pool &p) {
  p.hole_a = new u64[NBATCH][BS_BATCH];
  p.hole_b = new u64[NBATCH][BS_BATCH];
  p.board = new u64[NBATCH][BS_BATCH];
}

// same LCG as holdem.cc / verify/pack/bench_pack.c
static u64 rng_state;
static u64 rng_next() {
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return rng_state >> 33;
}

static Pool &random_pool(int k) {
  static Pool pools[3];
  Pool &p = pools[k - 4];
  if (!p.hole_a) {
    alloc(p);
    rng_state = 0x9e3779b97f4a7c15ull;
    for (int b = 0; b < NBATCH; b++)
      for (int l = 0; l < BS_BATCH; l++) {
        int cards[52];
        for (int i = 0; i < 52; i++) cards[i] = i;
        u64 ha = 0, hb = 0, bd = 0;
        for (int i = 0; i < 2 * k + 5; i++) { // partial Fisher-Yates
          int j = i + (int)(rng_next() % (52 - i));
          int t = cards[i]; cards[i] = cards[j]; cards[j] = t;
          (i < k ? ha : i < 2 * k ? hb : bd) |= 1ull << cards[i];
        }
        p.hole_a[b][l] = ha;
        p.hole_b[b][l] = hb;
        p.board[b][l] = bd;
      }
  }
  return p;
}

// Detach and return the lowest k set bits of *x.
static u64 take_low(u64 *x, int k) {
  u64 m = 0;
  for (int i = 0; i < k; i++) {
    u64 bit = *x & -*x;
    m |= bit;
    *x ^= bit;
  }
  return m;
}

static Pool &sequential_pool(int k) {
  static Pool pools[3];
  Pool &p = pools[k - 4];
  if (!p.hole_a) {
    alloc(p);
    u64 x = (1ull << (2 * k + 5)) - 1; // lowest (2k+5)-card combination
    for (int b = 0; b < NBATCH; b++)
      for (int l = 0; l < BS_BATCH; l++) {
        u64 r = x;
        p.hole_a[b][l] = take_low(&r, k);
        p.hole_b[b][l] = take_low(&r, k);
        p.board[b][l] = r; // the remaining (highest) 5 bits
        // Gosper's hack, division replaced by a shift
        u64 u = x & -x;
        u64 v = x + u;
        x = v | ((x ^ v) >> (__builtin_ctzll(x) + 2));
      }
  }
  return p;
}

// ---- high only -----------------------------------------------------------

static void run_equity(benchmark::State &state, const Pool &p) {
  for (auto _ : state) {
    u64 wins = 0, ties = 0;
    for (int b = 0; b < NBATCH; b++) {
      u64 w, t;
      circuit_equity_omaha(p.hole_a[b], p.hole_b[b], p.board[b],
                           BS_BATCH, &w, &t);
      wins += w;
      ties += t;
    }
    benchmark::DoNotOptimize(wins);
    benchmark::DoNotOptimize(ties);
  }
  per_matchup(state, NMATCH);
}

static void CircuitEquityOmaha4Random(benchmark::State &state) {
  run_equity(state, random_pool(4));
}
BENCHMARK(CircuitEquityOmaha4Random);

static void CircuitEquityOmaha4Sequential(benchmark::State &state) {
  run_equity(state, sequential_pool(4));
}
BENCHMARK(CircuitEquityOmaha4Sequential);

static void CircuitEquityOmaha5Random(benchmark::State &state) {
  run_equity(state, random_pool(5));
}
BENCHMARK(CircuitEquityOmaha5Random);

static void CircuitEquityOmaha5Sequential(benchmark::State &state) {
  run_equity(state, sequential_pool(5));
}
BENCHMARK(CircuitEquityOmaha5Sequential);

static void CircuitEquityOmaha6Random(benchmark::State &state) {
  run_equity(state, random_pool(6));
}
BENCHMARK(CircuitEquityOmaha6Random);

static void CircuitEquityOmaha6Sequential(benchmark::State &state) {
  run_equity(state, sequential_pool(6));
}
BENCHMARK(CircuitEquityOmaha6Sequential);

// ---- hi-lo ---------------------------------------------------------------

static void run_equity_hilo(benchmark::State &state, const Pool &p) {
  for (auto _ : state) {
    u64 acc[5] = {0, 0, 0, 0, 0};
    for (int b = 0; b < NBATCH; b++) {
      u64 c[5];
      circuit_equity_omaha_hilo(p.hole_a[b], p.hole_b[b], p.board[b],
                                BS_BATCH, &c[0], &c[1], &c[2], &c[3], &c[4]);
      for (int i = 0; i < 5; i++) acc[i] += c[i];
    }
    benchmark::DoNotOptimize(acc);
  }
  per_matchup(state, NMATCH);
}

static void CircuitEquityOmaha4HiLoRandom(benchmark::State &state) {
  run_equity_hilo(state, random_pool(4));
}
BENCHMARK(CircuitEquityOmaha4HiLoRandom);

static void CircuitEquityOmaha4HiLoSequential(benchmark::State &state) {
  run_equity_hilo(state, sequential_pool(4));
}
BENCHMARK(CircuitEquityOmaha4HiLoSequential);

int main(int argc, char **argv) {
  if (circuit_eval_omaha_raw_nb != 1 || circuit_eval_omaha_low_raw_nb != 1 ||
      circuit_cmp24_raw_nb != 1 || circuit_cmp8_raw_nb != 1)
    return 1; // multi-batch not served here
  if (circuit_eval_omaha_raw_num_outputs != 24 ||
      circuit_eval_omaha_low_raw_num_outputs != 8)
    return 1; // the 24-bit high / 8-bit low shapes documented in the header
  if (circuit_cmp24_raw_num_inputs != 48 || circuit_cmp24_raw_num_outputs != 2 ||
      circuit_cmp8_raw_num_inputs != 16 || circuit_cmp8_raw_num_outputs != 2)
    return 1; // two values in, gt + eq out (circuit_eval.h)
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
