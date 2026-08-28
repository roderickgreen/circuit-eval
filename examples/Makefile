# Builds the examples against the library.  The three assignments are
# what a downstream Makefile needs; CIRCUIT_EVAL points at the checkout.
CIRCUIT_EVAL := ..
CFLAGS  += -O2 -march=native -I$(CIRCUIT_EVAL)/include
LDLIBS  += $(CIRCUIT_EVAL)/lib/build/libcircuiteval.a

all: holdem_equity omaha_equity omaha_equity_manual holdem_showdown omaha_showdown holdem_census

%: %.c $(LDLIBS)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(CIRCUIT_EVAL)/lib/build/libcircuiteval.a:
	$(MAKE) -C $(CIRCUIT_EVAL)/lib

clean:
	rm -f holdem_equity omaha_equity omaha_equity_manual holdem_showdown omaha_showdown holdem_census

.PHONY: all clean
