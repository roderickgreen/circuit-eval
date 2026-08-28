#!/usr/bin/env python3
"""Restore PI/PO names that ABC's &deepsyn discarded (pi000/po00...),
mapping positionally from a name-source BLIF (order is preserved by ABC).

usage: renameio.py <renamed.blif> <name_source.blif> <out.blif>
"""
import re
import sys

from blif import parse

renamed, source, outp = sys.argv[1], sys.argv[2], sys.argv[3]

_, src_in, src_out, _ = parse(source)
_, ren_in, ren_out, _ = parse(renamed)
assert len(src_in) == len(ren_in) and len(src_out) == len(ren_out), \
    (len(src_in), len(ren_in), len(src_out), len(ren_out))

sub = dict(zip(ren_in, src_in))
sub.update(dict(zip(ren_out, src_out)))
if all(o == n for o, n in sub.items()):
    print("names already match; copying")

pat = re.compile(r"\b(" + "|".join(re.escape(k) for k in sub) + r")\b")
body = open(renamed).read()
open(outp, "w").write(pat.sub(lambda m: sub[m.group(1)], body))
print(f"renamed {len(sub)} ios -> {outp}")
