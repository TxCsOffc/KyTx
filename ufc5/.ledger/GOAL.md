# Project Goal

## Primary Objective

Make UFC 5 run in Kyty at a sustained playable frame rate by specializing Kyty for this game, while preserving correct rendering, stable gameplay, and controls.

## Success Criteria

- **30 FPS sustained in a real fight is the hard minimum.** Below that the result does not count as playable.
- **60 FPS is the stretch target**, currently considered unlikely but the thing to aim at.
- A real fight is visually correct and responsive from launch through sustained gameplay.
- Repeated, comparable gameplay runs hold the rate without major stutters, crashes, or memory exhaustion.
- Performance gains are measured end to end against a controlled baseline; diagnostic shortcuts that break the picture do not count.

## Current Milestone

The ufc5-v2 path renders correctly; the only remaining problem is frame rate (FACT-0005). All four
validated optimizations are default-on and the frame time is attributed: host CPU command recording
is the dominant cost. Reduce per-draw CPU work toward the 30 FPS floor.

## Constraints

- Windows / Vulkan / RTX 3070 8 GB is the current measured target; record hardware and flags for comparisons.
- This fork serves exactly one game. UFC 5 specific specialization is explicitly authorized
  (DEC-0003) and need not generalize: game-specific fast paths, hardcoded shader hashes and
  heuristics tuned to this title's render graph are all acceptable, provided they preserve
  visual correctness and stability for UFC 5.
- Keep a reference path and preserve visual correctness, synchronization, memory ownership, and CPU publication.
- The working tree contains uncommitted emulator and profiling work; do not discard it.

## Non-Goals

- General PS5 game compatibility before UFC 5 is playable.
- Claiming FPS wins from isolated CPU or GPU subpath timings alone.
- Treating broken 1080p output or disabled game work as a playable result.
