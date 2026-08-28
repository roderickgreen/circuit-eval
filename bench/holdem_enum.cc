// Best-implementation benchmark harness, holdem.
//
// The main holdem battery (holdem.cc) hands every evaluator the same
// neutral input -- a list of seven card ids per hand -- and forbids
// cross-hand state.  This file asks the opposite question: evaluating
// 2^20 distinct hands, how fast can each evaluator go when it is allowed
// to play to its strengths?  The workload is the first 2^20 hands of the
// nested-loop enumeration c0 < c1 < ... < c6 over the deck, each hand
// evaluated exactly once, every value folded into an accumulator.
// Consecutive hands share long prefixes, and each evaluator's walk is
// written to exploit exactly that:
//
//   CircuitEval  the mask-space walk: loop levels c0..c4 each carry
//                their prefix's presence mask (one OR when the level
//                advances); the two innermost levels are a table of all
//                C(52,2) two-card masks in (c5,c6) order, built on the
//                clock, so the inner loop stores prefix | pair[k] per
//                hand into a batch of masks over one contiguous run per
//                c4; each full batch goes through the packaged front
//                door (circuit_eval_holdem: vectorized transpose,
//                circuit, per-hand values transposed back out) and the
//                values are summed, the same altitude as the scalar
//                rows
//   TwoPlusTwo   prefix lookups hoisted per loop level (upstream's own
//                enumeration shape), so the inner loop is one
//                dependency-free load from the table per hand
//   OMPEval      one partial Hand per loop level, so the inner loop is
//                one Hand add + evaluate per hand
//   PHE          evaluate_7cards(c0..c6) in the plain nested loop; its
//                perfect-hash scheme keeps no per-prefix state a caller
//                could reuse, which is itself a data point
//
// Every walk sums its per-hand values; the checksums are per-evaluator
// (the ranks live on different scales).  The circuit
// walk's segmented batch fill is verified at startup against a plain
// per-hand enumeration through the same mask API (same value sum),
// and every walk checks that it evaluated exactly 2^20 hands.
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
#include "omp/HandEvaluator.h"
#include "phevaluator/phevaluator.h"

typedef uint64_t u64;

static const long NHANDS = 1 << 20;  // hands per benchmark iteration

static void per_hand(benchmark::State &state, int64_t hands_per_iter) {
  state.SetItemsProcessed(state.iterations() * hands_per_iter);
  state.counters["ns/hand"] = benchmark::Counter(
      (double)state.iterations() * (double)hands_per_iter,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  per_item_perf_counters(state, hands_per_iter, "hand");
}

// ---- CircuitEval: the mask-space walk ------------------------------------
// Presence masks are enumerated directly.  Levels c0..c4 each carry
// their prefix's mask, one OR when the level advances.  The two
// innermost levels, c5 < c6, are a table PAIR of every two-card mask in
// lexicographic (c5,c6) order: for a prefix ending at c4 the hands are
// prefix | PAIR[k] for k from POFF[c4+1] to the end of the table, one
// contiguous run of C(51-c4,2) hands, which the compiler vectorizes as
// load | OR | store.  (Enumerating c6 directly gives runs of 51-c5
// hands, mean about 11 over this workload, and the per-run setup and
// tail dominate the fill.)  The table and the one-hot card masks are
// built inside the walk, on the clock, as the harness rule requires.
// Each full batch goes through the packaged mask API: transpose in,
// circuit, values transposed back out and summed.  The output transpose
// is included deliberately, so the row prices delivering per-hand
// values (the scalar rows' altitude).

static const int NPAIR = 52 * 51 / 2;

static u64  MASKS[BS_BATCH];  // the current batch, one mask per hand
static uint32_t VALS[BS_BATCH];
static u64  ACC;              // sum of all hands' values (the checksum)
static long CIRC_N;           // hands evaluated by the current walk
static u64  BIT[52];          // one-hot card masks (selfcheck's walk)
static u64  PAIR[NPAIR];      // BIT[a] | BIT[b], a < b, (a,b) ascending
static int  POFF[53];         // POFF[a] = index of the first pair (a, *)

static void init_bits(void) {
  for (int c = 0; c < 52; c++) BIT[c] = 1ull << c;
}

// evaluate the full batch, fold every hand's value into ACC, start a
// fresh batch.  Inlined into the walk on purpose.
static inline void emit(void) {
  circuit_eval_holdem(MASKS, VALS);
  for (int l = 0; l < BS_BATCH; l++) ACC += VALS[l];
  CIRC_N += BS_BATCH;
}

// The nested-loop enumeration over card ids -- the same 2^20 hands the
// scalar walks visit.  Per c4 prefix, the run PAIR[POFF[c4+1]..NPAIR)
// fills consecutive batch slots in segments clipped to the batch
// boundary.  NHANDS is a multiple of BS_BATCH, so the cutoff lands
// exactly on a batch boundary.
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

  for (int c0 = 0; c0 < 46; c0++) {
    u64 p0 = bit[c0];
    for (int c1 = c0 + 1; c1 < 47; c1++) {
      u64 p1 = p0 | bit[c1];
      for (int c2 = c1 + 1; c2 < 48; c2++) {
        u64 p2 = p1 | bit[c2];
        for (int c3 = c2 + 1; c3 < 49; c3++) {
          u64 p3 = p2 | bit[c3];
          for (int c4 = c3 + 1; c4 < 50; c4++) {
            u64 p4 = p3 | bit[c4];
            for (int k = POFF[c4 + 1]; k < NPAIR; ) {
              int seg = NPAIR - k;
              int room = BS_BATCH - L;
              if (seg > room) seg = room;
              for (int i = 0; i < seg; i++)
                MASKS[L + i] = p4 | PAIR[k + i];
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

// Startup check: the same 2^20 hands enumerated one at a time (no
// segment fill, no pair table) through the same mask API must produce
// the same value sum -- pinning the walk's table and segment clipping
// against the plain loop.
static int circuit_selfcheck(void) {
  static u64 masks[BS_BATCH];
  static uint32_t vals[BS_BATCH];
  u64 sum = 0;
  long n = 0;
  int l = 0;

  for (int c0 = 0; c0 < 46; c0++) {
    u64 m0 = BIT[c0];
    for (int c1 = c0 + 1; c1 < 47; c1++) {
      u64 m1 = m0 | BIT[c1];
      for (int c2 = c1 + 1; c2 < 48; c2++) {
        u64 m2 = m1 | BIT[c2];
        for (int c3 = c2 + 1; c3 < 49; c3++) {
          u64 m3 = m2 | BIT[c3];
          for (int c4 = c3 + 1; c4 < 50; c4++) {
            u64 m4 = m3 | BIT[c4];
            for (int c5 = c4 + 1; c5 < 51; c5++) {
              u64 m5 = m4 | BIT[c5];
              for (int c6 = c5 + 1; c6 < 52; c6++) {
                masks[l++] = m5 | BIT[c6];
                if (l == BS_BATCH) {
                  circuit_eval_holdem(masks, vals);
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
      state.SkipWithError("hand count wrong");
      return;
    }
  }
  per_hand(state, NHANDS);
}
BENCHMARK(CircuitEval);

// ---- TwoPlusTwo ----------------------------------------------------------
// Upstream's own enumeration shape (third-party/TwoPlusTwoHandEvaluator/
// test.cpp): the prefix pointer u5 = HR[u4+c5] is loop-invariant in c6,
// so the inner loop is one load per hand.  Their deck is 1..52.  The
// inner bound is clamped so the walk stops at exactly NHANDS hands
// without a per-hand counter.

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

static u64 walk_2p2(const int *HR, long *count) {
  u64 sum = 0;
  long n = 0;
  for (int c0 = 1; c0 < 47; c0++) {
    int u0 = HR[53 + c0];
    for (int c1 = c0 + 1; c1 < 48; c1++) {
      int u1 = HR[u0 + c1];
      for (int c2 = c1 + 1; c2 < 49; c2++) {
        int u2 = HR[u1 + c2];
        for (int c3 = c2 + 1; c3 < 50; c3++) {
          int u3 = HR[u2 + c3];
          for (int c4 = c3 + 1; c4 < 51; c4++) {
            int u4 = HR[u3 + c4];
            for (int c5 = c4 + 1; c5 < 52; c5++) {
              int u5 = HR[u4 + c5];
              int hi = 53;
              if (52 - c5 >= NHANDS - n) hi = c5 + 1 + (int)(NHANDS - n);
              for (int c6 = c5 + 1; c6 < hi; c6++) sum += (u64)HR[u5 + c6];
              n += hi - (c5 + 1);
              if (n == NHANDS) goto done;
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

static void TwoPlusTwo(benchmark::State &state) {
  const int *HR = hr_table();
  if (!HR) {
    state.SkipWithError(
        "HandRanks.dat missing: run `make -C bench ../artifacts/HandRanks.dat`");
    return;
  }
  for (auto _ : state) {
    long n = 0;
    u64 sum = walk_2p2(HR, &n);
    benchmark::DoNotOptimize(sum);
    if (n != NHANDS) {
      state.SkipWithError("hand count wrong");
      return;
    }
  }
  per_hand(state, NHANDS);
}
BENCHMARK(TwoPlusTwo);

// ---- OMPEval -------------------------------------------------------------
// The Hand accumulator is associative, so each loop level keeps the Hand
// for its prefix and the inner loop is one add + evaluate per hand.

static u64 walk_omp(omp::HandEvaluator &ev, long *count) {
  u64 sum = 0;
  long n = 0;
  for (int c0 = 0; c0 < 46; c0++) {
    omp::Hand h0 = omp::Hand::empty() + omp::Hand(c0);
    for (int c1 = c0 + 1; c1 < 47; c1++) {
      omp::Hand h1 = h0 + omp::Hand(c1);
      for (int c2 = c1 + 1; c2 < 48; c2++) {
        omp::Hand h2 = h1 + omp::Hand(c2);
        for (int c3 = c2 + 1; c3 < 49; c3++) {
          omp::Hand h3 = h2 + omp::Hand(c3);
          for (int c4 = c3 + 1; c4 < 50; c4++) {
            omp::Hand h4 = h3 + omp::Hand(c4);
            for (int c5 = c4 + 1; c5 < 51; c5++) {
              omp::Hand h5 = h4 + omp::Hand(c5);
              int hi = 52;
              if (51 - c5 >= NHANDS - n) hi = c5 + 1 + (int)(NHANDS - n);
              for (int c6 = c5 + 1; c6 < hi; c6++)
                sum += ev.evaluate(h5 + omp::Hand(c6));
              n += hi - (c5 + 1);
              if (n == NHANDS) goto done;
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

static void OMPEval(benchmark::State &state) {
  static omp::HandEvaluator ev;
  for (auto _ : state) {
    long n = 0;
    u64 sum = walk_omp(ev, &n);
    benchmark::DoNotOptimize(sum);
    if (n != NHANDS) {
      state.SkipWithError("hand count wrong");
      return;
    }
  }
  per_hand(state, NHANDS);
}
BENCHMARK(OMPEval);

// ---- PHE -----------------------------------------------------------------
// No per-prefix state to hoist: the full seven cards go in every call.

static u64 walk_phe(long *count) {
  u64 sum = 0;
  long n = 0;
  for (int c0 = 0; c0 < 46; c0++) {
    for (int c1 = c0 + 1; c1 < 47; c1++) {
      for (int c2 = c1 + 1; c2 < 48; c2++) {
        for (int c3 = c2 + 1; c3 < 49; c3++) {
          for (int c4 = c3 + 1; c4 < 50; c4++) {
            for (int c5 = c4 + 1; c5 < 51; c5++) {
              int hi = 52;
              if (51 - c5 >= NHANDS - n) hi = c5 + 1 + (int)(NHANDS - n);
              for (int c6 = c5 + 1; c6 < hi; c6++)
                sum += (u64)evaluate_7cards(c0, c1, c2, c3, c4, c5, c6);
              n += hi - (c5 + 1);
              if (n == NHANDS) goto done;
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
      state.SkipWithError("hand count wrong");
      return;
    }
  }
  per_hand(state, NHANDS);
}
BENCHMARK(PHE);

int main(int argc, char **argv) {
  init_bits();
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
