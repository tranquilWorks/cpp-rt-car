[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Release and Compatibility Policy

RTFW 1.2 is a portable RT0 runtime release. It supports the named build and
test tuples in [the portable support matrix](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/docs/portable_support_matrix.json).
Passing those gates establishes functional behavior and bounded-capacity API
semantics; it does not establish a latency bound, RT1 measurement result, RT2
qualification, or hardware-backend qualification.

## Versioning

RTFW package releases follow Semantic Versioning:

- patch releases fix defects without intentionally changing supported API
  behavior;
- minor releases may add backward-compatible target-runtime functionality;
- major releases may remove or incompatibly change stable source interfaces.

The package version, C ABI version, device ABI version, observability schema,
checkpoint schema, and qualification-record schema are independent version
domains. A package-major change does not silently change any other domain.

## Compatibility surfaces

| Surface | 1.x promise |
| --- | --- |
| C ABI v8 | Stable binary interface represented by the v8 headers, fingerprint, export allowlist, and SONAME |
| Target C++ runtime | Source compatibility for the supported declarations in the default SDK headers; `<rt/runtime.hpp>` remains the umbrella, recompilation is required, and no C++ binary ABI is promised |
| Installed CMake package | Same-major package discovery, component names, and imported target names remain compatible |
| Artifact formats | Checkpoint/input-log readers retain explicit schema validation; incompatible formats require a new schema |
| Runtime profiles | Profile envelope schema 1 and runtime-config schema 7 retain fail-closed compatibility checks; incompatible formats require a new schema |
| Observability | Existing schema-v2 IDs and meanings are retained; additions require versioned review |
| Optional CUDA/XDMA adapters | Candidate source APIs until a separately documented stability milestone |
| Legacy `SimCore`, scheduler, plugin, fiber, and utility surfaces | Experimental and outside the 1.x compatibility promise |

Stable C++ source declarations are deprecated for at least one minor release
before removal when a safe compatibility path exists. Security fixes may
disable unsafe behavior immediately, but the release notes must identify the
exception. Incompatible C ABI changes require a new ABI number, SONAME,
allowlist, fingerprint, and compatibility test; the v8 manifest is immutable.

`rtfw::runtime` is the preferred target beginning in 1.2. The
`cpp_runtime` component and `rtfw::simcore_rt` target remain available
throughout 1.x as compatibility names. The deprecated `tick_duration`,
`DemoPipeline`, and `build_demo_pipeline` declarations accidentally reachable
from `<rt/runtime.hpp>` remain available until 2.0. The `tick_duration` shim is
provided by `rtfw::runtime`; the legacy pipeline implementation is provided
only by the compatibility target, so its SimCore/fiber path is not part of the
production runtime archive. The broader SimCore/plugin/scheduler/fiber surface
remains experimental and outside the compatibility promise.

## Support levels

- **Supported** — the exact tuple is a required pull-request and release gate;
  regressions are release blockers.
- **Best effort** — expected to work from portable code paths but not exercised
  on every change.
- **Candidate** — implementation and portable tests exist, but any hardware or
  latency claim requires an independently reviewed tuple.
- **Experimental** — no compatibility or support commitment.

Only entries in `supported_tuples` in the portable matrix are supported for
1.2. The tuple covers compiler/build/runtime functionality at RT0. Application
callbacks, host job systems, allocators, operating-system policy, drivers, and
hardware remain part of the deployment being evaluated.

## Release gate

A release candidate must:

1. match `VERSION.txt`, the public version header, package metadata, support
   matrices, changelog, and checked-in release contract;
2. pass the complete required CI matrix, documentation contract, stable C ABI
   check, sanitizers, determinism exchange, and relocated external consumers;
3. pass the dedicated multi-runtime device-isolation gate;
4. produce CPack archives from every supported tuple and run the relocated
   consumer against each exact extracted archive;
5. produce a content-addressed release manifest containing every archive's
   relative name, byte length, and SHA-256 digest;
6. retain the CI-generated SPDX SBOM and never describe a checksum or build
   record as a reproducible-build proof;
7. pin every third-party workflow action to a reviewed full commit;
8. review every present-tense support, determinism, latency, and qualification
   claim before creating tag `v<version>`.

M20-PRE-01 adds a pre-release candidate gate that deterministically verifies
dependency/action identity, exact Clang 14 analyzer coverage, bounded parser
fuzz smoke, a canonical SPDX 2.3 candidate SBOM, an unsigned in-toto candidate
statement, an exact expected-source final manifest, offline cryptographic
verification of a separate public non-target fixture, and relocated package/C
ABI/SONAME behavior. Those outputs remain below CI evidence storage and are not
tag assets. The candidate statement is unauthenticated; the signed fixture does
not sign RTFW. Target signing, authenticated provenance, reproducible releases,
and publication require later approved work and human release gates.

The tag must identify the exact source commit used by the release workflow.
Release archives and their generated manifest are immutable after publication;
a changed artifact requires a new package version.
After all three exact-tuple package/consumer jobs pass, a tag-triggered workflow
publishes their nine uniquely named archive, checksum, and manifest assets as
one draft GitHub Release, then makes it public. A manual workflow dispatch
builds and retains the same inputs for review but does not publish a release.
Publication fails if the tag does not exist or the release already exists;
assets are never overwritten. An interrupted upload remains a draft for
operator review rather than exposing a partial public release.

The checked-in release contract's digest keys are repository-relative source
paths. The contract is included in installed packages as an audit reference,
not as a claim that the binary archive contains the source workflows and
release tools. The separate artifact manifest hashes the complete published
archive and checksum files.

## Qualification boundary

Portable 1.2 completion does not promote M9 or M10 and does not create an RT2
record. PREEMPT_RT, CUDA, and XDMA evidence is admitted only through the
versioned matrices and procedures linked from the
[real-time readiness checklist](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/docs/real_time_readiness_checklist.md),
[CUDA contract](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/docs/cuda_backend.md), and [XDMA contract](docs__xdma_backend.md).
Raw evidence marked `evidence_only` is an input to review, not a qualification.
The opt-in CUDA/XDMA workflows validate that evidence schema, then bind every
raw file to the complete source commit with a SHA-256 manifest before upload.
Neither a schema pass nor that integrity manifest promotes the corresponding
support matrix.

M18 qualification schema version 1 is independent of the package and release
contract versions. A separately retained campaign plan, complete qualification
record, exact raw-artifact tree, and passing human review may produce only a
deterministic `proposal_only` handoff. The reviewer name and timestamp are not
authentication or cryptographic chronology, and the proposal never edits a
support matrix. A human must verify external pre-run plan provenance and review
the separate matrix change. Synthetic fixtures and portable CI are never
eligible. See [the qualification contract](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/docs/qualification.md).

## Security and lifecycle

Supported release lines and reporting instructions are in
[the repository security policy](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/SECURITY.md). Release checksums detect
artifact corruption or substitution only when the manifest itself is obtained
from a trusted channel. RTFW 1.2 does not claim signed releases,
bit-for-bit-reproducible builds, plugin isolation, or safe execution of
untrusted native callbacks.
# Extension ABI v1 release boundary

M19-01 adds one installed header and an additive C++ source API without
changing release 1.2.1, C ABI v8, SONAME 8, device ABI v1, target/component
inventory, support matrices, or qualification state. Extension ABI v1 may be
removed only as one coherent rollback before a release depends on it, or
evolved compatibly by appending size-guarded fields. An incompatible change
requires a new ABI version and symbol.

## Local independent package comparison

The owner-scoped M27-06 command builds the ordinary default C++ SDK twice in
fresh directories and tests a copied public consumer against each extracted
package. It requires a clean checkout at an explicitly expected full commit.
On Linux, with CMake, CPack, Make and GCC installed:

```sh
python3 tools/release_rebuild.py build --expected-commit "$(git rev-parse HEAD)" --work-directory /tmp/rtfw-local-rebuild --cc gcc --cxx g++
```

The work directory must be new and outside the source checkout. The command
retains both complete builds, packages, installed consumers, command logs and
`report.json`. It records observed source/tree, declared dependency verification,
tool paths/versions/digests, exact command arguments, CMake cache/compile-command/
CPack-config digests and package member inventories. `SOURCE_DATE_EPOCH` comes
from the source commit. Each command has a default 900-second limit; log size
is monitored at 100-ms intervals and stopped after exceeding 16 MiB, so the
retained failure log may exceed that threshold. Failed commands produce nonzero
exit and a failed report; existing output is never reused.

A successful result requires both installed consumer runs, unchanged observed
source/dependency/tool inputs, and matching complete archive hashes and member
inventories. File content, permissions, timestamps, ownership and link targets
are compared. A container-only difference is still a mismatch. Existing TGZ
archives can also be compared without executing builds:

```sh
python3 tools/release_rebuild.py compare first.tar.gz second.tar.gz
```

This is an unsigned observation of two local builds with recorded inputs. It is
not a hermetic build or an authenticated builder identity; host libraries and
unrecorded environment inputs remain outside its closure. Archive comparison
alone does not establish source provenance. Existing candidate provenance and
external signature fixtures remain unchanged. Original M27-06 signature-fixture
expansion is unperformed under the owner amendment; production identity, signing,
release approval and publication remain separate. No CAP-M20 completion follows.
