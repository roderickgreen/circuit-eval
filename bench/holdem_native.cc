// Best-implementation benchmark harness, holdem, random order.
//
// Companion to holdem_enum.cc.  That battery lets each evaluator exploit
// the prefix sharing of an enumeration walk; this one takes the opposite
// workload -- the same 2^20 uniformly random hands as the main battery's
// Random rows (identical generator, identical seed) -- and grants the
// other freedom instead: the input arrives pre-marshalled into the
// evaluator's native form, built off the clock, so the timed loop is
// pure evaluation.  The pattern is OMPEval's own "precalculated Hand
// objects" benchmark (third-party/OMPEval/benchmark.cpp, random2),
// applied to both evaluators that have a native form beyond the card
// list itself:
//
//   CircuitEval  the pool is pre-transposed into circuit input planes
//                (bs_transpose_map, off the clock); the timed loop runs
//                the raw circuit batch by batch and consumes the 24
//                output planes as per-plane set-bit counts.  No
//                transpose on the clock in either direction.  For a
//                bitsliced evaluator marshalling is the entire gap
//                between its mask-API and circuit-only rates, so this
//                row is the circuit-only rate measured on a real stream
//                of random hands rather than a cache-resident batch.
//   OMPEval      the pool is prebuilt Hand objects; the timed loop is
//                one evaluate per hand -- upstream's random2 exactly.
//
// TwoPlusTwo and PHE are not here: a card list already is their native
// input form, so their prebuilt rows would only repeat the main
// battery's Random rows.  (TwoPlusTwo's actual headroom on random hands
// -- keeping several hands' dependent-load chains in flight at once --
// is a different freedom than precomputed marshalling, and is not this
// file.)
//
// The native pools differ in footprint (52 planes are 6.5 MB for 2^20
// hands, 16-byte Hand objects are 16 MB), so the two rows stream
// different byte counts from memory; that asymmetry is inherent to
// "native form".
//
// The plane pool is verified at startup: the same presence masks pushed
// through the library's mask API (circuit_eval_holdem) must
// reproduce the per-bit counts the raw batch loop accumulates.
//
// items_per_second is hands/s; the ns/hand counter is its inverse.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bench_perf.h"
#include "benchmark/benchmark.h"
#include "circuit_eval.h"
#include "omp/HandEvaluator.h"

typedef uint64_t u64;

static const long NHANDS = 1 << 20;  // hands per benchmark iteration
static const int  NBATCH = NHANDS / BS_BATCH;
static_assert(NHANDS % BS_BATCH == 0, "pool must be whole batches");

#define NOUT 24

static void per_hand(benchmark::State &state, int64_t hands_per_iter) {
  state.SetItemsProcessed(state.iterations() * hands_per_iter);
  state.counters["ns/hand"] = benchmark::Counter(
      (double)state.iterations() * (double)hands_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, hands_per_iter, "hand");
}

// ---- the pools -----------------------------------------------------------
// One source pool of presence masks (the same fixed-seed LCG and partial
// Fisher-Yates as holdem.cc, so these are the identical 2^20 hands the
// main battery's Random rows evaluate), converted once into each
// evaluator's native form before timing starts.

typedef union { bs_word v; u64 u[BS_LANES]; } lanes;

static u64       MASKS[NHANDS];        // source: one presence mask per hand
static lanes     PLANES[NBATCH][52];   // circuit native form, per batch
static omp::Hand HANDS[NHANDS];        // OMPEval native form

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

static void build_pools(void) {
  rng_state = 0x9e3779b97f4a7c15ull;
  for (long i = 0; i < NHANDS; i++) MASKS[i] = sample7();
  for (int b = 0; b < NBATCH; b++)
    bs_transpose_map(&MASKS[(long)b * BS_BATCH],
                     circuit_eval_holdem_raw_card_input, &PLANES[b][0].v);
  for (long i = 0; i < NHANDS; i++) {
    u64 m = MASKS[i];
    omp::Hand h = omp::Hand::empty();
    for (int j = 0; j < 7; j++) {
      h += omp::Hand((unsigned)__builtin_ctzll(m));
      m &= m - 1;
    }
    HANDS[i] = h;
  }
}

// ---- CircuitEval ---------------------------------------------------------

static lanes OUT[NOUT];
static u64   ACC[NOUT];  // per-plane set-bit counts (the checksum)

static void circuit_pass(void) {
  memset(ACC, 0, sizeof ACC);
  for (int b = 0; b < NBATCH; b++) {
    circuit_eval_holdem_raw(&PLANES[b][0].v, &OUT[0].v);
    for (int o = 0; o < NOUT; o++)
      for (int w = 0; w < BS_LANES; w++)
        ACC[o] += (u64)__builtin_popcountll(OUT[o].u[w]);
  }
}

// Startup check: the same masks through the mask API must produce
// the per-bit counts the plane pool produces through the raw circuit.
static int circuit_selfcheck(void) {
  static uint32_t vals[BS_BATCH];
  u64 cnt[NOUT] = {0};
  for (int b = 0; b < NBATCH; b++) {
    circuit_eval_holdem(&MASKS[(long)b * BS_BATCH], vals);
    for (int i = 0; i < BS_BATCH; i++)
      for (int o = 0; o < NOUT; o++) cnt[o] += (vals[i] >> o) & 1;
  }
  circuit_pass();
  for (int o = 0; o < NOUT; o++)
    if (cnt[o] != ACC[o]) return 0;
  return 1;
}

static void CircuitEval(benchmark::State &state) {
  for (auto _ : state) {
    circuit_pass();
    u64 h = 1469598103934665603ull;
    for (int o = 0; o < NOUT; o++) { h ^= ACC[o]; h *= 1099511628211ull; }
    benchmark::DoNotOptimize(h);
  }
  per_hand(state, NHANDS);
}
BENCHMARK(CircuitEval);

// ---- OMPEval -------------------------------------------------------------

static void OMPEval(benchmark::State &state) {
  static omp::HandEvaluator ev;
  for (auto _ : state) {
    u64 acc = 0;
    for (long i = 0; i < NHANDS; i++) acc += ev.evaluate(HANDS[i]);
    benchmark::DoNotOptimize(acc);
  }
  per_hand(state, NHANDS);
}
BENCHMARK(OMPEval);

int main(int argc, char **argv) {
  if (circuit_eval_holdem_raw_nb != 1) return 1;  // multi-batch not served here
  if (circuit_eval_holdem_raw_num_outputs != NOUT) return 1;
  build_pools();
  if (!circuit_selfcheck()) {
    fprintf(stderr,
            "pre-transposed plane pool disagrees with the mask API\n");
    return 1;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
