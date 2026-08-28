# The synthesis flow that produced the netlists in netlists/: behavioral
# Verilog specs (spec/) -> yosys AIG -> ABC-normalized 2-input BLIF, plus
# the verification sequence (../verify).  Run from this directory or via
# the root Makefile, which forwards these targets.
#
#   make blifs        build every spec AIG/BLIF that is out of date
#   make check        fast parse/elaborate of all specs (edit loop)
#   make verify       build blifs + run the full verification sequence
#   make holdem       build one top (rank, flush, holdem, low, omaha)
#   make verify-holdem  build + verify one top
#   make -j           tops build in parallel
#
# synth/synth.sh wraps `make verify`; synth/restartloop.sh consumes the
# .aig files and low_core_start.blif produced here, names unchanged.

TP    := ../third-party
YOSYS ?= $(TP)/yosys/build/yosys
ABC   := $(TP)/abc/abc
V     := spec
A     := build

# Minimal yosys pass list everywhere: yosys's `synth` optimizer is both slow
# and (on omaha) a >64 GB RSS blowup, and ABC's dch + if -K 2 renormalizes
# the AIG anyway. setundef clears x bits from memory lowering (no real path
# reads them -- the verification sequence covers this). The final
# opt_clean -purge drops the flattened submodule port wires that alias the
# top-level inputs; write_aiger otherwise reports each alias as an undriven
# bit, though the AIG it writes is the same.
PASSES := proc; flatten; opt_expr; opt_clean; memory; techmap; opt_expr; opt_clean; setundef -zero; aigmap; opt_clean -purge

# ABC renormalization to 2-input BLIF
NORM := st; dch; if -K 2; st; dch; if -K 2; print_stats

# ABC stamps a dated "# Benchmark" comment on every BLIF it writes; strip
# it so an unchanged circuit rebuilds byte-identical (cmp-able artifacts).
STRIP_STAMP := sed -i '1{/^\# Benchmark/d}'

LIB       := $(V)/lib.vh
WRAP      := $(A)/io_wrappers.v
VALUE_SRC := $(V)/rank_value.v $(V)/flush_value.v $(V)/holdem_value.v \
             $(V)/suit_view.v
ALL_SRC   := $(VALUE_SRC) $(V)/low_core.v $(V)/omaha_value.v \
             $(V)/cmp8.v $(V)/cmp24.v

BLIFS := $(A)/rank_value_io.blif $(A)/flush_value_io.blif \
         $(A)/holdem_value_io.blif $(A)/low_core_start.blif \
         $(A)/omaha_value_io.blif $(A)/cmp8_io.blif $(A)/cmp24_io.blif

.PHONY: all blifs check verify clean help \
        rank flush holdem low omaha cmp8 cmp24 \
        verify-rank verify-flush verify-holdem verify-low verify-omaha \
        verify-cmp8 verify-cmp24
.SECONDARY:            # keep .aig intermediates (restartloop.sh reads them)

all: blifs

blifs: $(BLIFS)

check: $(WRAP)
	$(YOSYS) -q -p "read_verilog -sv $(ALL_SRC) $(WRAP); proc"
	@echo "check: all specs parse and elaborate"

help:
	@sed -n '2,13p' Makefile | sed 's/^# \{0,1\}//'

# Convenience names per top
rank:  $(A)/rank_value_io.blif
flush: $(A)/flush_value_io.blif
holdem: $(A)/holdem_value_io.blif
low:   $(A)/low_core_start.blif
omaha: $(A)/omaha_value_io.blif
cmp8:  $(A)/cmp8_io.blif
cmp24: $(A)/cmp24_io.blif

$(WRAP): synth/mkwrappers.py | $(A)
	python3 $< $@

$(A):
	mkdir -p $@

# rank/flush/holdem elaborate from the same sources (holdem instantiates
# the rank and flush cores plus the shared suit_view rewiring)
$(A)/rank_value_io.aig $(A)/flush_value_io.aig $(A)/holdem_value_io.aig: \
    $(A)/%_io.aig: $(VALUE_SRC) $(LIB) $(WRAP) | $(A)
	$(YOSYS) -q -p "read_verilog -sv $(VALUE_SRC) $(WRAP); \
	  hierarchy -top $*_io; $(PASSES); write_aiger -symbols $@"

# omaha also instantiates suit_view
$(A)/omaha_value_io.aig: $(V)/omaha_value.v $(V)/suit_view.v $(LIB) $(WRAP) | $(A)
	$(YOSYS) -q -p "read_verilog -sv $(V)/omaha_value.v $(V)/suit_view.v $(WRAP); \
	  hierarchy -top omaha_value_io; $(PASSES); write_aiger -symbols $@"

$(A)/%_io.aig: $(V)/%.v $(LIB) $(WRAP) | $(A)
	$(YOSYS) -q -p "read_verilog -sv $(V)/$*.v $(WRAP); \
	  hierarchy -top $*_io; $(PASSES); write_aiger -symbols $@"

$(A)/%_io.blif: $(A)/%_io.aig
	$(ABC) -c "read_aiger $<; $(NORM); write_blif $@"
	$(STRIP_STAMP) $@

# comparators: additionally pin the .inputs list to natural-sorted order
# (a0..aN b0..bN).  yosys numbers AIG inputs by internal creation order,
# which for these tops comes out b-first; positional consumers (deepsyn
# start -> renameio, the bitslice/WGSL codegens) all read this BLIF, so
# the order is fixed once here and everything downstream agrees.
$(A)/cmp8_io.blif $(A)/cmp24_io.blif: $(A)/%_io.blif: $(A)/%_io.aig \
    codegen/orderio.py
	$(ABC) -c "read_aiger $<; $(NORM); write_blif $@"
	python3 codegen/orderio.py $@ $@
	$(STRIP_STAMP) $@

# low core: small support (16 PIs), so write the collapse-first start
# (collapse re-canonicalizes the cover; skipping it strands deepsyn ~3x
# higher). restartloop.sh deepsyns the low core from the _start.blif, not
# the raw AIG.
# (cd: the compress2rs alias only loads from abc.rc's own directory, and an
# unknown command makes abc silently write nothing)
$(A)/low_core_start.blif: $(A)/low_core_io.aig
	cd $(TP)/abc && ./abc -c "read_aiger $(CURDIR)/$<; st; \
	  collapse; sop; fx; st; dc2; compress2rs; compress2rs; dc2; st; dch; \
	  if -K 2; print_stats; write_blif $(CURDIR)/$@"
	$(STRIP_STAMP) $@

# Verification sequence.  The harnesses read the ground-truth tables in
# ../artifacts and the netlists by repo-root-relative paths, so they run
# from the repo root.
R  := cd .. &&
F  := flow

verify: verify-rank verify-flush verify-holdem verify-low verify-omaha \
        verify-cmp8 verify-cmp24

verify-rank: $(A)/rank_value_io.blif
	$(R) python3 verify/spec/verify_value.py rank $(F)/$<

verify-flush: $(A)/flush_value_io.blif
	$(R) python3 verify/spec/verify_value.py flush $(F)/$<

# exhaustive at every arity the circuit serves (all C(52,5)+C(52,6)+C(52,7)
# hands vs PHE, ordinal + exact-through-the-map): ~26 s, and the slowest check
# in the sequence by far. verify/spec/crosscheck.py is the sampled spot
# check against the committed netlist, kept for the spec edit loop; this
# is the gate.
verify-holdem: $(A)/holdem_value_io.blif
	$(R) sh verify/holdem/verify_holdem.sh $(F)/$<

verify-low: $(A)/low_core_start.blif
	$(R) python3 verify/omaha/compose_low.py $(F)/$< $(F)/$(A)/low_composed_check.blif
	$(R) python3 verify/omaha/verify_low.py $(F)/$(A)/low_composed_check.blif 5000

verify-omaha: $(A)/omaha_value_io.blif
	$(R) for nh in 4 5 6; do \
	  python3 verify/omaha/verify_omaha.py $(F)/$< 20000 $$nh || exit 1; done

verify-cmp8: $(A)/cmp8_io.blif
	$(R) python3 verify/holdem/verify_cmp.py $(F)/$< 8

verify-cmp24: $(A)/cmp24_io.blif
	$(R) python3 verify/holdem/verify_cmp.py $(F)/$< 24

clean:
	rm -f $(A)/*.aig $(A)/*.blif $(WRAP)
