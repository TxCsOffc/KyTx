# Ledger Agent Instructions

This repository uses Ledger for persistent project memory.

## At the beginning of significant work

Run `ledger context "<your current task>"`. Use current ACTIVE knowledge. Do not assume historical entries remain valid.

## Before major architectural decisions

Run `ledger search "<topic>"` and check decisions, failures, warnings, and experiments.

## During work

Record useful discoveries with `ledger add fact|decision|experiment|failure|hypothesis|warning|task|note "..."`. Avoid trivial details.

When evidence disproves a belief, use `ledger invalidate <ID> --because <ID>` or `ledger supersede <OLD_ID> <NEW_ID>`. Preserve history.

## At the end of significant work

Record important discoveries, decisions, outcomes, failures, unresolved questions, and changes in project state. Run `ledger status` and update STATE.md when needed.
