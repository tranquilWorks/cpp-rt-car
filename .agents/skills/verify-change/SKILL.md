---
name: verify-change
description: Verify a change against its acceptance criteria using deterministic commands and record evidence without overstating validation.
---

## Procedure
1. Map every acceptance criterion to one or more checks.
2. Discover and run the repository's actual verification contract (verification.commands, verification.yaml, or its native verifier) in fail-fast order.
3. Capture command, exit code, relevant output, environment, and artifact paths.
4. Distinguish static, simulated, protocol, bench, field, playtest, staging, and production evidence.
5. Record skipped or impossible checks explicitly.
6. Produce evidence.json conforming to contracts/evidence.schema.json.

Exercise the changed behavior through its real entry point and relevant consumer,
including failure/recovery behavior when it matters. A parser test, mocked
transport, build, or screenshot establishes only the behavior it actually checks.
Use required gates plus checks tied to concrete remaining risks; do not add or
rerun broad suites solely to increase test counts. For documentation-only changes,
inspect semantics, links, and relevant formats unless mandatory gates require more.
Store verbose output in evidence artifacts and put concise, revision-bound results
in the handoff so a resumed task can reuse verified work.

## Exit criteria
Every acceptance item is pass, fail, blocked, or unverified with evidence.
