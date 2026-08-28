// Publication benchmark harness, bspack -- the conversion layer between
// per-hand values and the bit planes the circuits read (lib/bspack/,
// API in include/bsapi.h).  No circuit is evaluated here: this binary prices
// the packing alone, the difference between a circuit-only figure
// (bench/holdem_native.cc, bench/omaha_native.cc) and a mask-API one
// (bench/holdem.cc, bench/omaha.cc).
//
// Two kernels, measured in the shapes the library actually calls them in.
//
//   transpose   masks -> planes and planes -> values, three shapes:
//                 input holdem   bs_transpose_map, 52 planes, the shipped
//                                holdem circuit's card_input permutation
//                                (what circuit_eval_holdem packs)
//                 input omaha    two bs_transpose calls, hole -> planes
//                                0..51 and board -> 52..103 (what
//                                circuit_eval_omaha packs); the second
//                                call's 12 junk planes are why the buffer
//                                is 116 wide
//                 output         bs_untranspose, 24 planes -> one u32 per
//                                hand (the high value), plus the 8-plane
//                                shape the omaha low circuit emits
//   masks       bs_masks_from_cards: card ids -> presence masks, in the
//               arities and strides the games use -- holdem 7 cards at
//               stride 7, and per omaha k the two calls one deal needs
//               (k hole then 5 board, both at stride k+5)
//
// The last two rows compose the whole conversion layer for one game --
// masks, pack, unpack, everything circuit_eval_* does except calling the
// circuit.
//
// Workload shape follows the rest of the suite: a pool of 2^20 hands
// (deals) is built off the clock and one iteration streams the whole pool
// through the kernel.  The per-hand side of each kernel (card lists,
// masks, values) is the pool, streamed from RAM, and the plane side is a
// single batch buffer reused across batches -- exactly the library's own
// arrangement, where planes live on the stack for the circuit sitting
// between the two transposes and never reach memory.  The output planes
// the unpack rows read are pseudorandom bits rather than circuit output;
// untranspose is data-independent.
//
// The build knobs of lib/Makefile apply here unchanged; the library's
// alternate shipped cores are measured by rebuilding with a knob and
// reading the same rows:
//   make -C bench run-bspack MASKS=scalar   the scalar
//                                           bs_masks_from_cards core
//   make -C bench run-bspack PACK=portable  the portable pack/unpack
//                                           composition at full -march
//   make -C bench run-bspack BATCH=256      the narrow layout
//   make -C bench run-bspack ARCH=x86-64-v3 the AVX2 paths
// (each after a `make -C bench clean`, as elsewhere in this Makefile).
//
// items_per_second is hands/s; the ns/hand counter is its inverse.  For
// the omaha rows a "hand" is one deal, i.e. both calls of the pair.
#include <cstdint>

#include "bench_perf.h"
#include "benchmark/benchmark.h"
#include "circuit_eval.h"

typedef uint64_t u64;

static const long NHANDS = 1 << 20;  // hands per pool
static const int  NBATCH = NHANDS / BS_BATCH;
static_assert(NHANDS % BS_BATCH == 0, "pool must be whole batches");

static const int NOUT_HIGH = 24;  // holdem / omaha high value planes
static const int NOUT_LOW  = 8;   // omaha low value planes

static void per_hand(benchmark::State &state, int64_t hands_per_iter) {
  state.SetItemsProcessed(state.iterations() * hands_per_iter);
  state.counters["ns/hand"] = benchmark::Counter(
      (double)state.iterations() * (double)hands_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, hands_per_iter, "hand");
}

// ---- the pools -----------------------------------------------------------
// Card ids are 0..51 (4*rank + suit), drawn by the same fixed-seed LCG and
// partial Fisher-Yates the rest of the suite uses, so a pool here holds the
// same hands the evaluator benchmarks see.  Pools are built on first use:
// a filtered run pays only for the rows it asks for.

static u64 rng_state;
static u64 rng_next() {
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return rng_state >> 33;
}

// n distinct card ids into out[0..n-1]
static void draw(int n, uint8_t *out) {
  int deck[52];
  for (int c = 0; c < 52; c++) deck[c] = c;
  for (int i = 0; i < n; i++) {
    int j = i + (int)(rng_next() % (52 - i));
    int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    out[i] = (uint8_t)deck[i];
  }
}

static uint8_t (*holdem_cards())[7] {
  static uint8_t (*pool)[7];
  if (!pool) {
    pool = new uint8_t[NHANDS][7];
    rng_state = 0x9e3779b97f4a7c15ull;
    for (long i = 0; i < NHANDS; i++) draw(7, pool[i]);
  }
  return pool;
}

static const u64 *holdem_masks() {
  static u64 *pool;
  if (!pool) {
    const uint8_t (*cards)[7] = holdem_cards();
    pool = new u64[NHANDS];
    for (long i = 0; i < NHANDS; i++) {
      u64 m = 0;
      for (int j = 0; j < 7; j++) m |= 1ull << cards[i][j];
      pool[i] = m;
    }
  }
  return pool;
}

// One omaha deal is k hole cards then 5 board cards, k+5 bytes per record:
// the layout bs_masks_from_cards converts with two calls at stride k+5.
struct OmahaPool {
  uint8_t *cards;
  u64 *hole, *board;
};

static const OmahaPool &omaha_pool(int k) {
  static OmahaPool pools[3];
  OmahaPool &p = pools[k - 4];
  if (!p.cards) {
    const int nd = k + 5;
    p.cards = new uint8_t[(size_t)NHANDS * nd];
    p.hole = new u64[NHANDS];
    p.board = new u64[NHANDS];
    rng_state = 0x9e3779b97f4a7c15ull;
    for (long i = 0; i < NHANDS; i++) {
      uint8_t *d = p.cards + i * nd;
      draw(nd, d);
      u64 h = 0, b = 0;
      for (int j = 0; j < k; j++) h |= 1ull << d[j];
      for (int j = k; j < nd; j++) b |= 1ull << d[j];
      p.hole[i] = h;
      p.board[i] = b;
    }
  }
  return p;
}

static uint32_t *vals_pool() {
  static uint32_t *pool;
  if (!pool) pool = new uint32_t[NHANDS];
  return pool;
}

// The plane side: one batch buffer per shape, reused across batches (the
// library's own arrangement -- see the header).
static bs_word PLANES_HOLDEM[52];      // bs_transpose_map writes 52
static bs_word PLANES_OMAHA[116];      // 52 hole + 64 board (12 junk)
static bs_word OUT_PLANES[NOUT_HIGH];  // untranspose input, random bits

static void fill_out_planes() {
  u64 *w = (u64 *)OUT_PLANES;
  rng_state = 0x243f6a8885a308d3ull;
  for (size_t i = 0; i < NOUT_HIGH * BS_LANES; i++)
    w[i] = rng_next() ^ (rng_next() << 21) ^ (rng_next() << 42);
}

// ---- transpose: input holdem (52 planes, permuted) -----------------------

static const int *holdem_map() { return circuit_eval_holdem_raw_card_input; }

static void PackHoldem(benchmark::State &state) {
  const u64 *masks = holdem_masks();
  for (auto _ : state) {
    for (int b = 0; b < NBATCH; b++)
      bs_transpose_map(masks + (long)b * BS_BATCH, holdem_map(),
                       PLANES_HOLDEM);
    benchmark::ClobberMemory();
  }
  per_hand(state, NHANDS);
}
BENCHMARK(PackHoldem);

// ---- transpose: input omaha (104 planes, hole + board) -------------------
// Plane contents do not depend on k (a mask is a mask), so one pair of
// rows covers omaha 4/5/6; the k-dependence lives in the mask rows below.

static void PackOmaha(benchmark::State &state) {
  const OmahaPool &p = omaha_pool(4);
  for (auto _ : state) {
    for (int b = 0; b < NBATCH; b++) {
      bs_transpose(p.hole + (long)b * BS_BATCH, PLANES_OMAHA);
      bs_transpose(p.board + (long)b * BS_BATCH, PLANES_OMAHA + 52);
    }
    benchmark::ClobberMemory();
  }
  per_hand(state, NHANDS);
}
BENCHMARK(PackOmaha);

// ---- transpose: output (planes -> values) --------------------------------

static void unpack(benchmark::State &state, int nout) {
  uint32_t *vals = vals_pool();
  for (auto _ : state) {
    for (int b = 0; b < NBATCH; b++)
      bs_untranspose(OUT_PLANES, nout, vals + (long)b * BS_BATCH);
    benchmark::ClobberMemory();
  }
  per_hand(state, NHANDS);
}

static void UnpackHigh24(benchmark::State &state) { unpack(state, NOUT_HIGH); }
BENCHMARK(UnpackHigh24);

static void UnpackLow8(benchmark::State &state) { unpack(state, NOUT_LOW); }
BENCHMARK(UnpackLow8);

// ---- masks from card lists -----------------------------------------------

static u64 MASKS_A[BS_BATCH], MASKS_B[BS_BATCH];

static void MasksHoldem(benchmark::State &state) {
  const uint8_t (*cards)[7] = holdem_cards();
  for (auto _ : state) {
    for (int b = 0; b < NBATCH; b++)
      bs_masks_from_cards(&cards[(long)b * BS_BATCH][0], 7, 7, MASKS_A);
    benchmark::ClobberMemory();
  }
  per_hand(state, NHANDS);
}
BENCHMARK(MasksHoldem);

// One deal = two calls over the same record: k hole cards at offset 0 and
// 5 board cards at offset k, both at stride k+5.
static void masks_omaha(benchmark::State &state, int k) {
  const OmahaPool &p = omaha_pool(k);
  const size_t nd = k + 5;
  for (auto _ : state) {
    for (int b = 0; b < NBATCH; b++) {
      const uint8_t *rec = p.cards + (size_t)b * BS_BATCH * nd;
      bs_masks_from_cards(rec, k, nd, MASKS_A);
      bs_masks_from_cards(rec + k, 5, nd, MASKS_B);
    }
    benchmark::ClobberMemory();
  }
  per_hand(state, NHANDS);
}

static void MasksOmaha4(benchmark::State &state) { masks_omaha(state, 4); }
BENCHMARK(MasksOmaha4);

static void MasksOmaha5(benchmark::State &state) { masks_omaha(state, 5); }
BENCHMARK(MasksOmaha5);

static void MasksOmaha6(benchmark::State &state) { masks_omaha(state, 6); }
BENCHMARK(MasksOmaha6);

// ---- the whole conversion layer, per game --------------------------------
// circuit_eval_holdem / circuit_eval_omaha with the circuit call removed:
// card lists in, values out, the same buffers in between.  Read against
// the native-input rows (bench/holdem_native.cc, bench/omaha_native.cc)
// and the mask-API rows (bench/holdem.cc, bench/omaha.cc), whose
// difference this is.

static void PipelineHoldem(benchmark::State &state) {
  const uint8_t (*cards)[7] = holdem_cards();
  uint32_t *vals = vals_pool();
  for (auto _ : state) {
    for (int b = 0; b < NBATCH; b++) {
      bs_masks_from_cards(&cards[(long)b * BS_BATCH][0], 7, 7, MASKS_A);
      bs_transpose_map(MASKS_A, holdem_map(), PLANES_HOLDEM);
      bs_untranspose(OUT_PLANES, NOUT_HIGH, vals + (long)b * BS_BATCH);
    }
    benchmark::ClobberMemory();
  }
  per_hand(state, NHANDS);
}
BENCHMARK(PipelineHoldem);

static void PipelineOmaha(benchmark::State &state) {
  const OmahaPool &p = omaha_pool(4);
  uint32_t *vals = vals_pool();
  for (auto _ : state) {
    for (int b = 0; b < NBATCH; b++) {
      const uint8_t *rec = p.cards + (size_t)b * BS_BATCH * 9;
      bs_masks_from_cards(rec, 4, 9, MASKS_A);
      bs_masks_from_cards(rec + 4, 5, 9, MASKS_B);
      bs_transpose(MASKS_A, PLANES_OMAHA);
      bs_transpose(MASKS_B, PLANES_OMAHA + 52);
      bs_untranspose(OUT_PLANES, NOUT_HIGH, vals + (long)b * BS_BATCH);
    }
    benchmark::ClobberMemory();
  }
  per_hand(state, NHANDS);
}
BENCHMARK(PipelineOmaha);

int main(int argc, char **argv) {
  if (circuit_eval_holdem_raw_num_outputs != NOUT_HIGH) return 1;
  fill_out_planes();
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
