// Best-implementation benchmark harness, omaha (PLO4/5/6 high), random
// order.
//
// The omaha companion to holdem_native.cc, circuit-only: the same 2^20
// random deals per k as the main battery's Random rows (identical
// generator, identical seed), with the input pre-marshalled into the
// circuit's native form off the clock, so the timed loop is pure
// evaluation.  The other omaha evaluator benchmarked in this repo (PHE's
// dedicated plo4/5/6 oracles) takes card ids natively, so a prebuilt row
// for it would only repeat the main battery's Random rows -- same
// reasoning as holdem_native.cc.
//
// Native form: the raw circuit's 104 input planes per batch -- hole card
// c at plane [c], board card c at plane [52 + c] -- exactly the layout
// circuit_eval_omaha packs on every call.  One circuit serves all three
// k (4, 5, 6 hole cards); only the pool contents differ.  The timed loop
// runs circuit_eval_omaha_raw batch by batch and consumes the 24 output
// planes as per-plane set-bit counts: no transpose on the clock in
// either direction.  A pool is 104 planes x BS_BATCH bits per batch,
// 13 MB per k for 2^20 deals, so the loop streams the pool from memory
// rather than re-evaluating a cache-resident batch.
//
// Each pool is verified at startup: the same hole/board masks pushed
// through the library's mask API (circuit_eval_omaha) must
// reproduce the per-bit counts the raw batch loop accumulates.
//
// items_per_second is hands/s; the ns/hand counter is its inverse.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bench_perf.h"
#include "benchmark/benchmark.h"
#include "circuit_eval.h"

typedef uint64_t u64;

static const long NHANDS = 1 << 20;  // deals per benchmark iteration
static const int  NBATCH = NHANDS / BS_BATCH;
static_assert(NHANDS % BS_BATCH == 0, "pool must be whole batches");

#define NOUT    24
#define NPLANES 104

static void per_hand(benchmark::State &state, int64_t hands_per_iter) {
  state.SetItemsProcessed(state.iterations() * hands_per_iter);
  state.counters["ns/hand"] = benchmark::Counter(
      (double)state.iterations() * (double)hands_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, hands_per_iter, "hand");
}

// ---- the pools -----------------------------------------------------------
// Deals are drawn exactly as in omaha.cc's random pools (same fixed-seed
// LCG, same partial Fisher-Yates, seed reset per k), so each k's pool
// holds the identical 2^20 deals the main battery's Random rows evaluate.

typedef union { bs_word v; u64 u[BS_LANES]; } lanes;

static lanes POOLS[3][NBATCH][NPLANES];  // k = 4, 5, 6 in slots 0..2

static u64 rng_state;
static u64 rng_next() {
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return rng_state >> 33;
}

// one batch of random deals: k hole cards + 5 board cards, disjoint
static void deal_batch(int k, u64 *hole, u64 *board) {
  for (int l = 0; l < BS_BATCH; l++) {
    int cards[52];
    for (int c = 0; c < 52; c++) cards[c] = c;
    u64 h = 0, bd = 0;
    for (int c = 0; c < k + 5; c++) { // partial Fisher-Yates
      int j = c + (int)(rng_next() % (52 - c));
      int t = cards[c]; cards[c] = cards[j]; cards[j] = t;
      (c < k ? h : bd) |= 1ull << cards[c];
    }
    hole[l] = h;
    board[l] = bd;
  }
}

static void build_pool(int k) {
  static u64 hole[BS_BATCH], board[BS_BATCH];
  // bs_transpose writes a full 64 planes, so the batch is staged in a
  // 116-plane buffer (the board's transpose overwrites the hole's junk
  // planes 52..63, as in circuit_eval_omaha) and the 104 the circuit
  // reads are kept
  static bs_word in[116];
  rng_state = 0x9e3779b97f4a7c15ull;
  for (int b = 0; b < NBATCH; b++) {
    deal_batch(k, hole, board);
    bs_transpose(hole, in);
    bs_transpose(board, in + 52);
    memcpy(&POOLS[k - 4][b][0].v, in, NPLANES * sizeof(bs_word));
  }
}

// ---- CircuitEval ---------------------------------------------------------

static lanes OUT[NOUT];
static u64   ACC[NOUT];  // per-plane set-bit counts (the checksum)

static void circuit_pass(int k) {
  memset(ACC, 0, sizeof ACC);
  for (int b = 0; b < NBATCH; b++) {
    circuit_eval_omaha_raw(&POOLS[k - 4][b][0].v, &OUT[0].v);
    for (int o = 0; o < NOUT; o++)
      for (int w = 0; w < BS_LANES; w++)
        ACC[o] += (u64)__builtin_popcountll(OUT[o].u[w]);
  }
}

// Startup check: the same deals through the mask API must produce
// the per-bit counts the plane pool produces through the raw circuit.
static int circuit_selfcheck(int k) {
  static u64 hole[BS_BATCH], board[BS_BATCH];
  static uint32_t vals[BS_BATCH];
  u64 cnt[NOUT] = {0};
  rng_state = 0x9e3779b97f4a7c15ull;
  for (int b = 0; b < NBATCH; b++) {
    deal_batch(k, hole, board);
    circuit_eval_omaha(hole, board, vals);
    for (int i = 0; i < BS_BATCH; i++)
      for (int o = 0; o < NOUT; o++) cnt[o] += (vals[i] >> o) & 1;
  }
  circuit_pass(k);
  for (int o = 0; o < NOUT; o++)
    if (cnt[o] != ACC[o]) return 0;
  return 1;
}

static void run_circuit_eval(benchmark::State &state, int k) {
  for (auto _ : state) {
    circuit_pass(k);
    u64 h = 1469598103934665603ull;
    for (int o = 0; o < NOUT; o++) { h ^= ACC[o]; h *= 1099511628211ull; }
    benchmark::DoNotOptimize(h);
  }
  per_hand(state, NHANDS);
}

static void CircuitEvalOmaha4(benchmark::State &state) {
  run_circuit_eval(state, 4);
}
BENCHMARK(CircuitEvalOmaha4);

static void CircuitEvalOmaha5(benchmark::State &state) {
  run_circuit_eval(state, 5);
}
BENCHMARK(CircuitEvalOmaha5);

static void CircuitEvalOmaha6(benchmark::State &state) {
  run_circuit_eval(state, 6);
}
BENCHMARK(CircuitEvalOmaha6);

int main(int argc, char **argv) {
  if (circuit_eval_omaha_raw_nb != 1) return 1;  // multi-batch not served here
  if (circuit_eval_omaha_raw_num_outputs != NOUT) return 1;
  for (int k = 4; k <= 6; k++) {
    build_pool(k);
    if (!circuit_selfcheck(k)) {
      fprintf(stderr,
              "omaha-%d plane pool disagrees with the mask API\n", k);
      return 1;
    }
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
