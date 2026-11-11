#!/usr/bin/env python3 -I
"""Static statistics of the MSL that the KyTx RDNA->MSL backend emits for the heaviest GPU programs.

Run:  python3 -I tools/bench/msl_static_stats.py [--md] [--csv FILE] [--dump DIR ...]

The programs are the ten heaviest by GPU time of the profile (host hashes as the profiler prints them). Each file is looked up in the
dump directories in order (default: .porting/work/out/r7/msl, then every .porting/artifacts/2026-11-01/*-msl, newest first). A program
that is in no dump is reported as MISSING. The script only reads files; nothing game-derived is printed beyond counters.

Definitions (all counters are over the entry function body, the helper functions at the top of the file are skipped):
  stmts        statements (lines ending in ';' that are not the zero-initialised register declarations)
  regs         register declarations at the top of the function: one per IR value, "_h" ones are the second wave64 half
  h_stmts      statements that define a "_h" value (second half of the wave64 pair lowering), phi copies included
  phi          "kyty_phi_*" temporaries plus the copy-backs (control-flow merges lowered to temporaries); the categories phi / sel / cast /
               fp_ops / int_ops / bool_ops / mem are mutually exclusive, the remainder is "other"
  mem          statements that load from a buffer/LDS/image/texture (stores are not assignments and are counted by img_wr / lds_acc)
  sel          statements that are a plain select 'c ? a : b' (exec-mask / v_cndmask style)
  cast         statements that only reinterpret bits (as_type / float() / (uint) / component extract)
  fp_ops       statements with a float destination that do arithmetic or call a float math function
  int_ops      statements with an integer/vector destination that do arithmetic, shifts or bit ops
  fma, xcend   fma() calls, transcendental calls (exp2 log2 sin cos sqrt rsqrt) and float divisions
  bchk_dw      dword buffer loads with a bounds check:  idx < (sizes[N] >> 2) ? ab.buffers[N][idx] : 0u
  bchk_rng     typed (f16/byte) buffer loads with a byte range check:  x <= sizes[N] && k <= sizes[N] - x
  lds_acc      accesses to the emulated LDS (threadgroup array kyty_lds), lds_chk those that carry a bound comparison
  max_bb       longest straight-line run of statements between control-flow lines (if / else / loop / break / continue / return)
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# (host hash, label, guest hash or None, share of GPU time in the profile, kind)
PROGRAMS = [
    ("3ad95c14ba036b6e", "cs", None, 7.9, "compute"),
    ("5d0d6f16bddf8cad", "cs", None, 5.3, "compute"),
    ("a170bb86d24a6f2a", "cs", "1e8ae08a", 3.3, "compute"),
    ("c4bb690fcdf9b908", "cs", "243db11c", 3.2, "compute"),
    ("30caea79425a62b0", "cs", "c29caf0c", 3.0, "compute"),
    ("78d2a08316edb04c", "cs", None, 1.9, "compute"),
    ("0ef491bfaeb7b0fc", "cs", "0e838ae6", 1.9, "compute"),
    ("e5288af74a34533b", "cs", None, 1.6, "compute"),
    ("edbcd7e4f0579e83", "cs", None, 1.5, "compute"),
    ("090e154f2a6e535c", "ps", "dec48a13", 7.0, "pixel"),
]

DECL_RE = re.compile(r"^\t(uint|int|float|bool|half|ushort|uint2|uint3|uint4|int2|int3|int4|float2|float3|float4)\s+(\w+)\s*=\s*\(\w+\)\s*0;\s*$")
ENTRY_RE = re.compile(r"^(kernel|fragment|vertex)\s.*\bmain0\s*\(")
ASSIGN_RE = re.compile(r"^\s*(?:const\s+(\w+)\s+)?(\w+)\s*=\s*(.*);\s*$")
FLOAT_FUNCS = re.compile(r"\b(fma|fmin|fmax|fabs|sqrt|rsqrt|exp2|log2|sin|cos|floor|ceil|trunc|fract|saturate|clamp|rint|round|precise::\w+|fast::\w+|kyty_med3f|mix|step|isnan|isinf|copysign)\s*\(")
XCEND_RE = re.compile(r"\b(exp2|log2|sin|cos|sqrt|rsqrt)\s*\(")
INT_FUNCS = re.compile(r"\b(popcount|clz|ctz|extract_bits|insert_bits|reverse_bits|mulhi|abs|min|max|select|rotate|addsat|subsat)\s*\(")
BCHK_DW = re.compile(r"<\s*\(sizes\[\d+\]\s*>>\s*2\)\s*\?")
BCHK_RNG = re.compile(r"<=\s*sizes\[\d+\]")
ATOMIC_RE = re.compile(r"\batomic_\w+|\batomic\b")
BARRIER_RE = re.compile(r"\b(threadgroup_barrier|simdgroup_barrier)\b")
SIMD_RE = re.compile(r"\b(simd_\w+|quad_\w+)\s*\(")
KY_SIMD_RE = re.compile(r"\bkyty_(ballot2|ballot|shuffle64|first64|first_lane)\s*\(")
IMG_READ = re.compile(r"\.read\(")
IMG_WRITE = re.compile(r"\.write\(")
IMG_SAMPLE = re.compile(r"\.(sample|sample_compare)\(")
IMG_GATHER = re.compile(r"\.(gather|gather_compare)\(")
IMG_ATOMIC = re.compile(r"\.atomic_\w+\(")
FTZ_RE = re.compile(r"\bkyty_ftz\s*\(")
LOOP_RE = re.compile(r"^\s*(while|for)\s*\(")
OTHER_HELPERS = re.compile(r"\b(kyty_f2u|kyty_f2i|kyty_class_f32|kyty_f16_rtz|kyty_med3f|kyty_sample_cmp|kyty_gather_cmp|kyty_d16_pack|kyty_d16_unpack)\s*\(")


def find_file(h, dirs):
    for d in dirs:
        p = os.path.join(d, "0x%s.metal" % h)
        if os.path.isfile(p):
            return p
    return None


def default_dirs():
    dirs = [os.path.join(ROOT, ".porting/work/out/r7/msl")]
    olds = glob.glob(os.path.join(ROOT, ".porting/artifacts/2026-11-01/*-msl"))
    olds.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return dirs + olds


def analyse(path):
    with open(path, "r", errors="replace") as f:
        lines = f.read().split("\n")
    st = dict(lines=len(lines), bytes=os.path.getsize(path))
    # entry function
    entry = None
    for i, l in enumerate(lines):
        if ENTRY_RE.match(l):
            entry = i
            break
    st["entry_line"] = (entry + 1) if entry is not None else 0
    head = lines[: entry if entry is not None else 0]
    body = lines[entry + 1:] if entry is not None else lines
    st["head_lines"] = len(head)
    st["body_lines"] = len(body)

    types = {}
    counts = dict(mem=0, stmts=0, regs=0, regs_h=0, h_stmts=0, phi=0, sel=0, cast=0, fp_ops=0, int_ops=0, bool_ops=0, fma=0, xcend=0, fdiv=0,
                 bchk_dw=0, bchk_rng=0, sizes_refs=0, lds_acc=0, lds_chk=0, atomics=0, barriers=0, simd=0, ky_simd=0, loops=0, img_read=0,
                 img_write=0, img_sample=0, img_gather=0, img_atomic=0, ftz=0, helpers=0, if_stmts=0, ctl_lines=0, uint_regs=0,
                 float_regs=0, bool_regs=0, vec_regs=0)
    max_bb_stmts = 0
    max_bb_lines = 0
    cur_stmts = 0
    cur_lines = 0
    stack = []

    def flush():
        nonlocal cur_stmts, cur_lines, max_bb_stmts, max_bb_lines
        if cur_stmts > max_bb_stmts:
            max_bb_stmts = cur_stmts
        if cur_lines > max_bb_lines:
            max_bb_lines = cur_lines
        cur_stmts = 0
        cur_lines = 0

    for line in body:
        s = line.strip()
        if not s:
            continue
        m = DECL_RE.match(line)
        if m:
            t, name = m.group(1), m.group(2)
            types[name] = t
            counts["regs"] += 1
            if name.endswith("_h"):
                counts["regs_h"] += 1
            if t in ("uint", "int", "ushort"):
                counts["uint_regs"] += 1
            elif t in ("float", "half"):
                counts["float_regs"] += 1
            elif t == "bool":
                counts["bool_regs"] += 1
            else:
                counts["vec_regs"] += 1
            continue
        # control flow / scope handling
        if s == "{":
            stack.append("scope")
            continue
        if s == "}":
            kind = stack.pop() if stack else "ctl"
            if kind != "scope":
                flush()
                counts["ctl_lines"] += 1
            continue
        if s.startswith("} else"):
            flush()
            counts["ctl_lines"] += 1
            if not stack:
                stack.append("ctl")
            continue
        if s.startswith("if (") or s.startswith("if(") or LOOP_RE.match(line):
            flush()
            counts["ctl_lines"] += 1
            if LOOP_RE.match(line):
                counts["loops"] += 1
            else:
                counts["if_stmts"] += 1
            if s.endswith("{"):
                stack.append("ctl")
            else:
                # single-statement if: keep scanning the rest of the line for counters but do not push
                pass
        elif s in ("break;", "continue;", "return;") or s.startswith("return "):
            flush()
            counts["ctl_lines"] += 1
            continue
        if not s.endswith(";") and not s.endswith("{"):
            continue
        # per-line counters
        counts["ftz"] += len(FTZ_RE.findall(line))
        counts["bchk_dw"] += len(BCHK_DW.findall(line))
        counts["bchk_rng"] += len(BCHK_RNG.findall(line))
        counts["sizes_refs"] += line.count("sizes[")
        counts["atomics"] += len(ATOMIC_RE.findall(line))
        counts["barriers"] += len(BARRIER_RE.findall(line))
        counts["simd"] += len(SIMD_RE.findall(line))
        counts["ky_simd"] += len(KY_SIMD_RE.findall(line))
        counts["img_read"] += len(IMG_READ.findall(line))
        counts["img_write"] += len(IMG_WRITE.findall(line))
        counts["img_sample"] += len(IMG_SAMPLE.findall(line))
        counts["img_gather"] += len(IMG_GATHER.findall(line))
        counts["img_atomic"] += len(IMG_ATOMIC.findall(line))
        counts["helpers"] += len(OTHER_HELPERS.findall(line))
        counts["xcend"] += len(XCEND_RE.findall(line))
        if "kyty_lds[" in line:
            counts["lds_acc"] += line.count("kyty_lds[")
            if re.search(r"<\s*\d+u", line):
                counts["lds_chk"] += 1
        if not s.endswith(";"):
            cur_lines += 1
            continue
        # statement classification
        a = ASSIGN_RE.match(line)
        if s.startswith("if ("):
            # 'if (cond) stmt;'
            counts["stmts"] += 1
            cur_stmts += 1
            cur_lines += 1
            continue
        counts["stmts"] += 1
        cur_stmts += 1
        cur_lines += 1
        if not a:
            continue
        decl_t, lhs, rhs = a.group(1), a.group(2), a.group(3)
        if lhs.endswith("_h"):
            counts["h_stmts"] += 1
        if "kyty_phi_" in lhs or "kyty_phi_" in rhs:
            counts["phi"] += 1
            continue
        t = decl_t or types.get(lhs, "")
        rhs_s = rhs.strip()
        if re.match(r"^\(?[\w.]+\s*\?\s*[\w.()]+\s*:\s*[\w.()]+\)?$", rhs_s):
            counts["sel"] += 1
            continue
        if re.match(r"^(as_type<\w+>\(\w+\)|\(?\w+\)?\.[xyzw]+|\w+|float\(\w+\)|\(uint\)\(?\w+\)?)$", rhs_s):
            counts["cast"] += 1
            continue
        has_fma = len(re.findall(r"\bfma\(", rhs))
        counts["fma"] += has_fma
        if re.search(r"ab\.buffers\[|ab\.flattened_srt\[|\.(read|sample|gather|sample_compare|gather_compare|get_width|get_height)\(|kyty_lds\[", rhs):
            counts["mem"] += 1
            continue
        if t in ("float", "half", "float2", "float3", "float4"):
            if FLOAT_FUNCS.search(rhs) or re.search(r"[-+*/]", re.sub(r"as_type<\w+>|0x[0-9a-fA-F]+u?", "", rhs)):
                counts["fp_ops"] += 1
            counts["fdiv"] += len(re.findall(r"\)?\s/\s", rhs))
        elif t == "bool":
            counts["bool_ops"] += 1
        elif t:
            if INT_FUNCS.search(rhs) or re.search(r"(<<|>>|[-+*/%&|^~])", re.sub(r"0x[0-9a-fA-F]+u?", "K", rhs)):
                counts["int_ops"] += 1
    flush()
    counts["max_bb_stmts"] = max_bb_stmts
    counts["max_bb_lines"] = max_bb_lines
    st.update(counts)
    # features of the whole text
    txt_head = "\n".join(head)
    st["lds_decl"] = ", ".join(re.findall(r"threadgroup\s+\w+\s+kyty_lds\[(\d+)\]", "\n".join(body[:200]))) or "-"
    st["pair"] = counts["regs_h"] > 0
    return st


COLS = [
    ("lines", "lines"), ("bytes", "KB"), ("stmts", "stmts"), ("regs", "regs"), ("regs_h", "regs_h"), ("h_stmts", "h_stmts"),
    ("phi", "phi"), ("sel", "sel"), ("mem", "mem"), ("cast", "cast"), ("fp_ops", "fp_ops"), ("int_ops", "int_ops"), ("bool_ops", "bool_ops"),
    ("fma", "fma"), ("xcend", "xcend"), ("ftz", "ftz"), ("helpers", "helpers"),
    ("bchk_dw", "bchk_dw"), ("bchk_rng", "bchk_rng"), ("sizes_refs", "sizes[]"), ("lds_acc", "lds_acc"), ("lds_chk", "lds_chk"),
    ("atomics", "atomic"), ("barriers", "barrier"), ("simd", "simd_*"), ("ky_simd", "ky_ballot/shfl"), ("loops", "loops"), ("if_stmts", "if"),
    ("img_read", "img_rd"), ("img_write", "img_wr"), ("img_sample", "sample"), ("img_gather", "gather"), ("img_atomic", "img_atom"),
    ("max_bb_stmts", "max_bb"), ("max_bb_lines", "max_bb_ln"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="append", help="dump directory (repeatable); default: r7 then the 2026-11-01 artifacts")
    ap.add_argument("--md", action="store_true", help="markdown table output")
    ap.add_argument("--csv", help="also write a CSV file")
    ap.add_argument("--extra", action="append", default=[], help="additional host hash to analyse (repeatable)")
    args = ap.parse_args()
    dirs = args.dump if args.dump else default_dirs()
    progs = list(PROGRAMS) + [(h, "cs", None, 0.0, "compute") for h in args.extra]
    rows = []
    for h, label, guest, share, kind in progs:
        p = find_file(h, dirs)
        if p is None:
            rows.append((h, label, guest, share, kind, None, None))
            continue
        rows.append((h, label, guest, share, kind, p, analyse(p)))

    print("# source files")
    for h, label, guest, share, kind, p, st in rows:
        print("%s %s guest=%s share=%.1f%% %s -> %s" % (h, label, guest or "-", share, kind, os.path.relpath(p, ROOT) if p else "MISSING from every dump"))
    print()
    heads = ["program", "share%", "pair"] + [c[1] for c in COLS]
    out_rows = []
    for h, label, guest, share, kind, p, st in rows:
        if st is None:
            out_rows.append([h[:8], "%.1f" % share, "MISSING"] + ["-"] * len(COLS))
            continue
        vals = []
        for key, _ in COLS:
            v = st[key]
            vals.append(str(v // 1024) if key == "bytes" else str(v))
        out_rows.append([h[:8], "%.1f" % share, "y" if st["pair"] else "n"] + vals)
    if args.md:
        print("| " + " | ".join(heads) + " |")
        print("|" + "|".join(["---"] * len(heads)) + "|")
        for r in out_rows:
            print("| " + " | ".join(r) + " |")
    else:
        w = [max(len(heads[i]), max(len(r[i]) for r in out_rows)) for i in range(len(heads))]
        print("  ".join(heads[i].ljust(w[i]) for i in range(len(heads))))
        for r in out_rows:
            print("  ".join(r[i].rjust(w[i]) for i in range(len(heads))))
    # derived ratios
    print()
    print("# derived")
    print("program   h_stmts/stmts  bchk_dw per 1000 stmts  ftz per 1000 stmts  sel+phi+cast share of stmts (no ALU)  bounds-checked share of mem loads")
    for h, label, guest, share, kind, p, st in rows:
        if st is None:
            print("%s  MISSING" % h[:8])
            continue
        n = max(st["stmts"], 1)
        loads = st["bchk_dw"] + st["bchk_rng"]
        print("%s  %5.1f%%   %7.2f   %7.2f   %5.1f%%   (%d checked loads, %d ternaries + %d range)" % (
            h[:8], 100.0 * st["h_stmts"] / n, 1000.0 * st["bchk_dw"] / n, 1000.0 * st["ftz"] / n,
            100.0 * (st["sel"] + st["phi"] + st["cast"]) / n, loads, st["bchk_dw"], st["bchk_rng"]))
    if args.csv:
        with open(args.csv, "w") as f:
            f.write(",".join(heads) + "\n")
            for r in out_rows:
                f.write(",".join(r) + "\n")


if __name__ == "__main__":
    main()
