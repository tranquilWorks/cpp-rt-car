---
name: ci-repair
description: Apply a bounded repair to a trusted pull-request branch after an evidence-backed CI diagnosis.
---

## Preconditions
- Maintainer-triggered invocation.
- Clean isolated branch or worktree.
- Diagnosis identifies a likely causal failure.
- Allowed paths and validation are explicit.

## Procedure
1. Trace the first causal failure and make a coherent repair, including affected consumers when they are inside the approved scope.
2. Do not rewrite unrelated code or weaken tests.
3. Run the failing check, then the broader required gate set.
4. Allow at most two repair iterations.
5. Resolve routine ambiguity with source and logs. Stop on unresolved material ambiguity, scope expansion, missing credentials, or protected-path changes; report the exact boundary.

The two-attempt limit governs this automated CI repair lane. Preserve commands,
results, failed approaches, and the next diagnostic action for resumption. Do not
reinterpret an environment failure as a product pass or weaken required checks.

## Outputs
Patch, validation evidence, and remaining uncertainty. Never merge or deploy.
