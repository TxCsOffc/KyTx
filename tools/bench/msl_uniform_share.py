#!/usr/bin/env python3 -I
"""How many statements of a pair-lowered (wave64 -> two halves) MSL kernel compute wave-uniform values?

A statement whose result does not depend on the lane id (kyty_gl0/kyty_gl1/lane/tid/gid/liid, transitively through registers) has the same
value in both halves; the emitter still writes it twice (vN and vN_h). This is a conservative static estimate (no control dependence: a value
assigned under a divergent branch is counted as uniform if its operands are), so it is an upper bound of what a 'uniform value is shared by
both halves' optimisation could remove.

usage: python3 -I tools/bench/msl_uniform_share.py file.metal [...]
"""
import re
import sys

LANE_SEEDS = {"kyty_gl0", "kyty_gl1", "lane", "tid", "gid", "liid"}
ASSIGN = re.compile(r"^\s*(?:const\s+\w+\s+)?(\w+)\s*=\s*(.*);\s*$")
IDENT = re.compile(r"[A-Za-z_]\w*")
DECL = re.compile(r"^\t\w+\s+\w+\s*=\s*\(\w+\)\s*0;\s*$")


def base(n):
    return n[:-2] if n.endswith("_h") else n


def main(path):
    lines = open(path, errors="replace").read().split("\n")
    start = next(i for i, l in enumerate(lines) if re.match(r"^(kernel|fragment)\s.*main0\(", l))
    body = [l for l in lines[start + 1:] if not DECL.match(l)]
    stmts = []
    for l in body:
        m = ASSIGN.match(l)
        if m:
            stmts.append((m.group(1), IDENT.findall(m.group(2))))
    tainted = set(LANE_SEEDS)
    for _ in range(4):
        changed = False
        for lhs, ids in stmts:
            b = base(lhs)
            if b in tainted:
                continue
            if any(base(i) in tainted for i in ids):
                tainted.add(b)
                changed = True
        if not changed:
            break
    h_total = 0
    h_uniform = 0
    total = 0
    uniform = 0
    for lhs, ids in stmts:
        total += 1
        u = base(lhs) not in tainted
        uniform += u
        if lhs.endswith("_h"):
            h_total += 1
            h_uniform += u
    print("%s: assignments %d, uniform %d (%.1f%%); of the %d second-half statements %d are wave-uniform (%.1f%%)" % (
        path.split("/")[-1], total, uniform, 100.0 * uniform / max(total, 1), h_total, h_uniform, 100.0 * h_uniform / max(h_total, 1)))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
