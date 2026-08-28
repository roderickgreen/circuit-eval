# Per-phase timing for the pack/unpack wiring in lib/bspack/, run
# against generated circuit kernels at either plane width.  Run from
# repo root, after touching the pack/unpack code:
#
#   make -C verify/pack             # per-phase transpose timings, holdem + omaha
#   make -C verify/pack BATCH=512   # the 512-lane layout
#
# BATCH pins -DBS_BATCH, so the wide layout builds anywhere: without
# AVX-512 the 512 build runs through split vector ops and the generic
# ladder (correctness, not speed); on AVX-512 silicon it times the
# GFNI/VBMI core.  Each width builds into its own dir, so no clean is
# needed between them.
#
# default = the netlists in flow/netlists/; pass CIRCUIT=.../OMAHA=... to time
# another netlist.  NOTE: circuit.c does not depend on the CIRCUIT
# *value*, so after changing CIRCUIT run `make clean` first.
CIRCUIT ?= ../../flow/netlists/holdem_value_1192.blif
OMAHA   ?= ../../flow/netlists/omaha_high_2324.blif
BATCH   ?= 256
# PACK=portable forces the portable pack/unpack composition (lib/Makefile
# has the full story); it gets its own build dir, so no clean is needed
BUILD   ?= build/$(BATCH)$(if $(filter portable,$(PACK)),-portable)
SRC     := ../../lib
INC     := ../../include
GEN     := ../../flow/codegen/cpu

CC      ?= cc
CFLAGS  ?= -O2 -march=native -Wall
CFLAGS  += -DBS_BATCH=$(BATCH)
ifeq ($(PACK),portable)
CFLAGS  += -DBS_PORTABLE_PACK
endif

# macOS ld64 refuses the undefined weak bs_* externs that ELF quietly
# resolves to NULL (bsglue.c / bench_pack.c probe them to detect card-less
# circuits); -U marks each allowed-undefined so they resolve to NULL there
# too.  Empty elsewhere.
ifeq ($(shell uname),Darwin)
WEAKU := -Wl,-U,_bs_eval -Wl,-U,_bs_num_outputs -Wl,-U,_bs_card_input
endif

# transpose API (bsapi.h): per-phase throughput, on the holdem
# and omaha circuits
bench_pack: $(BUILD)/bench_pack $(BUILD)/bench_pack_omaha
	$(BUILD)/bench_pack
	$(BUILD)/bench_pack_omaha

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bench_pack: bench_pack.c $(BUILD)/bspack.o $(BUILD)/bsglue.o \
                     $(BUILD)/circuit.o $(INC)/bsapi.h
	$(CC) $(CFLAGS) -I$(INC) -o $@ bench_pack.c $(BUILD)/bspack.o \
	  $(BUILD)/bsglue.o $(BUILD)/circuit.o

$(BUILD)/bench_pack_omaha: bench_pack.c $(BUILD)/bspack.o $(BUILD)/bsglue.o \
                           $(BUILD)/omaha_circuit.o $(INC)/bsapi.h
	$(CC) $(CFLAGS) -I$(INC) -o $@ bench_pack.c $(BUILD)/bspack.o \
	  $(BUILD)/bsglue.o $(BUILD)/omaha_circuit.o $(WEAKU)

# bspack.c lives in the library (lib/), bsapi.h in include/; bsglue.c (the link-time-
# bound bs_eval_hands wrappers the tools use) lives here.  Plain CFLAGS
# (-O2), matching the library build.
$(BUILD)/bspack.o: $(SRC)/bspack/bspack.c $(wildcard $(SRC)/bspack/*.h) \
                   $(INC)/bsapi.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(INC) -c -o $@ $(SRC)/bspack/bspack.c

$(BUILD)/bsglue.o: bsglue.c $(INC)/bsapi.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(INC) -c -o $@ bsglue.c

$(BUILD)/circuit.c: $(CIRCUIT) $(GEN)/bitslice.py ../../flow/codegen/blif.py | $(BUILD)
	python3 $(GEN)/bitslice.py $(CIRCUIT) $@

$(BUILD)/omaha_circuit.c: $(OMAHA) $(GEN)/bitslice.py ../../flow/codegen/blif.py | $(BUILD)
	python3 $(GEN)/bitslice.py $(OMAHA) $@

$(BUILD)/%.o: $(BUILD)/%.c $(INC)/bsword.h
	$(CC) $(CFLAGS) -I$(INC) -c -o $@ $<

clean:
	rm -rf build

.PHONY: bench_pack clean
