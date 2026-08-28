# WebGPU compute kernels generated from the committed circuits.
#
# make                   generate every demo kernel into build/kernels/
# make validate-kernels  vet the staged set (see below)
# make install           validate-kernels, then copy into demo/src/kernels/
#
# Needs: python3, deno (set DENO=...), and a Vulkan ICD (RADV for the real
# GPU, lavapipe works for validation).
#
# Generators, each a BLIF -> WGSL compiler over the front end in ../blif.py:
#   wgsl.py            hand value kernels: one circuit evaluation per lane
#   equity_wgsl.py     equity, deal-major: one villain draw x 32 boards per
#                      invocation
#   equity_bm_wgsl.py  equity, board-major: one board per invocation,
#                      villains across the lanes
#   equity_xc_wgsl.py  equity, cross-compare: each hand evaluated once per
#                      board, hero x villain pairings compared scalar-vs-plane

B := build
VF := ../../../verify

# the committed circuits everything here is generated from
HOLDEM_HIGH := ../../netlists/holdem_value_1192.blif
OMAHA_HIGH  := ../../netlists/omaha_high_2324.blif
OMAHA_LOW   := ../../netlists/low_composed_181.blif

DENO ?= deno

all: kernels

# ---- demo kernels ---------------------------------------------------------
#
# The demo page's shaders: generated from the committed circuits
# into build/kernels/, validated there, and only then copied into
# demo/src/kernels/ to be committed. demo/ generates nothing itself -- the
# page runs whatever `make install` last put there, and verify/webgpu/
# holds the harnesses (they import demo/src/lib directly, so what gets vetted is the
# code the page actually runs, pointed at the staged kernels via KERNELS).
#
# validate-kernels runs the correctness sequence, all of it headless: the
# benchmark pool generators against the C harness's definitions (no GPU),
# every equity kernel against the from-the-rules JS reference, and the
# value kernels' entry point (main_eval) pairwise against the same reference
# (lavapipe is fine; pass --full on a real adapter for the preflop spots).
# The perf probes stay manual -- they answer perf questions on real
# hardware, not "is this generation correct".

K    := $(B)/kernels
DEMO := ../../../demo

KERNELS := holdem_value_l1 omaha_high_l1 omaha_low_l1 \
           equity_holdem equity_holdem_bm equity_holdem_xc \
           equity_omaha equity_omaha_bm equity_omaha_xc \
           equity_omaha_hilo equity_omaha_hilo_bm equity_omaha_hilo_xc
KFILES := $(KERNELS:%=$(K)/%.wgsl)

kernels: $(KFILES)

$(K):
	mkdir -p $(K)

VAL_DEPS := wgsl.py ../blif.py
EQ_DEPS  := equity_wgsl.py $(VAL_DEPS)

$(K)/holdem_value_l1.wgsl: $(HOLDEM_HIGH) $(VAL_DEPS) | $(K)
	python3 wgsl.py $(HOLDEM_HIGH) $@ --lanes 1
$(K)/omaha_high_l1.wgsl: $(OMAHA_HIGH) $(VAL_DEPS) | $(K)
	python3 wgsl.py $(OMAHA_HIGH) $@ --lanes 1
$(K)/omaha_low_l1.wgsl: $(OMAHA_LOW) $(VAL_DEPS) | $(K)
	python3 wgsl.py $(OMAHA_LOW) $@ --lanes 1

$(K)/equity_holdem.wgsl: $(HOLDEM_HIGH) $(EQ_DEPS) | $(K)
	python3 equity_wgsl.py --style holdem --high $(HOLDEM_HIGH) $@
$(K)/equity_omaha.wgsl: $(OMAHA_HIGH) $(EQ_DEPS) | $(K)
	python3 equity_wgsl.py --style omaha --high $(OMAHA_HIGH) $@
$(K)/equity_omaha_hilo.wgsl: $(OMAHA_HIGH) $(OMAHA_LOW) $(EQ_DEPS) | $(K)
	python3 equity_wgsl.py --style omaha --high $(OMAHA_HIGH) --low $(OMAHA_LOW) $@

$(K)/equity_holdem_bm.wgsl: $(HOLDEM_HIGH) equity_bm_wgsl.py $(EQ_DEPS) | $(K)
	python3 equity_bm_wgsl.py --style holdem --high $(HOLDEM_HIGH) $@
$(K)/equity_omaha_bm.wgsl: $(OMAHA_HIGH) equity_bm_wgsl.py $(EQ_DEPS) | $(K)
	python3 equity_bm_wgsl.py --style omaha --high $(OMAHA_HIGH) $@
$(K)/equity_omaha_hilo_bm.wgsl: $(OMAHA_HIGH) $(OMAHA_LOW) equity_bm_wgsl.py $(EQ_DEPS) | $(K)
	python3 equity_bm_wgsl.py --style omaha --high $(OMAHA_HIGH) --low $(OMAHA_LOW) $@

$(K)/equity_holdem_xc.wgsl: $(HOLDEM_HIGH) equity_xc_wgsl.py $(EQ_DEPS) | $(K)
	python3 equity_xc_wgsl.py --style holdem --high $(HOLDEM_HIGH) $@
$(K)/equity_omaha_xc.wgsl: $(OMAHA_HIGH) equity_xc_wgsl.py $(EQ_DEPS) | $(K)
	python3 equity_xc_wgsl.py --style omaha --high $(OMAHA_HIGH) $@
$(K)/equity_omaha_hilo_xc.wgsl: $(OMAHA_HIGH) $(OMAHA_LOW) equity_xc_wgsl.py $(EQ_DEPS) | $(K)
	python3 equity_xc_wgsl.py --style omaha --high $(OMAHA_HIGH) --low $(OMAHA_LOW) $@

validate-kernels: kernels
	$(DENO) run --allow-read $(VF)/webgpu/bench_pool_test.js
	KERNELS=$(K) $(DENO) run --allow-read --allow-env=KERNELS \
	  --unstable-webgpu $(VF)/webgpu/equity_test.js
	KERNELS=$(K) $(DENO) run --allow-read --allow-env=KERNELS \
	  --unstable-webgpu $(VF)/webgpu/bench_eval_test.js

install: validate-kernels
	cp $(KFILES) $(DEMO)/src/kernels/

clean:
	rm -rf $(B)

.PHONY: all kernels validate-kernels install clean
