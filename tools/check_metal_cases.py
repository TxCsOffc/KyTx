#!/usr/bin/env python3
"""Success check for the Metal TestCase runner (--metal-cases of shader_recompiler_compute_tests).

Builds the target, runs every case on Metal, then runs again with KYTX_METAL_CASES_CORRUPT=1, where each executed case
must fail (proves the comparison is wired to the GPU result). Real emulator failures are reported, not fatal."""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path("/Users/tx/Developer/KyTx")
BUILD = REPO / "_Build/macos-arm64"
BIN = BUILD / "shader_recompiler_compute_tests"
MARKER = REPO / ".porting/work/HOST_GPU_TESTS_BLOCKED"
RUN_DIR = BUILD / "metal-cases-run"


ap = argparse.ArgumentParser()
ap.add_argument("--require", default="", help="comma-separated case names that must pass")
ap.add_argument("--baseline", default="", help="file with case names that passed before; all must still pass")
args = ap.parse_args()


def fail(text):
    print(text)
    print("FAIL")
    sys.exit(1)


build = subprocess.run(["ninja", "-C", str(BUILD), "-j", "14", "shader_recompiler_compute_tests"],
                       capture_output=True, text=True)
if build.returncode:
    lines = (build.stdout + build.stderr).splitlines()
    errors = [l for l in lines if "error" in l.lower()][:30]
    fail("BUILD FAILED\n" + "\n".join(errors or lines[-40:]))
if MARKER.exists():
    fail("GPU blocked by " + str(MARKER) + ": " + MARKER.read_text().strip())
RUN_DIR.mkdir(exist_ok=True)


def run(corrupt):
    env = os.environ.copy()
    env.pop("KYTX_METAL_CASES_CORRUPT", None)
    if corrupt:
        env["KYTX_METAL_CASES_CORRUPT"] = "1"
    try:
        r = subprocess.run([str(BIN), "--metal-cases"], capture_output=True, text=True, timeout=900, env=env, cwd=RUN_DIR)
    except subprocess.TimeoutExpired:
        fail("--metal-cases did not finish in 900 s")
    out = r.stdout + r.stderr
    total = re.search(r"^MetalCases: total=(\d+)$", out, re.M)
    summary = re.search(r"^MetalCases: passed=(\d+) failed=(\d+) skipped=(\d+)$", out, re.M)
    if r.returncode != 0 or not total or not summary:
        fail(f"--metal-cases exit={r.returncode}, summary lines missing. Last output:\n" + "\n".join(out.splitlines()[-40:]))
    return out, int(total.group(1)), *map(int, summary.groups())


out, total, passed, failed, skipped = run(False)
fails = [l for l in out.splitlines() if l.startswith("[metal] ") and " FAIL " in l]
print(f"normal run: total={total} passed={passed} failed={failed} skipped={skipped}")
print("\n".join(fails[:40]))
if passed + failed + skipped != total:
    fail("passed+failed+skipped != total")
if len(fails) != failed:
    fail("number of '[metal] <name> FAIL' lines != failed")
if passed < 1:
    fail("no case passed: the runner itself is broken")
status = {}
for l in out.splitlines():
    m = re.match(r"\[metal\] (\S+) (ok|FAIL|skip)\b", l)
    if m:
        status[m.group(1)] = m.group(2)
if args.baseline or args.require:
    problems = []
    for name in filter(None, args.require.split(",")):
        if status.get(name) != "ok":
            line = next((l for l in out.splitlines() if l.startswith(f"[metal] {name} ")), "(not run)")
            problems.append("REQUIRED " + line[:400])
    if args.baseline:
        for name in Path(args.baseline).read_text().split():
            if status.get(name) != "ok":
                line = next((l for l in out.splitlines() if l.startswith(f"[metal] {name} ")), "(not run)")
                problems.append("REGRESSION " + line[:400])
    if problems:
        fail("\n".join(problems[:30]))
    print(f"required cases pass; no regression ({passed} passed)")
    print("PASS")
    sys.exit(0)
_, _, c_passed, c_failed, c_skipped = run(True)
print(f"corrupt self check: passed={c_passed} failed={c_failed} skipped={c_skipped}")
if c_passed != 0 or c_failed != passed + failed or c_skipped != skipped:
    fail("corrupted expectations must make every executed case fail")
(RUN_DIR / "last.txt").write_text(out)
print("PASS")
