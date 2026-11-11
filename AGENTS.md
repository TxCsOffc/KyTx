<!-- Tracked copy. The live file Codex loads is D:\PS5\AGENTS.md at the workspace root,
     because the session root sits two levels above this repo. Keep the two in step; this copy
     exists so the instructions are backed up and travel with a clone. -->

# PS5 / Kyty — UFC 5 work

## Read project memory before doing anything significant

This project's persistent memory is a **Ledger** store, not a markdown file. The `ledger`
CLI is on PATH, but it resolves `.ledger` from the *current directory* — it does not search
upward. So always run it from the project dir:

```bash
cd D:/PS5/src/KytyPS5/ufc5 && ledger context "<your current task>"
```

- `ledger context "<task>"` — start here for any real work. Returns goal, current state, and
  only the facts/decisions/experiments/warnings/tasks relevant to that task.
- `ledger brief` — compact orientation when you just need bearings.
- `ledger search "<topic>"` — run before any architectural decision.
- `ledger blockers` / `ledger unresolved` — active warnings, failures, open questions.
- `ledger status` — full human snapshot.

Entries are versioned with status. **Use ACTIVE knowledge only; do not assume a historical
entry still holds.** Record findings with
`ledger add fact|decision|experiment|failure|hypothesis|warning|task|note "..."`, and retire
beliefs with `ledger invalidate <ID> --because <ID>` or `ledger supersede <OLD> <NEW>` rather
than editing history. Update `STATE.md` at the end of significant work.

Full conventions: `src/KytyPS5/ufc5/.ledger/AGENT-INSTRUCTIONS.md`

## Historical notes

`src/KytyPS5/ufc5/ledger.md` (403 KB) is the original free-form log, preserved because curated
ledger entries cite it as `ledger.md:L42..68`. It is DEPRECATED: read it only to follow an
evidence reference, never as current state. A byte-identical copy lives at
`.ledger/archive/ledger-legacy-2026-10-06.md`.

## Measurement discipline

The ledger's warnings encode mistakes already made on this project. Before claiming any
performance win, check `ledger blockers`. In particular: subpath CPU savings are not additive
and do not establish an FPS gain, and a result with a broken picture is a diagnostic, not a win.
Record exact build, scene, and flags for every comparison.
