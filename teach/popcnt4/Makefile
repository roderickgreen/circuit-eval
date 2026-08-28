# End-to-end teaching pipeline on a circuit small enough to read:
#
#   popcnt4_top.v --yosys--> popcnt4.aig --abc--> popcnt4.blif
#                 --bitslice64.py--> popcnt4.c --cc--> popcnt4_test
#
# Every stage uses the same recipe as the production evaluators (see the
# flow/Makefile); only the circuit and the C backend are teaching-sized.
#
#   make        build everything and run the exhaustive test
#   make clean  remove all generated files
#
# Run from this directory.

ROOT  := ../..
YOSYS ?= $(ROOT)/third-party/yosys/build/yosys
ABC   := $(ROOT)/third-party/abc/abc
V     := $(ROOT)/flow/spec

# Stage 1 (yosys): elaborate the behavioral Verilog and lower it to an
# AIG -- a sea of 2-input ANDs and inverters, the canonical form ABC
# optimizes. Same minimal pass list as production: yosys only translates,
# ABC does the optimizing.
PASSES := proc; flatten; opt_expr; opt_clean; memory; techmap; \
          opt_expr; opt_clean; setundef -zero; aigmap

# Stage 2 (ABC): minimize and map to 2-input gates. dch synthesizes
# don't-care-aware AIG restructurings; "if -K 2" maps the result onto
# 2-input LUTs, which is exactly the gate library bitsliced C can execute
# (one bitwise operator per gate). Two rounds, as in production.
NORM := st; dch; if -K 2; st; dch; if -K 2; print_stats

all: run

popcnt4.aig: popcnt4_top.v $(V)/lib.vh
	$(YOSYS) -q -p "read_verilog -I$(V) popcnt4_top.v; \
	  hierarchy -top popcnt4_top; $(PASSES); write_aiger -symbols $@"

popcnt4.blif: popcnt4.aig
	$(ABC) -c "read_aiger $<; $(NORM); write_blif $@"

# Stage 3: BLIF -> straight-line C over uint64_t, one statement per gate.
popcnt4.c: popcnt4.blif $(ROOT)/flow/codegen/cpu/bitslice64.py $(ROOT)/flow/codegen/blif.py
	python3 $(ROOT)/flow/codegen/cpu/bitslice64.py $< $@ --name popcnt4

# Stage 4: compile with the pack/unpack wrappers and the exhaustive test
# driver, and run it.
popcnt4_test: main.c pack.c pack.h popcnt4.c
	cc -O2 -Wall -Wextra -o $@ main.c pack.c popcnt4.c

run: popcnt4_test
	./popcnt4_test

clean:
	rm -f popcnt4.aig popcnt4.blif popcnt4.c popcnt4_test

.PHONY: all run clean
