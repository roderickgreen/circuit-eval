# The evaluator library, built from the checked-in C in generated/ and
# bspack/.  Run from the repo root (make / make test) or from lib/:
#   make          build/libcircuiteval.a  (API: ../include/circuit_eval.h)
#   make test     build + run the selftest (known deals, exact values)
#   make regen    regenerate generated/circuit_*.c from the netlists in
#                 ../flow/netlists (needs python3)
B  := build
I  := ../include
CC ?= cc

# ARCH pins -march for every object (default: the host).  Setting it
# to a baseline builds the corresponding code paths on a bigger host --
# e.g. ARCH=x86-64-v3 on an AVX-512 machine compiles the AVX2 paths
# (no AVX-512 instructions or intrinsics anywhere, and the BS_BATCH
# probe below follows the knob, so the width lands at 256 on its own).
# Same caveat as BATCH: different ARCH values cannot share a build
# directory, `make clean` between them.
ARCH ?= native

# The batch width follows the compile target: 512 hands per call on
# AVX-512 hosts, 128 on NEON, 256 elsewhere.  ONE set of checked-in
# generated C serves every width -- the circuit files and bsapi.h share
# one BS_BATCH definition, bsword.h -- so the width is purely a
# compile-time parameter, pinned here so every object agrees.  The value
# is read back out of bsword.h under the target's own -march rather than
# restated here, so the rule lives in one file.  BATCH=<n> overrides it
# (after `make clean`; two widths cannot share a build directory):
# BATCH=256 on an AVX-512 host builds the narrow library for comparison,
# and a width the target has no register for runs through split vector
# ops -- correct, for testing, not fast.
ifeq ($(origin BATCH), undefined)
BATCH := $(shell echo BS_BATCH \
           | $(CC) -march=$(ARCH) -include bsword.h -I$(I) -E -P -x c - \
             2>/dev/null | grep -v '^[[:space:]]*$$' | tail -1)
endif
WFLAG := -DBS_BATCH=$(BATCH)

# MASKS picks a bs_masks_from_cards core other than the one the target
# would select.  scalar forces the scalar core even where a vector core
# exists (the vector-vs-scalar A/B, measurable on any machine); portable
# forces the generic vector core, which needs a per-element variable
# shift on the target to be worth using -- see
# bspack/masks_from_cards_portable.h.  Nothing else changes; the circuits
# keep their full vector width.  Same caveat as BATCH/ARCH: different
# MASKS values cannot share a build directory, `make clean` between
# them.
ifeq ($(MASKS),scalar)
WFLAG += -DBS_SCALAR_MASKS
endif
ifeq ($(MASKS),portable)
WFLAG += -DBS_PORTABLE_MASKS
endif

# PACK=portable forces the portable pack/unpack composition even where a
# fused intrinsics core exists, without touching -march: the
# intrinsics-vs-generic-vector-ops A/B, on any target.  Same caveat as
# BATCH/ARCH/MASKS: different PACK values cannot share a build
# directory, `make clean` between them.
ifeq ($(PACK),portable)
WFLAG += -DBS_PORTABLE_PACK
endif

# Extra flags for the generated circuit objects only, each kept when the
# compiler accepts it (a toolchain that rejects a flag builds without
# it; -mforce-drap is x86 gcc).  A circuit is one straight-line function
# whose ~200 live vectors spill to a 10-20 KB frame, so most of its
# instructions are spills, reloads and register copies, and their
# encoding is a large share of the object:
#   -mno-red-zone (BATCH=512)  lays the realigned frame out with the
#                         spill slots at 64-byte multiples from the stack
#                         pointer instead of 8 mod 64, which lets the EVEX
#                         encoder use the compressed 8-bit displacement for
#                         every slot within 8 KB of it (8-9 bytes per
#                         access instead of 11-12).  It changes no
#                         instruction, only their encoding, and it is worth
#                         nothing to a compiler that already aligns the
#                         slots or to a target without EVEX
#   -mforce-drap (other widths)  addresses the frame through a base
#                         register that needs no SIB byte, one byte per
#                         access; VEX has no scaled 8-bit displacement, so
#                         this is the encoding win available there
#   -frename-registers    renames allocated registers into ones left free,
#                         which lets the copy-propagation pass after it
#                         delete register copies it otherwise cannot
#   -flive-range-shrinkage  runs the scheduler before allocation with
#                         register pressure as its objective: fewer spills
#                         and copies
# Worth 12-15% of the bytes on the two large circuits at equal or better
# speed; the small ones do not spill and do not move.  Why bytes matter:
# the circuit plus the conversion code around it runs from the core's op
# cache, and a few KB decide whether the whole per-batch path fits in it.
#
# -Werror in the probe so a flag a compiler merely warns about is dropped
# rather than kept and warned about on every object.
CIRCFLAGS := $(foreach f,$(if $(filter 512,$(BATCH)),-mno-red-zone,-mforce-drap) \
                         -frename-registers -flive-range-shrinkage,\
               $(shell $(CC) $(f) -Werror -c -x c /dev/null -o /dev/null \
                       2>/dev/null && echo $(f)))

# One optimization level for everything.  The generated circuit code
# compiles to identical bytes at -O2 and -O3; bspack does not, and -O3
# costs it about 2 KB, most of it in bs_untranspose and
# bs_masks_from_cards, which every call executes.  -Os is much slower.
OBJS := $(B)/circuit_holdem.o $(B)/circuit_omaha.o \
        $(B)/circuit_omaha_low.o $(B)/circuit_cmp8.o $(B)/circuit_cmp24.o \
        $(B)/circuit_eval.o $(B)/circuit_equity.o $(B)/bspack.o

all: $(B)/libcircuiteval.a

$(B):
	mkdir -p $(B)

HDRS := $(I)/circuit_eval.h $(I)/bsapi.h $(I)/bsword.h

$(B)/circuit_%.o: generated/circuit_%.c $(I)/bsword.h | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) $(CIRCFLAGS) -I$(I) -c -o $@ $<

$(B)/circuit_eval.o: circuit_eval.c $(HDRS) | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) -Wall -I$(I) -c -o $@ circuit_eval.c

$(B)/circuit_equity.o: circuit_equity.c $(HDRS) | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) -Wall -I$(I) -c -o $@ circuit_equity.c

$(B)/bspack.o: bspack/bspack.c $(wildcard bspack/*.h) $(HDRS) | $(B)
	$(CC) -O2 -march=$(ARCH) $(WFLAG) -Wall -I$(I) -c -o $@ bspack/bspack.c

$(B)/libcircuiteval.a: $(OBJS)
	ar rcs $@ $^

test: $(B)/selftest
	./$(B)/selftest

$(B)/selftest: ../test/selftest.c $(I)/circuit_eval.h $(B)/libcircuiteval.a
	$(CC) -O2 -march=$(ARCH) $(WFLAG) -Wall -I$(I) -o $@ ../test/selftest.c \
	    $(B)/libcircuiteval.a

# Rebuild the checked-in circuit C from the netlists (one file per
# circuit; the width is compile-time, see above).  After swapping in a
# new netlist: update its BLIF name here, `make regen`, rebuild, rerun
# `make test` and the verification sequence.
GEN := ../flow/codegen/cpu/bitslice.py
N   := ../flow/netlists
regen:
	python3 $(GEN) --sym circuit_eval_holdem_raw \
	    $(N)/holdem_value_1192.blif generated/circuit_holdem.c
	python3 $(GEN) --sym circuit_eval_omaha_raw \
	    $(N)/omaha_high_2324.blif generated/circuit_omaha.c
	python3 $(GEN) --sym circuit_eval_omaha_low_raw \
	    $(N)/low_composed_181.blif generated/circuit_omaha_low.c
	python3 $(GEN) --sym circuit_cmp8_raw \
	    $(N)/cmp8_35.blif generated/circuit_cmp8.c
	python3 $(GEN) --sym circuit_cmp24_raw \
	    $(N)/cmp24_107.blif generated/circuit_cmp24.c

clean:
	rm -rf $(B)

.PHONY: all test regen clean
