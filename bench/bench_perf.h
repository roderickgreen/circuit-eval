// Per-item PMU counters.
//
// google-benchmark reports the counters --benchmark_perf_counters asks for
// per benchmark iteration, and one iteration here is a whole pool -- 2^20
// hands or matchups.  per_item_perf_counters() rescales them to per item and
// renames them to match: CYCLES becomes CYCLES/hand.
//
// Needs the harness built with PERF=1 (see the Makefile) and
// kernel.perf_event_paranoid <= 2.  Without either, this is a no-op.
#ifndef BENCH_PERF_H
#define BENCH_PERF_H

#include <string>
#include <vector>

#include "benchmark/benchmark.h"

// Call from the per-hand/per-matchup helper, after that helper has set its own
// counters.  What to rescale is what is left: gbench keeps user counters, its
// own rate counters and the perf counters in one map, and ours all carry a
// "/" while gbench's own are the two _per_second rates.
//
// A kAvgIterations counter stores the raw total -- gbench divides by the
// iteration count when it reports -- so the rescale is one divide by the item
// count and the flag stays as it was.
inline void per_item_perf_counters(benchmark::State &state,
                                   int64_t items_per_iter, const char *unit) {
  std::vector<std::string> perf;
  for (const auto &kv : state.counters) {
    if (kv.first.find('/') != std::string::npos) continue;
    if (kv.first == "items_per_second" || kv.first == "bytes_per_second")
      continue;
    perf.push_back(kv.first);
  }
  for (const auto &name : perf) {
    benchmark::Counter c = state.counters[name];
    state.counters.erase(name);
    state.counters[name + "/" + unit] = benchmark::Counter(
        c.value / (double)items_per_iter, benchmark::Counter::kAvgIterations);
  }
}

#endif  // BENCH_PERF_H
