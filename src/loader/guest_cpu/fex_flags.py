#!/usr/bin/env python3
"""Print the compile flags FEX-darwin uses for FEXCore sources, one per line.

The GuestCpu FEX backend (fex_guest_cpu.cpp) and FEX's AllocatorHooks.cpp must be compiled with the
same defines/includes as libFEXCore.a. They are taken from FEX's compile_commands.json.
"""
import json
import shlex
import sys

db = json.load(open(sys.argv[1]))
cmd = next(e["command"] for e in db if e["file"].endswith("Interface/Context/Context.cpp"))
toks = shlex.split(cmd)
keep = []
i = 1
while i < len(toks):
    t = toks[i]
    if t in ("-o", "-c", "-MD", "-MT", "-MF"):
        i += 2
        continue
    if t == "-include":
        keep += [t, toks[i + 1]]
        i += 2
        continue
    if t.startswith(("-D", "-I", "-f")):
        keep.append(t)
    i += 1
print(";".join(keep))
