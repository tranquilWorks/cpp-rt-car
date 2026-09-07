# M23-02 CLI discovery closure — 2026-09-07

## Provenance and scope

Continuation starts from merged target PR #252 at
`543af2b4728a171243e6d756554efa188c2e7d2f`. Its final source head was
`c6b14e49ad5d26f995ce7523dd5e6b9974c9af92`, tree
`9ea42193b81f1fb9d9f50a5e26a3fb0e3127a6b2`.
Control PR #366 merged at `7549d5d647fada8fad7637779301226c3ad87296`;
the amended canonical M23-02 blob is
`8e760eb75b228cf063c1a4e82ca92434a8f81cf4`. Activation copies this contract
and binds its revision/blob. The original planning baseline and control
planning revision remain unchanged.

The source correction changes only the legacy CLI discovery assertion in
`tests/benchmark_fixtures/test_artifacts.py`: expect the exact sorted union
of `rtfw.self:structural` and the 51 checked CPU catalog IDs. This retains
equality, ordering, no-extra/no-missing behavior and all nine legacy artifact
tests, including invalid selection, destination preservation, malformed
artifacts, privacy, and fake/steady self-provider runs. The independent CPU
inventory gate still checks the finite catalog and actual provider behavior.
Other changes are contract binding, current-state/handoff/roadmap, this
evidence, and mechanically refreshed release digests.

## Retained predecessor results

The original M23-02 evidence is preserved without rewriting its failures.
Final-head CI run [34085474434](https://github.com/tranquilWorks/cpp-rt-car/actions/runs/34085474434)
finished with 17 successful jobs, nine failed matrix jobs, and one cancelled
Windows sibling. All nine failed job logs identify only the obsolete exact
list assertion; each completed 42 of 43 CTest gates successfully.
The failed job IDs are 101628566816, 101628566877, 101628566983,
101628567002, 101628567034, 101628567046, 101628567049,
101628567092, and 101628567105.

Windows optional SDK job 101628566921 passed the path-normalization repair.
Artifact exchange job 101630036788 passed the canonical CPU/self/external
comparisons across GCC, Clang, FMA settings, and MSVC package fixtures.
Portable Clang 14 assurance (101628566667) and GCC ThreadSanitizer
(101628566926) passed. These are predecessor evidence, not a substitute for
the correction's own hosted checks. Control #366's pre-step CI failure is
also historical; user integration is observed, not inferred CI success.

## Correction verification

All commands below run in the target repository with
`PATH=/root/.local/bin:$PATH` for the installed CMake/CTest toolchain.

- `ctest --test-dir build/m23-cpu -R 'm23_benchmark_(artifact_validator|cpu_inventory)' --output-on-failure`:
  exit 0, both gates passed, including all nine legacy tests and 51 CPU cases.
- `ctest --test-dir build/m23-cpu --output-on-failure`: exit 0, 39/39 gates passed;
  log `/tmp/m23-closure-ctest.log`.
- `./scripts/agent-verify.sh full`: exit 0, 37/37 gates passed on the first
  correction run, including experimental/RapidCheck coverage. Retained local
  log `docs/evidence/local/verify-full-20260907T052009Z.log`.
- `git diff --check`: exit 0. Scope/canonical equality and unchanged
  Runtime/ABI/schema/provider/prior-evidence tree checks passed.
- `python3 tools/check_release_contract.py --write-hashes`: exit 0.

Hosted CI for this correction and its final source/PR identifiers will be
recorded in the PR after publication. No successful result is inferred from
the predecessor merge. A verified recovery bundle is retained before session
exit; the PR records its exact source head and tree.

## Acceptance and preserved boundaries

The omitted-scope regression is repaired under the integrated amendment,
without disabling or weakening discovery, artifact, privacy, or package
gates. Review of this small diff confirms exact inventory equality and no
product execution change. Original provider acceptance/evidence remains in
the previous M23-02 record; follow-up CI is still required for gate closure.
No separate human-review record is manufactured.

Runtime/runner/provider implementation, frozen schemas, artifact validator,
C ABI v8 (70 exports and fingerprint 0xd0e7a5a14bf35f97), SONAME 8,
device ABI v1, release 1.2.1, support claims, and prior evidence are unchanged.
There are no new concurrency, lifetime, timing, or allocation paths. No
new sanitizer campaign is needed for a Python expectation-only correction;
the mandatory hosted sanitizer matrix remains required.
This is portable RT0 structural/characterization work. M23/CAP-M23 remain
incomplete pending M23-03 through M23-05; hardware/RT, controlled thresholds,
Unreal, signing, release, and deployment remain unperformed and unclaimed.

Rollback: revert this scoped correction and its governance/docs/digest
updates. That restores the known failing discovery assertion; it does not
change runtime behavior or require data migration.
