# M27-06 review

Decision: ready for final-head full and hosted validation. Integration remains
conditional on all32 existing exact-name/multiplicity records and source/tree/
base/review guards. This is an agent review, not independent human acceptance.

- The added Python build/report command and eight ordinary regression tests do
  not alter production C++, installed SDK targets/headers, ABI/defaults, previous
  tools/fixtures/workflows or qualification claims. Existing release-tool tests
  remain intact; their suite additionally discovers the new class. Generated
  documentation and reviewed release digests are refreshed mechanically.
- Real CLI runs compiled two fresh SDKs, extracted both actual CPack archives,
  copied the public consumer outside the checkout and executed it against each.
  Both full archives and complete member inventories match. The final run binds
  clean source45cf8a1e10f4e0508132b225e0d761c032eb8ff9 and the driver digest.
  No archive copy is substituted for the second build.
- Source/dependency/tool identities are observed before and after builds. Exact
  commands, cache/compile-command/CPack-config and consumer-source digests are
  retained. The installed configuration must resolve inside the extracted SDK;
  consumer compilation must contain no source-checkout/private-build path.
- Archive comparison checks contents, metadata and link targets without
  extracting entries. It rejects missing, incomplete and duplicate inventories;
  matching members alone cannot turn different container bytes into a pass.
- A reproduced intermediate driver gap accepted a normally completed command
  beyond its output limit. The final driver also checks completed-command limits;
  the original driver fails the new oracle and the final driver passes. Ordinary
  nonzero command exits preserve output and status. Owned command groups are
  cleaned up on abort; interruption scheduling itself was not separately tested.
- Reports are unsigned local observations. They do not authenticate a builder or
  establish a hermetic environment, host-library closure, cross-compiler or
  cross-platform reproducibility, release approval or qualification. Ignored and
  external environment inputs remain a limit of clean-Git observations.
- The build entry point is Linux-only; archive comparison tests run portably.
  Original signature-fixture expansion remains unperformed by owner scope.
  Existing fixture/candidate distinctions and all CAP-M20 criteria stay unchanged.

Rollback: revert only this batch's tool, test-class wiring and documentation/hash
updates. Retain previous evidence. M27-07/08 and excluded05/06/external obligations
remain separate. No production race, ABI or timing behavior is changed here.
