# The verification harnesses and the oracle objects they link.  Run from
# this directory or the repo root (`make -C verify`).  What each gate
# proves, and against which oracle, is in README.md here.
#
#   build/verify_holdem   holdem circuit vs the circuit-free stamp oracle
#   build/omporacle5      the stamp oracle: OMPEval + the encoding spec, no circuit
#   build/verify_nk       holdem circuit on 5/6-card hands, ordinal vs PHE
#   build/dump_valmap     value->rank table from the live circuit + PHE
#   build/crosscheck      OMPEval vs PHE agreement
#   build/fullsweep       exhaustive C(52,7) check of the mkartifacts.py tables vs PHE
#   build/verify_omaha, verify_omaha_lo, domain_omaha
#                         omaha fold gates and the domain-constant tool
#   build/pheoracle_omaha
#                         omaha stamp oracle: PHE's plo evaluators + the encoding spec, no circuit
#   build/verify_omaha_hilo
#                         both omaha sides in one sweep, same references
LIB := ../lib
INC := ../include
OMP := ../third-party/OMPEval
PHE := ../third-party/PokerHandEvaluator/cpp
PHE_SRC := $(PHE)/src/evaluator7.c $(PHE)/src/hashtable7.c \
           $(PHE)/src/dptables.c $(PHE)/src/tables_bitwise.c \
           $(PHE)/src/hash.c $(PHE)/src/hashtable.c \
           $(PHE)/src/evaluator5.c $(PHE)/src/hashtable5.c \
           $(PHE)/src/evaluator_plo4.c $(PHE)/src/tables_plo4.c
B   := build
TBL := ../artifacts/phe-tables
H   := holdem
O   := omaha

CC       ?= cc
CXX      ?= c++
CXXFLAGS ?= -O3 -march=native -std=c++14 -w
# The PHE-side harnesses generate and link 256-lane circuits
# (bitslice.py); pin the plane width so builds on hosts where bsword.h
# defaults to something else stay consistent.
BS       := -DBS_BATCH=256
CXXFLAGS += $(BS)

# Circuit objects that link beside another circuit get their bs_* symbols
# renamed at compile time (-D on the generated C), not with objcopy
# --redefine-sym: identical result, and a stock macOS toolchain has no
# objcopy.
RENAME_HOLDEM := -Dbs_eval=bs_eval_holdem -Dbs_card_input=bs_card_input_holdem \
    -Dbs_num_outputs=bs_num_outputs_holdem -Dbs_num_inputs=bs_num_inputs_holdem \
    -Dbs_nb=bs_nb_holdem

all: $(B)/crosscheck $(B)/fullsweep $(B)/verify_nk $(B)/dump_valmap \
     $(B)/verify_holdem $(B)/omporacle5 \
     $(B)/verify_omaha $(B)/verify_omaha_lo $(B)/verify_omaha_hilo \
     $(B)/domain_omaha $(B)/pheoracle_omaha

$(B):
	mkdir -p $(B)

$(LIB)/build/libcircuiteval.a:
	$(MAKE) -C $(LIB)

# ---- holdem ---------------------------------------------------------------

$(B)/phe.o: $(PHE_SRC) | $(B)
	for f in $(PHE_SRC); do $(CC) -O3 -march=native -w -I$(PHE)/include -c $$f -o $(B)/phe_$$(basename $$f .c).o; done
	ld -r $(B)/phe_*.o -o $@

# evaluate_6cards lives in its own translation unit + table, not pulled into
# phe.o (which carries the 5- and 7-card oracles); verify_nk needs all three
$(B)/phe6.o: $(PHE)/src/evaluator6.c $(PHE)/src/hashtable6.c | $(B)
	$(CC) -O3 -march=native -w -I$(PHE)/include -c $(PHE)/src/evaluator6.c -o $(B)/phe6_evaluator6.o
	$(CC) -O3 -march=native -w -I$(PHE)/include -c $(PHE)/src/hashtable6.c -o $(B)/phe6_hashtable6.o
	ld -r $(B)/phe6_evaluator6.o $(B)/phe6_hashtable6.o -o $@

# holdem circuit correctness on 5/6-card hands, ordinal vs PHE's native
# evaluate_5cards/evaluate_6cards (nh=7 self-checks the harness)
$(B)/verify_nk: $(H)/verify_nk.cpp $(B)/circuit_holdem.o $(B)/phe.o $(B)/phe6.o
	$(CXX) $(CXXFLAGS) -I$(PHE)/include -o $@ $^

# the 7462-row value->rank table dumped from the live circuit + PHE; the
# battery diffs it against ../flow/encoding/mkspec.py (the encoding
# contract)
$(B)/dump_valmap: $(H)/dump_valmap.cpp $(B)/circuit_holdem.o $(B)/phe.o
	$(CXX) $(CXXFLAGS) -I$(PHE)/include -o $@ $^

$(B)/crosscheck: $(H)/crosscheck.cpp $(OMP)/omp/HandEvaluator.cpp $(B)/phe.o | $(B)
	$(CXX) $(CXXFLAGS) -I$(OMP) -I$(PHE)/include -o $@ $^

# exhaustive C(52,7) validation of the mkartifacts.py tables vs PHE
$(B)/fullsweep: $(H)/fullsweep.cpp $(B)/phe.o | $(B)
	$(CXX) $(CXXFLAGS) -I$(PHE)/include -o $@ $^ -pthread

HOLDEM_BLIF := ../flow/netlists/holdem_value_1192.blif
GEN := ../flow/codegen/cpu/bitslice.py

$(B)/circuit_holdem.o: $(HOLDEM_BLIF) $(GEN) ../flow/codegen/blif.py | $(B)
	python3 $(GEN) $(HOLDEM_BLIF) $(B)/circuit_holdem.c
	$(CC) -O2 -march=native $(BS) $(RENAME_HOLDEM) -I$(INC) -c -o $@ $(B)/circuit_holdem.c

# the deployed library vs the circuit-free stamp oracle (see
# holdem/verify_holdem.cpp for the argument)
$(B)/verify_holdem: $(H)/verify_holdem.cpp $(LIB)/build/libcircuiteval.a | $(B)
	$(CXX) -O2 -march=native -std=c++14 -Wall -I$(INC) -pthread \
	    -o $@ $(H)/verify_holdem.cpp $(LIB)/build/libcircuiteval.a

# the standalone stamp oracle: OMPEval + the encoding spec, and on
# purpose NOT the circuit library -- see holdem/omporacle5.cpp
$(B)/omporacle5: $(H)/omporacle5.cpp $(H)/valmap.h $(OMP)/omp/HandEvaluator.cpp | $(B)
	$(CXX) -O2 -march=native -std=c++14 -w -I$(OMP) -o $@ \
	    $(H)/omporacle5.cpp $(OMP)/omp/HandEvaluator.cpp

# ---- omaha ----------------------------------------------------------------

# the deployed library held to the anchored 5-card values over every
# configuration per hole count (see omaha/verify_omaha.c); the same
# rule builds verify_omaha_lo and the merged verify_omaha_hilo
$(B)/verify_%: $(O)/verify_%.c $(LIB)/build/libcircuiteval.a | $(B)
	$(CC) -O2 -march=native -Wall -I$(INC) -pthread -o $@ \
	    $< $(LIB)/build/libcircuiteval.a

# domain constants only: no evaluator, so no circuit library
$(B)/domain_omaha: $(O)/domain_omaha.c | $(B)
	$(CC) -O2 -march=native -Wall -pthread -o $@ $<

# the standalone omaha stamp oracle: PHE's plo4/5/6 evaluators + the
# encoding spec, and on purpose NOT the circuit library -- see
# omaha/pheoracle_omaha.c.  phe.o carries the 5-card and plo4 code;
# the plo5/6 tables ship as tarballs (332 and 655 MB of C once
# extracted) and compile to objects shared with bench/ under $(TBL),
# built once at -O1 (pure data, no -march) and left alone by clean.
# The touch is required: tar restores the tarball's stored mtime, which
# would leave the .c forever older than the tarball.
$(PHE)/src/tables_plo%.c: $(PHE)/src/tables_plo%.tar.gz
	cd $(PHE)/src && tar xzf tables_plo$*.tar.gz && touch tables_plo$*.c

$(TBL):
	mkdir -p $(TBL)

$(TBL)/tables_plo%.o: $(PHE)/src/tables_plo%.c | $(TBL)
	$(CC) -O1 -w -I$(PHE)/include -c $< -o $@

$(B)/phe_plo56.o: $(PHE)/src/evaluator_plo5.c $(PHE)/src/evaluator_plo6.c \
                  $(TBL)/tables_plo5.o $(TBL)/tables_plo6.o | $(B)
	$(CC) -O3 -march=native -w -I$(PHE)/include -c $(PHE)/src/evaluator_plo5.c -o $(B)/phe_plo5.o
	$(CC) -O3 -march=native -w -I$(PHE)/include -c $(PHE)/src/evaluator_plo6.c -o $(B)/phe_plo6.o
	ld -r $(B)/phe_plo5.o $(B)/phe_plo6.o $(TBL)/tables_plo5.o $(TBL)/tables_plo6.o -o $@

$(B)/pheoracle_omaha: $(O)/pheoracle_omaha.c $(H)/valmap.h $(B)/phe.o $(B)/phe_plo56.o | $(B)
	$(CC) -O2 -march=native -Wall -I$(PHE)/include -pthread -o $@ \
	    $(O)/pheoracle_omaha.c $(B)/phe.o $(B)/phe_plo56.o

clean:
	rm -rf $(B)

.PHONY: all clean
