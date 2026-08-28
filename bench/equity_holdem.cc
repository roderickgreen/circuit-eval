// Publication benchmark harness, holdem heads-up equity.
//
// Same methodology as holdem.cc: one workload shape, two orders.  A pool
// of 2^20 heads-up matchups is generated up front (off the clock); a
// matchup is two 2-card holdings plus a shared 5-card board, all nine
// cards distinct.  One benchmark iteration streams the whole pool through
// the fused equity call (circuit_equity_holdem): both players' 7-card
// masks in, the batch's win/tie counts out, counts folded into
// accumulators.  The two pools:
//
//   Random       nine distinct cards drawn per matchup (fixed-seed LCG,
//                same generator as holdem.cc); the first two drawn are
//                player a's hole cards, the next two player b's, the
//                last five the board
//   Sequential   consecutive 9-card combinations in enumeration order
//                (colex, via Gosper's hack); the lowest two cards are
//                player a's, the next two player b's, the highest five
//                the board.  That split keeps the board nearly constant
//                from matchup to matchup -- the access pattern of a real
//                equity enumeration.
//
// The comparison that matters is against holdem.cc's CircuitEval rows: a
// matchup evaluates the value circuit twice (once per player), so twice
// the eval's ns/hand is break-even.  Anything under that is what the
// fusion saves -- the comparator reads the value planes directly, so the
// per-hand values are never transposed back out and only win/tie bits
// leave plane space.  items_per_second is matchups/s; the ns/matchup
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

// ---- the two pools -------------------------------------------------------
// Full 7-card presence masks per player (bit 4*rank+suit, the repo's card
// ids), hole cards OR'd with the shared board -- the call shape
// circuit_equity_holdem consumes.

struct Pool {
  u64 (*a)[BS_BATCH];
  u64 (*b)[BS_BATCH];
};

// same LCG as holdem.cc / verify/pack/bench_pack.c
static u64 rng_state;
static u64 rng_next() {
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return rng_state >> 33;
}

static Pool &random_pool() {
  static Pool p;
  if (!p.a) {
    p.a = new u64[NBATCH][BS_BATCH];
    p.b = new u64[NBATCH][BS_BATCH];
    rng_state = 0x9e3779b97f4a7c15ull;
    for (int b = 0; b < NBATCH; b++)
      for (int l = 0; l < BS_BATCH; l++) {
        int cards[52];
        for (int i = 0; i < 52; i++) cards[i] = i;
        u64 ha = 0, hb = 0, bd = 0;
        for (int i = 0; i < 9; i++) { // partial Fisher-Yates
          int j = i + (int)(rng_next() % (52 - i));
          int t = cards[i]; cards[i] = cards[j]; cards[j] = t;
          (i < 2 ? ha : i < 4 ? hb : bd) |= 1ull << cards[i];
        }
        p.a[b][l] = ha | bd;
        p.b[b][l] = hb | bd;
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

static Pool &sequential_pool() {
  static Pool p;
  if (!p.a) {
    p.a = new u64[NBATCH][BS_BATCH];
    p.b = new u64[NBATCH][BS_BATCH];
    u64 x = (1ull << 9) - 1; // lowest 9-card combination
    for (int b = 0; b < NBATCH; b++)
      for (int l = 0; l < BS_BATCH; l++) {
        u64 r = x;
        u64 ha = take_low(&r, 2);
        u64 hb = take_low(&r, 2);
        p.a[b][l] = ha | r; // r: the remaining (highest) 5 bits, the board
        p.b[b][l] = hb | r;
        // Gosper's hack, division replaced by a shift
        u64 u = x & -x;
        u64 v = x + u;
        x = v | ((x ^ v) >> (__builtin_ctzll(x) + 2));
      }
  }
  return p;
}

// ---- the benchmark -------------------------------------------------------

static void run_equity(benchmark::State &state, const Pool &p) {
  for (auto _ : state) {
    u64 wins = 0, ties = 0;
    for (int b = 0; b < NBATCH; b++) {
      u64 w, t;
      circuit_equity_holdem(p.a[b], p.b[b], BS_BATCH, &w, &t);
      wins += w;
      ties += t;
    }
    benchmark::DoNotOptimize(wins);
    benchmark::DoNotOptimize(ties);
  }
  per_matchup(state, NMATCH);
}

static void CircuitEquityRandom(benchmark::State &state) {
  run_equity(state, random_pool());
}
BENCHMARK(CircuitEquityRandom);

static void CircuitEquitySequential(benchmark::State &state) {
  run_equity(state, sequential_pool());
}
BENCHMARK(CircuitEquitySequential);

int main(int argc, char **argv) {
  if (circuit_eval_holdem_raw_nb != 1 || circuit_cmp24_raw_nb != 1)
    return 1; // multi-batch not served here
  if (circuit_cmp24_raw_num_inputs != 48 || circuit_cmp24_raw_num_outputs != 2)
    return 1; // two 24-bit values in, gt + eq out (circuit_eval.h)
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
