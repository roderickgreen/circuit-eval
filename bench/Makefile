# Publication benchmarks (google benchmark). Run from repo root:
#   make -C bench          build
#   make -C bench run      build + run (3 repetitions, aggregates only)
#   make -C bench report   build + run every battery below except equity
#                          and native, at publication settings (25
#                          repetitions, interleaved, pinned to one core,
#                          plus PMU counters where libpfm4 is installed),
#                          headed by the OS and compiler versions
# or `make bench` at the root, which does both.
#
# One binary per game so a machine only builds what it measures:
#   build/holdem    CircuitEval / TwoPlusTwo / OMPEval / PHE, random +
#                   sequential
#   build/omaha     CircuitEval / PHE, omaha 4/5/6, random + sequential;
#                   plus the CircuitEval-only omaha-4 hi-lo rows (high +
#                   low combined)
#
# The best-implementation batteries are separate:
#   build/holdem_enum     (`make -C bench run-enum`) 2^20 hands walked in
#                         enumeration order with cross-hand reuse allowed,
#                         each evaluator playing to its strengths
#                         (holdem_enum.cc): CircuitEval / TwoPlusTwo /
#                         OMPEval / PHE
#   build/omaha_enum      (also under run-enum) the omaha-4 high
#                         equivalent: 2^20 deals in enumeration order,
#                         lowest 4 cards hole / highest 5 board
#                         (omaha_enum.cc): CircuitEval / PHE
#   build/holdem_native   (`make -C bench run-native`) the same 2^20
#                         random hands as the main battery, inputs
#                         pre-marshalled into native forms off the clock
#                         (holdem_native.cc): CircuitEval / OMPEval
#   build/omaha_native    (also under run-native) the omaha 4/5/6
#                         equivalent, circuit-only (omaha_native.cc):
#                         pre-transposed 104-plane batches, raw circuit
#                         on the clock
#
# The fused equity calls get their own pair of binaries, same battery
# (2^20 matchups, random + sequential), circuit-only
# (`make -C bench run-equity`):
#   build/equity_holdem   circuit_equity_holdem
#   build/equity_omaha    circuit_equity_omaha 4/5/6 + omaha-4 hi-lo
#
# The conversion layer under all of the above has its own binary, no
# circuit evaluated (`make -C bench run-bspack`):
#   build/bspack          bs_transpose / bs_transpose_map /
#                         bs_untranspose in the three shapes the library
#                         packs (holdem input, omaha input, output) and
#                         bs_masks_from_cards in the arities the games
#                         use (bspack.cc).  MASKS= and BATCH= below
#                         rebuild the same rows on the alternate shipped
#                         cores.  Each row runs one conversion in a tight
#                         loop, so its text stays in the op cache and its
#                         size costs nothing here; in the library's own
#                         calls the same code shares the op cache (about
#                         600 64-byte windows of straight-line code on a
#                         Zen 5 core, the 32 KiB L1i behind it) with a
#                         13-29 KB circuit.  A row that improves while
#                         growing the text of bs_transpose or
#                         bs_untranspose therefore may not be an
#                         end-to-end gain.  Note that the Pipeline rows
#                         do not settle it either, nor do the mask-API
#                         rows in build/holdem and build/omaha: at these
#                         sizes they are sensitive to where the linker
#                         places the conversion code relative to the
#                         circuit, and swapping link order alone has moved
#                         the same two builds by 4-5% in both directions.
#                         Settling a footprint question needs placement
#                         sampled rather than fixed -- a dozen link orders
#                         or pad offsets per variant, variants interleaved
#                         within each, medians compared -- which no target
#                         here does.
#
# "Ours" is the library exactly as shipped: the checked-in generated C in
# lib/ through its public API (include/circuit_eval.h), compiled here with
# the library's own flags.  No python or BLIFs anywhere on this path.
#
# Every run target also takes PERF=1, which adds CYCLES/hand and
# INSTRUCTIONS/hand columns from the PMU; see the PERF block below for its
# three prerequisites and the COUNTERS knob.
#
# Switching ARCH (or BATCH, or MASKS, or PERF) means `make clean` first --
# different values cannot share a build directory -- so clean is deliberately
# cheap:
# it removes build/ and nothing else, which is seconds to rebuild.  The
# products that cost minutes are all arch-independent and live outside
# build/, so they survive: PHE's plo table objects (../artifacts/phe-tables,
# see below), the 2+2 HandRanks.dat, the google-benchmark static lib, and
# the two extracted plo5/6 table sources.  To throw those away:
#   make clean-tables    the plo table objects + the extracted plo5/6 C
#   make clean-all       clean + clean-tables + HandRanks.dat + gbench
B    := build
SRC  := ../lib
INC  := ../include
GB   := ../third-party/benchmark
# PERF=1 links a second, libpfm-enabled gbench; its own build dir so the two
# configurations never share a libbenchmark.a (see the PERF block below).
# Recursively expanded: report sets PERF below this line, and a := here
# would have resolved the suffix first.
GBB   = ../artifacts/gbench-build$(if $(PERF),-pfm)
OMP  := ../third-party/OMPEval
PHE  := ../third-party/PokerHandEvaluator/cpp
TP2  := ../third-party/TwoPlusTwoHandEvaluator
HANDRANKS   := ../artifacts/HandRanks.dat
TBL  := ../artifacts/phe-tables

CC       ?= cc
CXX      ?= c++

# ARCH pins -march for every measured object -- ours AND the other
# evaluators, so they all move as one (default: the host).
# A baseline value builds the corresponding code paths on a bigger
# host, e.g. ARCH=x86-64-v3 on an AVX-512 machine benchmarks the AVX2
# paths (the BS_BATCH probe below follows the knob, so the width lands
# at 256 on its own).  Same caveat as BATCH: different ARCH values
# cannot share a build directory, `make clean` between them.
ARCH ?= native

CXXFLAGS ?= -O3 -march=$(ARCH) -std=c++14 -w

# Batch width follows the compile target, exactly as in lib/Makefile:
# one set of checked-in circuits, compiled at 512 lanes on AVX-512 hosts,
# 128 on NEON and 256 elsewhere, with the value read back out of
# include/bsword.h so the rule lives in one file; BS_BATCH is pinned here so
# every object agrees.  (BATCH=<n> overrides, after a clean, same caveats
# as lib/Makefile.)
ifeq ($(origin BATCH), undefined)
BATCH := $(shell echo BS_BATCH \
           | $(CC) -march=$(ARCH) -include bsword.h -I$(INC) -E -P -x c - \
             2>/dev/null | grep -v '^[[:space:]]*$$' | tail -1)
endif
WFLAG := -DBS_BATCH=$(BATCH)

# MASKS=scalar forces the scalar bs_masks_from_cards core and
# MASKS=portable the generic vector one, exactly as in lib/Makefile.
# Same caveat: different MASKS values cannot share a build directory.
ifeq ($(MASKS),scalar)
WFLAG += -DBS_SCALAR_MASKS
endif
ifeq ($(MASKS),portable)
WFLAG += -DBS_PORTABLE_MASKS
endif

# PACK=portable forces the portable pack/unpack composition, exactly as
# in lib/Makefile (the intrinsics-vs-generic-vector-ops A/B).  Same
# caveat: different PACK values cannot share a build directory.
ifeq ($(PACK),portable)
WFLAG += -DBS_PORTABLE_PACK
endif

# The generated circuits take the same extra flags as in lib/Makefile
# (see the CIRCFLAGS comment there), probed the same way.
CIRCFLAGS := $(foreach f,$(if $(filter 512,$(BATCH)),-mno-red-zone,-mforce-drap) \
                         -frename-registers -flive-range-shrinkage,\
               $(shell $(CC) $(f) -Werror -c -x c /dev/null -o /dev/null \
                       2>/dev/null && echo $(f)))

# PERF=1 adds CYCLES/hand and INSTRUCTIONS/hand columns, read from the PMU.
# Three things are needed; PERF=1 arranges the first two:
#
#   libpfm4 headers + shared lib   apt install libpfm4-dev (or equivalent)
#   gbench -DBENCHMARK_ENABLE_LIBPFM=ON, in its own build directory
#                                  (../artifacts/gbench-build-pfm), -lpfm
#   perf_event_paranoid <= 2       sudo sysctl -w kernel.perf_event_paranoid=2
#                                  (the counters are user-mode only; Ubuntu
#                                  ships 4, which denies them)
#
# COUNTERS lists the events, in libpfm names; the suite rescales whatever is
# requested from gbench's per-iteration units to per hand (bench_perf.h):
#   make run PERF=1 COUNTERS=CYCLES,BRANCH-MISSES
#
# PERF and non-PERF binaries differ, so `make clean` between them, as with
# ARCH.  Only the binaries: every measured object is built the same either
# way.
#
# report sets PERF=1 for itself when libpfm4 is installed, probing for it
# rather than failing at the link where it is not.  PERF=1 or PERF= on the
# command line wins.  A too-strict perf_event_paranoid only warns -- gbench
# drops the columns and the run stands.
# (the \# is escaped for make, which would take the rest of the line as a
# comment)
PFMPROBE := printf '\#include <perfmon/pfmlib.h>\nint main(void){return 0;}\n'
PARANOID  = $(shell cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)
ifeq ($(origin PERF), undefined)
ifneq ($(filter report,$(MAKECMDGOALS)),)
PERF := $(shell $(PFMPROBE) | $(CC) -x c - -lpfm -o /dev/null 2>/dev/null \
                && echo 1)
ifneq ($(PERF),1)
$(info report: libpfm4 missing, no cycle columns -- apt install libpfm4-dev)
else ifeq ($(shell test "$(PARANOID)" -le 2 2>/dev/null && echo ok),)
$(info report: counters denied -- sysctl -w kernel.perf_event_paranoid=2)
endif
endif
endif

PERF     ?=
COUNTERS ?= CYCLES,INSTRUCTIONS
# gbench's FindPFM decides whether to compile its perf-counter code in, and
# probes for libpfm by static-linking libpfm.a.  Where the system libpfm.a
# carries an .sframe section the host ld will not read, that probe fails to
# link and gbench builds with the counter code stubbed out: the binary then
# prints "Performance counters not supported.", drops the columns and runs
# on.  Nothing on this path links the archive -- GBLINK below takes
# libpfm.so -- so the probe's result is pre-seeded and the three header
# checks behind it are what gate the feature.  A libpfm.so that is missing
# at the bench link is a hard error there, not a silent drop here.
ifeq ($(PERF),1)
GBCMAKE  := -DBENCHMARK_ENABLE_LIBPFM=ON -DHAVE_LIBPFM_INITIALIZE=1
GBLINK   := -lpfm
PERFARGS := --benchmark_perf_counters=$(COUNTERS)
endif

# what every bench binary links google-benchmark through
GBLIB = $(GBB)/src/libbenchmark.a $(GBLINK)

# Run-target knobs.  REPS sets --benchmark_repetitions; GBFLAGS appends
# extra google-benchmark flags; RUNPREFIX prefixes each binary invocation.
# On machines whose clock ramps slowly under load (some cloud guests),
# benchmarks late in a binary otherwise measure at a higher frequency than
# early ones -- spread each row's repetitions across the whole run and pin
# the process to one core.  That is what `report` below sets; any run
# target takes the same three by hand:
#   make run REPS=25 GBFLAGS=--benchmark_enable_random_interleaving=true \
#       RUNPREFIX='taskset -c 2'
REPS      ?= 3
GBFLAGS   ?=
RUNPREFIX ?=
# recursively expanded on purpose: report sets the three above per-target,
# and a := here would have baked in the defaults at parse time
GBARGS = --benchmark_repetitions=$(REPS) \
    --benchmark_report_aggregates_only=true \
    --benchmark_counters_tabular=true $(PERFARGS) $(GBFLAGS)

# PHE's 7-card oracle and its dependencies, compiled with our flags (their
# own CMake build sets no -march at all)
PHE7_SRC := $(PHE)/src/evaluator7.c $(PHE)/src/hashtable7.c \
            $(PHE)/src/dptables.c $(PHE)/src/tables_bitwise.c \
            $(PHE)/src/hash.c $(PHE)/src/hashtable.c

# PHE's dedicated plo4/5/6 evaluators.  Their shared helpers (choose, flush,
# hash_quinary, bit_of_div_4) already live in phe7.o, so these objects carry
# only the evaluators and their tables.
PLO_EVAL_OBJS  := $(B)/phe_plo_4.o $(B)/phe_plo_5.o $(B)/phe_plo_6.o
PLO_TABLE_OBJS := $(TBL)/tables_plo4.o $(TBL)/tables_plo5.o $(TBL)/tables_plo6.o

all: $(B)/holdem $(B)/omaha $(B)/holdem_enum $(B)/omaha_enum \
     $(B)/holdem_native $(B)/omaha_native $(B)/equity_holdem \
     $(B)/equity_omaha $(B)/bspack

$(B):
	mkdir -p $(B)

# google benchmark static lib
$(GBB)/src/libbenchmark.a:
	cmake -S $(GB) -B $(GBB) -DCMAKE_BUILD_TYPE=Release \
	    -DBENCHMARK_ENABLE_TESTING=OFF -DBENCHMARK_ENABLE_GTEST_TESTS=OFF \
	    $(GBCMAKE)
	cmake --build $(GBB) -j 8

# the circuit evaluators and their API layer, same sources and flags as
# the library build (lib/Makefile)
$(B)/circuit_%.o: $(SRC)/generated/circuit_%.c $(INC)/bsword.h | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) $(CIRCFLAGS) -I$(INC) -c -o $@ $<

$(B)/circuit_eval.o: $(SRC)/circuit_eval.c $(INC)/circuit_eval.h $(INC)/bsapi.h $(INC)/bsword.h | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) -Wall -I$(INC) -c -o $@ $(SRC)/circuit_eval.c

$(B)/bspack.o: $(SRC)/bspack/bspack.c $(wildcard $(SRC)/bspack/*.h) $(INC)/bsapi.h $(INC)/bsword.h | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) -Wall -I$(INC) -c -o $@ $(SRC)/bspack/bspack.c

$(B)/circuit_equity.o: $(SRC)/circuit_equity.c $(INC)/circuit_eval.h $(INC)/bsapi.h $(INC)/bsword.h | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) -Wall -I$(INC) -c -o $@ $(SRC)/circuit_equity.c

$(B)/phe7.o: $(PHE7_SRC) | $(B)
	for f in $(PHE7_SRC); do \
	  $(CC) -O3 -march=$(ARCH) -w -I$(PHE)/include -c $$f \
	      -o $(B)/phe7_$$(basename $$f .c).o || exit 1; done
	ld -r $(B)/phe7_*.o -o $@

# the plo5/6 tables ship as tarballs (332 and 655 MB of C once extracted);
# the touch is required: tar restores the tarball's stored mtime, which
# would leave the .c forever older than the tarball and re-run this rule
# (and the long table compile) on every make
$(PHE)/src/tables_plo5.c: $(PHE)/src/tables_plo5.tar.gz
	cd $(PHE)/src && tar xzf tables_plo5.tar.gz && touch tables_plo5.c

$(PHE)/src/tables_plo6.c: $(PHE)/src/tables_plo6.tar.gz
	cd $(PHE)/src && tar xzf tables_plo6.tar.gz && touch tables_plo6.c

# The table files are the one-time cost in this Makefile: pure data (giant
# array initializers -- 93, 332 and 655 MB of C), identical bytes at any -O
# and, since they carry no code, built with no -march at all.  So they are
# compiled once at -O1 into ../artifacts, shared by every ARCH/BATCH/MASKS
# combination and by both plo objects below, and left alone by clean --
# `make clean-tables` is what removes them.
$(TBL):
	mkdir -p $(TBL)

$(PLO_TABLE_OBJS): $(TBL)/tables_plo%.o: $(PHE)/src/tables_plo%.c | $(TBL)
	$(CC) -O1 -w -I$(PHE)/include -c $< -o $@

# the evaluators are ordinary code and do follow ARCH, so they stay in $(B)
$(PLO_EVAL_OBJS): $(B)/phe_plo_%.o: $(PHE)/src/evaluator_plo%.c | $(B)
	$(CC) -O3 -march=$(ARCH) -w -I$(PHE)/include -c $< -o $@

$(B)/phe_plo.o: $(PLO_EVAL_OBJS) $(PLO_TABLE_OBJS) | $(B)
	ld -r $(PLO_EVAL_OBJS) $(PLO_TABLE_OBJS) -o $@

# plo4 alone, for binaries that only need the 4-card game: its tables
# ship as plain C (no tarball extraction) and skipping plo5/6 keeps the
# link and the binary far smaller
$(B)/phe_plo4.o: $(B)/phe_plo_4.o $(TBL)/tables_plo4.o | $(B)
	ld -r $(B)/phe_plo_4.o $(TBL)/tables_plo4.o -o $@

$(B)/omp_eval.o: $(OMP)/omp/HandEvaluator.cpp | $(B)
	$(CXX) $(CXXFLAGS) -I$(OMP) -c -o $@ $<

# The 2+2 table generator plus upstream's smoke test. The submodule carries
# both already ported off Windows, so they build straight from it.
$(B)/gen2p2: $(TP2)/generate_table.cpp $(TP2)/pokerlib.cpp $(TP2)/mtrand.cpp | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $^

# the 2+2 table (124 MB, gitignored, a few minutes once per machine);
# gen2p2 writes ./HandRanks.dat in the cwd.  Order-only on the generator:
# gen2p2 lives in build/, so a normal prerequisite would regenerate the
# whole table after every `make clean`, which is exactly the arch-switch
# loop.  The .dat is a pure function of upstream's generator; if that ever
# changes, `make clean-all` (or just rm it).
$(HANDRANKS): | $(B)/gen2p2
	cd ../artifacts && ../bench/$(B)/gen2p2 > /dev/null && ls -l HandRanks.dat

# upstream's own smoke test: enumerates all C(52,7) and checks the ten
# category frequencies (reads ./HandRanks.dat from the cwd)
$(B)/test2p2: $(TP2)/test.cpp | $(B)
	$(CXX) $(CXXFLAGS) -include cstdio -include cstring -include cstdint -o $@ $<

check2p2: $(B)/test2p2 $(HANDRANKS)
	cd ../artifacts && ../bench/$(B)/test2p2

# circuit_eval.o carries all three entry points, so each binary links every
# circuit; the sizes table below still reports per-object
CIRC := $(B)/circuit_holdem.o $(B)/circuit_omaha.o $(B)/circuit_omaha_low.o \
        $(B)/circuit_eval.o $(B)/bspack.o

$(B)/holdem: holdem.cc bench_perf.h $(CIRC) $(B)/phe7.o \
             $(B)/omp_eval.o $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include -I$(OMP) -I$(PHE)/include \
	    -o $@ holdem.cc $(CIRC) $(B)/phe7.o \
	    $(B)/omp_eval.o $(GBLIB) -pthread

$(B)/omaha: omaha.cc bench_perf.h $(CIRC) $(B)/phe7.o $(B)/phe_plo.o \
            $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include -I$(PHE)/include \
	    -o $@ omaha.cc $(CIRC) \
	    $(B)/phe7.o $(B)/phe_plo.o $(GBLIB) -pthread

$(B)/holdem_enum: holdem_enum.cc bench_perf.h $(CIRC) $(B)/phe7.o \
                  $(B)/omp_eval.o $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include -I$(OMP) -I$(PHE)/include \
	    -o $@ holdem_enum.cc $(CIRC) $(B)/phe7.o \
	    $(B)/omp_eval.o $(GBLIB) -pthread

$(B)/omaha_enum: omaha_enum.cc bench_perf.h $(CIRC) $(B)/phe7.o \
                 $(B)/phe_plo4.o \
                 $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include -I$(PHE)/include \
	    -o $@ omaha_enum.cc $(CIRC) $(B)/phe7.o $(B)/phe_plo4.o \
	    $(GBLIB) -pthread

$(B)/holdem_native: holdem_native.cc bench_perf.h $(CIRC) $(B)/omp_eval.o \
                    $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include -I$(OMP) \
	    -o $@ holdem_native.cc $(CIRC) \
	    $(B)/omp_eval.o $(GBLIB) -pthread

$(B)/omaha_native: omaha_native.cc bench_perf.h $(CIRC) \
                   $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include \
	    -o $@ omaha_native.cc $(CIRC) $(GBLIB) -pthread

# the equity binaries add the comparators and the fused calls on top of
# the same circuit objects
EQCIRC := $(CIRC) $(B)/circuit_cmp8.o $(B)/circuit_cmp24.o \
          $(B)/circuit_equity.o

$(B)/equity_holdem: equity_holdem.cc bench_perf.h $(EQCIRC) \
                    $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include \
	    -o $@ equity_holdem.cc $(EQCIRC) $(GBLIB) -pthread

$(B)/equity_omaha: equity_omaha.cc bench_perf.h $(EQCIRC) \
                    $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include \
	    -o $@ equity_omaha.cc $(EQCIRC) $(GBLIB) -pthread

# the pack/unpack layer alone: bspack.o plus one circuit object, for the
# holdem card_input permutation and the output width (no circuit is run)
$(B)/bspack: bspack.cc bench_perf.h $(B)/bspack.o $(B)/circuit_holdem.o \
             $(GBB)/src/libbenchmark.a
	$(CXX) $(CXXFLAGS) $(WFLAG) -I$(INC) -I$(GB)/include \
	    -o $@ bspack.cc $(B)/bspack.o $(B)/circuit_holdem.o \
	    $(GBLIB) -pthread

run: $(B)/holdem $(B)/omaha $(HANDRANKS)
	$(RUNPREFIX) ./$(B)/holdem $(GBARGS)
	$(RUNPREFIX) ./$(B)/omaha $(GBARGS)
	sh sizes.sh $(B)

run-enum: $(B)/holdem_enum $(B)/omaha_enum $(HANDRANKS)
	$(RUNPREFIX) ./$(B)/holdem_enum $(GBARGS)
	$(RUNPREFIX) ./$(B)/omaha_enum $(GBARGS)

run-native: $(B)/holdem_native $(B)/omaha_native
	$(RUNPREFIX) ./$(B)/holdem_native $(GBARGS)
	$(RUNPREFIX) ./$(B)/omaha_native $(GBARGS)

run-equity: $(B)/equity_holdem $(B)/equity_omaha
	$(RUNPREFIX) ./$(B)/equity_holdem $(GBARGS)
	$(RUNPREFIX) ./$(B)/equity_omaha $(GBARGS)

run-bspack: $(B)/bspack
	$(RUNPREFIX) ./$(B)/bspack $(GBARGS)

# The batteries BENCH.md quotes, in one go and at settings worth quoting:
# 25 repetitions, random interleaving so a row's repetitions spread across
# the whole binary instead of bunching at one clock state, the process
# pinned to one core, and the PMU columns where libpfm4 is installed (PERF
# above).  Equity and native are deliberately out -- they are circuit-only
# side batteries, not cross-evaluator comparisons.  Any of the settings
# still overrides on the command line:
#   make report RUNPREFIX='taskset -c 3'
#   make report PERF=
report: REPS      := 25
report: GBFLAGS   := --benchmark_enable_random_interleaving=true
report: RUNPREFIX := taskset -c 1
report: preamble $(B)/holdem $(B)/omaha $(B)/holdem_enum $(B)/omaha_enum \
        $(B)/bspack $(HANDRANKS)
	$(RUNPREFIX) ./$(B)/holdem $(GBARGS)
	$(RUNPREFIX) ./$(B)/omaha $(GBARGS)
	$(RUNPREFIX) ./$(B)/holdem_enum $(GBARGS)
	$(RUNPREFIX) ./$(B)/omaha_enum $(GBARGS)
	$(RUNPREFIX) ./$(B)/bspack $(GBARGS)
	sh sizes.sh $(B)

# The OS, compilers and build knobs the numbers came from, printed ahead
# of everything else `report` runs.  A run whose objects are already
# built shows no compile lines at all, so without this the saved output
# records none of it.  /etc/issue is Linux; sw_vers is macOS; uname
# covers the rest.  The sed strips the getty escapes (\n \l) /etc/issue
# carries.  The knob line is the build side only: the run settings
# (repetitions, interleaving, counters) are on every $(GBARGS) line
# below it, and the pinning is on every RUNPREFIX.
preamble:
	@if [ -r /etc/issue ]; then \
	   sed -n 1p /etc/issue \
	   | sed -e 's/\\[a-zA-Z]//g' -e 's/[[:space:]]*$$//'; \
	 elif command -v sw_vers > /dev/null 2>&1; then \
	   echo "$$(sw_vers -productName) $$(sw_vers -productVersion)"; \
	 fi
	@uname -srm
	@$(CC) --version | sed -n 1p
	@$(CXX) --version | sed -n 1p
	@echo "ARCH=$(ARCH) BATCH=$(BATCH) MASKS=$(MASKS) PACK=$(PACK)"
	@echo "CIRCFLAGS=$(strip $(CIRCFLAGS))"
	@echo "CXXFLAGS=$(strip $(CXXFLAGS))"

# the non-timed benchmark: per-evaluator code/data footprint
sizes: $(B)/holdem $(B)/omaha
	sh sizes.sh $(B)

# build/ only: everything in here is arch-dependent and cheap.  See the
# note at the top of this file for what deliberately survives.
clean:
	rm -rf $(B)

# the arch-independent one-time products, in increasing order of regret
clean-tables:
	rm -rf $(TBL)
	rm -f $(PHE)/src/tables_plo5.c $(PHE)/src/tables_plo6.c

clean-all: clean clean-tables
	rm -f $(HANDRANKS)
	rm -rf ../artifacts/gbench-build ../artifacts/gbench-build-pfm

.PHONY: all run run-enum run-native run-equity run-bspack report preamble \
        sizes check2p2 clean clean-tables clean-all
