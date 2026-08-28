// Best-implementation benchmark harness, omaha-4 high.
//
// The main omaha battery (omaha.cc) hands every evaluator the same
// neutral input -- a list of nine card ids per deal -- and forbids
// cross-hand state.  This file asks the opposite question, exactly as
// holdem_enum.cc does for holdem: evaluating 2^20 distinct deals, how
// fast can each evaluator go when it is allowed to play to its
// strengths?  The workload is the first 2^20 deals of the nested-loop
// enumeration c0 < c1 < ... < c8 over the deck, the lowest four cards
// as the hole cards and the highest five as the board (the same split
// as the main battery's sequential pool), each deal evaluated exactly
// once, every value folded into an accumulator.
//
// A consequence of truncating the lexicographic order worth knowing:
// there are C(48,5) = 1,712,304 boards for the first hole combination,
// so all 2^20 deals share the hole cards {0,1,2,3} and the walk is a
// fixed-hole board sweep -- the shape of a real equity enumeration.
// The board prefix still advances level by level, and each evaluator's
// walk is written to exploit exactly that:
//
//   CircuitEval  the mask-space walk: board loop levels c4..c6 each
//                carry their prefix's presence mask (one OR when the
//                level advances), the two innermost levels are a table
//                of all C(52,2) two-card masks in (c7,c8) order, built
//                on the clock, and the hole mask is one OR chain reused
//                for as long as it lasts, so the inner loop stores
//                prefix | pair[k] board masks plus the constant hole
//                mask into a batch over one contiguous run per c6; each
//                full batch goes through the packaged mask API
//                (circuit_eval_omaha: vectorized transpose, circuit,
//                per-hand values transposed back out) and the values
//                are summed, the same altitude as the scalar row
//   PHE          evaluate_plo4_cards(c4..c8, c0..c3) in the plain
//                nested loop; its perfect-hash scheme keeps no
//                per-prefix state a caller could reuse, which is
//                itself a data point (the same finding as holdem)
//
// Every walk sums its per-hand values; the checksums are per-evaluator
// (the ranks live on different scales).  The circuit walk's segmented
// batch fill is verified at startup against a plain per-hand
// enumeration through the same mask API (same value sum), and every
// walk checks that it evaluated exactly 2^20 deals.
//
// 2^20 matches the pool size of the main battery, and is a multiple of
// BS_BATCH, so the circuit walk stops on a batch boundary.
//
// items_per_second is hands/s; the ns/hand counter is its inverse.
#include <cstdint>
#include <cstdio>

#include "bench_perf.h"
#include "benchmark/benchmark.h"
#include "circuit_eval.h"
#include "phevaluator/phevaluator.h"

typedef uint64_t u64;

static const long NHANDS = 1 << 20;  // deals per benchmark iteration

static void per_hand(benchmark::State &state, int64_t hands_per_iter) {
  state.SetItemsProcessed(state.iterations() * hands_per_iter);
  state.counters["ns/hand"] = benchmark::Counter(
      (double)state.iterations() * (double)hands_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, hands_per_iter, "hand");
}

// ---- CircuitEval: the mask-space walk ------------------------------------
// Presence masks are enumerated directly.  The hole mask is built once
// per hole combination; board levels c4..c6 each carry their prefix's
// mask, one OR when the level advances.  The two innermost levels,
// c7 < c8, are a table PAIR of every two-card mask in lexicographic
// (c7,c8) order: for a board prefix ending at c6 the boards are
// prefix | PAIR[k] for k from POFF[c6+1] to the end of the table, one
// contiguous run of C(51-c6,2) deals, which the compiler vectorizes as
// two stores per deal (constant hole, load | OR | store board).
// (Enumerating c8 directly gives runs of 51-c7 deals, and the per-run
// setup and tail dominate the fill.)  The table and the one-hot card
// masks are built inside the walk, on the clock, as the harness rule
// requires.  Each full batch goes through the packaged mask API:
// transpose in, circuit, values transposed back out and summed.  The
// output transpose is included deliberately, so the row prices
// delivering per-hand values (the scalar row's altitude).

static const int NPAIR = 52 * 51 / 2;

static u64  HOLE[BS_BATCH];   // the current batch, hole mask per deal
static u64  BOARD[BS_BATCH];  // and board mask per deal
static uint32_t VALS[BS_BATCH];
static u64  ACC;              // sum of all deals' values (the checksum)
static long CIRC_N;           // deals evaluated by the current walk
static u64  BIT[52];          // one-hot card masks (selfcheck's walk)
static u64  PAIR[NPAIR];      // BIT[a] | BIT[b], a < b, (a,b) ascending
static int  POFF[53];         // POFF[a] = index of the first pair (a, *)

static void init_bits(void) {
  for (int c = 0; c < 52; c++) BIT[c] = 1ull << c;
}

// evaluate the full batch, fold every deal's value into ACC, start a
// fresh batch.  Inlined into the walk on purpose.
static inline void emit(void) {
  circuit_eval_omaha(HOLE, BOARD, VALS);
  for (int l = 0; l < BS_BATCH; l++) ACC += VALS[l];
  CIRC_N += BS_BATCH;
}

// The nested-loop enumeration over card ids -- the same 2^20 deals the
// scalar walk visits.  Per c6 board prefix, the run PAIR[POFF[c6+1]..
// NPAIR) fills consecutive batch slots under a constant hole mask, in
// segments clipped to the batch boundary.  NHANDS is a multiple of
// BS_BATCH, so the cutoff lands exactly on a batch boundary.
static void walk_circuit(void) {
  ACC = 0;
  CIRC_N = 0;

  u64 bit[52];
  for (int c = 0; c < 52; c++) bit[c] = 1ull << c;
  {
    int k = 0;
    for (int a = 0; a < 52; a++) {
      POFF[a] = k;
      for (int b = a + 1; b < 52; b++) PAIR[k++] = bit[a] | bit[b];
    }
    POFF[52] = k;
  }

  int L = 0;  // next free slot of the current batch

  for (int c0 = 0; c0 < 44; c0++) {
    u64 h0 = bit[c0];
    for (int c1 = c0 + 1; c1 < 45; c1++) {
      u64 h1 = h0 | bit[c1];
      for (int c2 = c1 + 1; c2 < 46; c2++) {
        u64 h2 = h1 | bit[c2];
        for (int c3 = c2 + 1; c3 < 47; c3++) {
          u64 hole = h2 | bit[c3];
          for (int c4 = c3 + 1; c4 < 48; c4++) {
            u64 p4 = bit[c4];
            for (int c5 = c4 + 1; c5 < 49; c5++) {
              u64 p5 = p4 | bit[c5];
              for (int c6 = c5 + 1; c6 < 50; c6++) {
                u64 p6 = p5 | bit[c6];
                for (int k = POFF[c6 + 1]; k < NPAIR; ) {
                  int seg = NPAIR - k;
                  int room = BS_BATCH - L;
                  if (seg > room) seg = room;
                  for (int i = 0; i < seg; i++) {
                    HOLE[L + i] = hole;
                    BOARD[L + i] = p6 | PAIR[k + i];
                  }
                  k += seg;
                  L += seg;
                  if (L == BS_BATCH) {
                    emit();
                    if (CIRC_N == NHANDS) return;
                    L = 0;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

// Startup check: the same 2^20 deals enumerated one at a time (no
// segment fill, no pair table) through the same mask API must produce
// the same value sum -- pinning the walk's table and segment clipping
// against the plain loop.
static int circuit_selfcheck(void) {
  static u64 hole[BS_BATCH], board[BS_BATCH];
  static uint32_t vals[BS_BATCH];
  u64 sum = 0;
  long n = 0;
  int l = 0;

  for (int c0 = 0; c0 < 44; c0++) {
    u64 h0 = BIT[c0];
    for (int c1 = c0 + 1; c1 < 45; c1++) {
      u64 h1 = h0 | BIT[c1];
      for (int c2 = c1 + 1; c2 < 46; c2++) {
        u64 h2 = h1 | BIT[c2];
        for (int c3 = c2 + 1; c3 < 47; c3++) {
          u64 h3 = h2 | BIT[c3];
          for (int c4 = c3 + 1; c4 < 48; c4++) {
            u64 m4 = BIT[c4];
            for (int c5 = c4 + 1; c5 < 49; c5++) {
              u64 m5 = m4 | BIT[c5];
              for (int c6 = c5 + 1; c6 < 50; c6++) {
                u64 m6 = m5 | BIT[c6];
                for (int c7 = c6 + 1; c7 < 51; c7++) {
                  u64 m7 = m6 | BIT[c7];
                  for (int c8 = c7 + 1; c8 < 52; c8++) {
                    hole[l] = h3;
                    board[l] = m7 | BIT[c8];
                    if (++l == BS_BATCH) {
                      circuit_eval_omaha(hole, board, vals);
                      for (int i = 0; i < BS_BATCH; i++) sum += vals[i];
                      l = 0;
                    }
                    if (++n == NHANDS) goto done;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
done:
  if (n != NHANDS || l != 0) return 0;
  walk_circuit();
  if (CIRC_N != NHANDS) return 0;
  return sum == ACC;
}

static void CircuitEval(benchmark::State &state) {
  for (auto _ : state) {
    walk_circuit();
    benchmark::DoNotOptimize(ACC);
    if (CIRC_N != NHANDS) {
      state.SkipWithError("deal count wrong");
      return;
    }
  }
  per_hand(state, NHANDS);
}
BENCHMARK(CircuitEval);

// ---- PHE -----------------------------------------------------------------
// No per-prefix state to hoist: the full nine cards go in every call,
// board first and then hole (the evaluator's own argument order).

static u64 walk_phe(long *count) {
  u64 sum = 0;
  long n = 0;
  for (int c0 = 0; c0 < 44; c0++) {
    for (int c1 = c0 + 1; c1 < 45; c1++) {
      for (int c2 = c1 + 1; c2 < 46; c2++) {
        for (int c3 = c2 + 1; c3 < 47; c3++) {
          for (int c4 = c3 + 1; c4 < 48; c4++) {
            for (int c5 = c4 + 1; c5 < 49; c5++) {
              for (int c6 = c5 + 1; c6 < 50; c6++) {
                for (int c7 = c6 + 1; c7 < 51; c7++) {
                  int hi = 52;
                  if (51 - c7 >= NHANDS - n) hi = c7 + 1 + (int)(NHANDS - n);
                  for (int c8 = c7 + 1; c8 < hi; c8++)
                    sum += (u64)evaluate_plo4_cards(c4, c5, c6, c7, c8,
                                                    c0, c1, c2, c3);
                  n += hi - (c7 + 1);
                  if (n == NHANDS) goto done;
                }
              }
            }
          }
        }
      }
    }
  }
done:
  *count = n;
  return sum;
}

static void PHE(benchmark::State &state) {
  for (auto _ : state) {
    long n = 0;
    u64 sum = walk_phe(&n);
    benchmark::DoNotOptimize(sum);
    if (n != NHANDS) {
      state.SkipWithError("deal count wrong");
      return;
    }
  }
  per_hand(state, NHANDS);
}
BENCHMARK(PHE);

int main(int argc, char **argv) {
  init_bits();
  if (circuit_eval_omaha_raw_nb != 1 ||
      circuit_eval_omaha_raw_num_outputs != 24)
    return 1; // the single-batch 24-bit shape documented in the header
  if (!circuit_selfcheck()) {
    fprintf(stderr,
            "circuit segmented walk disagrees with per-hand enumeration\n");
    return 1;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
