# `make` builds the evaluator library from the C checked into lib/.
# flow/ holds the synthesis workflow that produced those circuits and
# verify/ the checks it is held to.  The synthesis targets below forward
# to flow/Makefile.
#
#   make              build lib/build/libcircuiteval.a (API: include/circuit_eval.h)
#   make test         build + run the library selftest
#   make examples     build examples/
#   make bench        build + run the benchmarks (bench/)
#   make blifs        build every spec AIG/BLIF that is out of date
#   make check        fast parse/elaborate of all specs (edit loop)
#   make verify       build blifs + run the full verification sequence
#   make holdem       build one top (rank, flush, holdem, low, omaha)
#   make verify-holdem  build + verify one top
#   make -j           tops build in parallel
#   make toolchain    fetch and build third-party/abc and third-party/yosys

FLOW := blifs check verify rank flush holdem low omaha cmp8 cmp24 \
        verify-rank verify-flush verify-holdem verify-low verify-omaha \
        verify-cmp8 verify-cmp24

.PHONY: all lib test examples bench toolchain clean help $(FLOW)

all: lib

lib:
	$(MAKE) -C lib

test:
	$(MAKE) -C lib test

examples: lib
	$(MAKE) -C examples

bench:
	$(MAKE) -C bench run

$(FLOW):
	$(MAKE) -C flow $@

# The synthesis toolchain, built in place from the submodules.  A
# submodule is fetched when a file it must contain is missing.  flow/
# and teach/ default to these two binary paths.
ABC   := third-party/abc/abc
YOSYS := third-party/yosys/build/yosys

toolchain: $(ABC) $(YOSYS)

third-party/abc/Makefile:
	git submodule update --init third-party/abc

third-party/yosys/CMakeLists.txt:
	git submodule update --init --recursive third-party/yosys

$(ABC): third-party/abc/Makefile
	$(MAKE) -C third-party/abc -j "$(shell nproc)" ABC_USE_NO_READLINE=1

$(YOSYS): third-party/yosys/CMakeLists.txt $(ABC)
	cmake -S third-party/yosys -B third-party/yosys/build \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DYOSYS_ABC_EXECUTABLE="$(CURDIR)/$(ABC)"
	cmake --build third-party/yosys/build --parallel "$(shell nproc)"

help:
	@sed -n '6,16p' Makefile | sed 's/^# \{0,1\}//'

# Removes build products only.  The benchmark tables (PHE's plo table
# objects, the 2+2 HandRanks.dat, the google-benchmark lib) take minutes
# to build and no compile flag changes them, so they are left in place.
# `make -C bench clean-tables` and `make -C bench clean-all` remove them.
clean:
	$(MAKE) -C lib clean
	$(MAKE) -C flow clean
	$(MAKE) -C examples clean
	$(MAKE) -C bench clean
