---
name: implement-batch
description: Implement an approved batch in an isolated branch or worktree while preserving scope, invariants, and unrelated work.
---

## Rules
- Read the batch contract and repository guidance first.
- Apply the engineering-execution skill for nontrivial delivery and resume.
- Trace affected entry points, consumers, state, and failure/recovery paths before choosing the repair.
- Do not broaden scope silently.
- Complete the coherent outcome, including integration and operational documentation. Do not stop at a scaffold or disconnected helper.
- Add or update tests for meaningful behavior and plausible regressions; use proportionate inspection for reversible prose-only changes.
- Run narrow checks before broad checks.
- Use current user authorization for commit/push/PR actions; do not ask again for already-authorized steps. Merge, release, and deployment retain their separate authority.
- Resolve routine ambiguity through inspection and reasonable stated assumptions. Preserve scope, decisions, evidence, and next action in the existing durable handoff.

## Required output
- The requested implementation and relevant verification.
- Updated documentation when interfaces or operation change.
- Evidence draft listing commands, results, residual risks, and unperformed validation.

## Stop conditions
Complete unblocked authorized work. Stop the affected action when a material
product decision is required, a forbidden path must change, validation cannot be
made credible, or the batch contract is contradictory. Identify the specific
missing decision or evidence. Do not treat routine uncertainty or context
compaction as a reason to abandon the task.
