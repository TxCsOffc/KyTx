"""Summarise a KytyPS5 printf log into median cost per draw.

    python frame_stats.py <log> [<log> ...]

WHY THIS EXISTS. fps on its own is not comparable between two runs of "the
same" fight: scenes vary 2,400-4,100 draws per frame, so a 70% difference in
frame time can be pure scene mix. Twice on 2026-09-11 a change was called a win
from a single FrameProfile line or a bucket ratio and had to be withdrawn:

  - the transfer queue cut finish_ms 85 -> 17 ms/frame and was NEUTRAL per draw
  - raising the GC threshold looked like -25% in an early window and was 0 once
    the run reached steady state

So: normalise by draws, take the MEDIAN over the whole run, and drop warm-up.

TRAP: every FrameProfile field accumulates over the window and is reset
together; `frames=N` is the window length. draws=7971 frames=3 is 2,657 draws
per frame, not 7,971. Same for every _ms field. GpuBusy/GpuDraws are different -
those windows are exactly one frame.
"""

import re
import statistics
import sys

# All buckets accumulate over `frames`; draw_ms nests inside drawprep_ms, and
# gc_ms contains finish_ms spent inside GC, so these do not sum to the total.
FRAME = re.compile(
    r"fps=([0-9.]+) frames=(\d+) .*?draws=(\d+) draw_ms=([0-9.]+) "
    r"dispatch=\d+ dispatch_ms=([0-9.]+).*?finish=\d+ finish_ms=([0-9.]+).*?"
    r"drawprep_ms=([0-9.]+).*?getprog_ms=([0-9.]+) rtresolve_ms=([0-9.]+) "
    r"gc=\d+ gc_ms=([0-9.]+).*?cp_rest=([0-9.]+)"
)
BUCKETS = ["draw", "dispatch", "finish", "drawprep", "getprog", "rtresolve", "gc", "cp_rest"]
MIN_DRAWS_PER_FRAME = 1500  # below this it is a menu/loading frame, not a fight


def analyse(path):
    try:
        text = open(path, errors="ignore").read()
    except OSError as error:
        return f"{path}: {error}"

    rows = []
    for m in FRAME.finditer(text):
        fps, frames, draws = float(m.group(1)), int(m.group(2)), int(m.group(3))
        if fps <= 0 or frames == 0 or draws == 0:
            continue
        per_frame = draws / frames
        if per_frame < MIN_DRAWS_PER_FRAME:
            continue
        row = {"total": 1000.0 / fps / per_frame * 1000, "fps": fps, "dpf": per_frame}
        for name, group in zip(BUCKETS, range(4, 12)):
            row[name] = float(m.group(group)) / draws * 1000
        rows.append(row)

    if not rows:
        return f"{path}: no in-fight frames (need >={MIN_DRAWS_PER_FRAME} draws/frame)"

    rows = rows[len(rows) // 3:]  # drop warm-up: shader compiles, cache fill
    med = lambda key: statistics.median(r[key] for r in rows)
    totals = sorted(r["total"] for r in rows)

    out = [
        f"{path}",
        f"  samples {len(rows)}   draws/frame {med('dpf'):.0f}   fps median {med('fps'):.2f}",
        f"  TOTAL   {med('total'):6.1f} us/draw"
        f"   (p25 {totals[len(totals)//4]:.1f}  p75 {totals[3*len(totals)//4]:.1f})",
        "  " + "  ".join(f"{n}={med(n):.1f}" for n in BUCKETS),
    ]
    return "\n".join(out)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        print(analyse(path))
        print()
