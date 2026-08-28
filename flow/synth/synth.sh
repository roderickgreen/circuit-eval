#!/bin/sh -e
# Behavioral Verilog specs -> normalized BLIFs + verification sequence.
# The recipes live in the top-level Makefile; this wrapper is kept as the
# documented entry point (restartloop.sh consumes its outputs).
# Run from repo root.
exec make verify
