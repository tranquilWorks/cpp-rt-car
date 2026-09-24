---
name: plan-batch
description: Turn an approved milestone into a coherent implementation batch with bounded paths, observable acceptance, validation, rollback, and evidence requirements.
---

## Inputs
- Product and architecture contracts.
- Current repository state.
- Milestone dependency graph.

## Procedure
1. Map affected components and prerequisites.
2. Choose a complete vertical outcome, including consumer wiring, operation, and recovery where relevant. Size it by coupling and reviewability, not an arbitrary file count.
3. Define allowed and forbidden paths.
4. State acceptance as observable behavior.
5. Specify exact deterministic validation and manual gates.
6. Identify rollback and residual hazards.
7. Validate against contracts/batch.schema.json.

Use the engineering-execution skill for investigation and continuity. Resolve
routine implementation choices from evidence. If the user authorized a sequence,
preserve its dependencies and stopping boundary rather than inserting approval
pauses between every mechanical step. Material unresolved product decisions
remain explicit; planning alone does not authorize implementation.

## Exit criteria
A builder can implement the batch without making new product decisions or touching unrelated systems.
