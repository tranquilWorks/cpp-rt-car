# Qualification records and promotion proposals

RTFW qualification is an offline, human-governed evidence process. M18-01
defines four independent JSON Schema draft 2020-12 domains at schema version 1
and a Python 3.11 standard-library validator. It does not run a device, change a
support matrix, authenticate a reviewer, sign an artifact, or qualify NVIDIA,
XDMA, combined, RT1, or RT2 behavior.

## Evidence states

| State | Meaning |
| --- | --- |
| `synthetic_fixture` | Deterministic parser and proposal test input. It is never support-matrix eligible. |
| Portable CI | Build, unit, sanitizer, package, and protocol evidence on named CI environments. It is not physical or latency evidence. |
| Raw `evidence_only` | M12 CUDA/XDMA sample output plus a source-bound artifact manifest. It may be retained inside an M18 record but is not a record or threshold result itself. |
| Qualification record | Complete run output bound to a separately retained plan, observed tuple, raw artifacts, trials, trends, and threshold evaluations. |
| Passing review | Human attribution and decision bound to the exact plan, record, and embedded artifact-manifest digest. The name is not authenticated. |
| Promotion proposal | Deterministic `proposal_only` handoff labeled `human_matrix_change_required`. A human must still edit and review the appropriate matrix. |

M18-01 produces only synthetic fixtures and offline tooling. No support tuple is
promoted. M17-05 remains blocked by native command-capability discovery, so the
tool rejects every non-synthetic combined proposal. The combined synthetic
fixture exercises schema composition only and cannot establish the missing
runtime behavior.

## Files and independent versions

- `qualification/schemas/campaign-plan.schema.json`: pre-run campaign intent;
- `qualification/schemas/qualification-record.schema.json`: immutable run
  output;
- `qualification/schemas/promotion-review.schema.json`: separate human
  decision metadata;
- `qualification/schemas/promotion-proposal.schema.json`: generated handoff;
- `tools/qualification.py`: bounded semantic, artifact, and digest validation.

Each document has `schema_version: 1` and an exact `document_type`. These
versions do not change the package, C ABI, device ABI, runtime-profile,
observability, checkpoint, input-log, rate-action, release-contract, or support
matrix schemas. An incompatible qualification-format change requires a new
qualification schema version and retained readers for records still under
review. Additive changes cannot weaken version-1 required fields or reinterpret
their meanings.

## Campaign-plan contract

The plan is retained separately before measurement. It contains the immutable
campaign, tuple, scope, full 40-character lowercase source commit, runtime build
and product version; CPU, motherboard, BIOS/firmware, memory, OS, kernel,
configuration digest, boot parameters, compiler; NUMA, IOMMU, IRQ, and power
topology; resolved runtime/thread/memory/time policy; workload and input digest;
warm-up, duration, and sample bounds; scope-specific accelerator identity; all
seven trial categories; and every threshold definition.

Threshold identities are unique. A definition contains exact metric, unit,
population, statistic, comparison, numeric bound, miss allowance, and error
allowance. The record must repeat the complete definition byte-semantically;
renaming, adding, removing, or changing a threshold fails validation.

| Scope | Required tuple and policy facts |
| --- | --- |
| `nvidia` | GPU, PCI BDF/link, CUDA driver/toolkit, firmware, power, and clock policy. |
| `xdma` | FPGA part, PCI BDF/link, driver revision/module parameters, firmware-or-bitstream SHA-256, IP configuration, and memory map. |
| `combined` | Both identities and an explicit bounded host-staging path with `direct_peer_dma: false`. |
| `rt1` | A named measured host policy and explicit latency and deadline populations. Device-loss/reset-rebind are retained as explicit non-applicable rows only when no device is in scope. |
| `rt2` | RT1 facts plus PREEMPT_RT, kernel/config/boot, CPU isolation, IRQ placement, scheduling, and locking evidence. |

Functional, endurance, thermal, saturation, and shutdown trials are always
mandatory. Device-loss and reset-rebind are mandatory for device scopes. RT2
is optional and cannot be inferred from RT1, portable CI, or strict preflight.

## Record, artifact, and review binding

The record carries the SHA-256 of the exact plan bytes. It repeats the exact
source, build, host, topology, resolved policy, workload, scope, tuple, and
accelerator facts; records a UTC run interval; references all raw release,
wake, compute, submit, poll, completion, slack, miss, temperature, clock,
power, memory, queue, error, and recovery populations; and records resource,
thermal, health, recovery, trial, threshold, and overall results.

The embedded artifact manifest is a strictly sorted list of canonical POSIX
relative paths, byte counts, and lowercase SHA-256 values. Its digest is:

```text
SHA-256("rtfw-qualification-artifact-manifest-v1\\0" || canonical-json(entries))
```

Canonical JSON uses UTF-8 ASCII escapes, lexically sorted keys, no insignificant
whitespace, and exactly one trailing newline. Plan, record, and review digests
instead cover their exact retained bytes, including whitespace.

Every listed artifact must be a regular file below one explicit root. The
validator rejects absolute, escaping, dot-segment, backslash, colon, control,
case-fold-colliding, non-normalized-colliding, duplicate, or unsorted paths;
symlinks at any level; nonregular, missing, unlisted, oversized, size-mismatched,
or digest-mismatched files. Diagnostics name paths and failures but never dump
artifact content.

A passing record has the exact planned trial and threshold identity sets, no
failed mandatory trial, no unsupported applicable trial, stable resource and
thermal trends, healthy recovered device state with no unresolved loss, and
threshold results recomputed from the predeclared comparison and allowances.
Every trend maximum must cover its start and end values, a stable resource
trend cannot finish above its starting value, and every thermal start, end,
and maximum must remain in the version-1 range from -100 through 300. There is
no waiver path in a generated proposal.

The separate review binds the exact plan, record, and embedded manifest
digests, scope, tuple, reviewer attribution, decision, rationale, and
exceptions. Proposal generation requires `decision: pass`, no exception, and
explicit human confirmation that external evidence showed the plan was
retained before the run. The timestamp and reviewer string remain untrusted
metadata: the tool proves neither chronology nor identity, and
`reviewer_authentication` remains `attribution_only` until later signing and
identity governance.

## Bounded parser and artifact ceilings

| Bound | Version-1 value |
| --- | ---: |
| Plan, record, review, or proposal JSON | 1 MiB each |
| JSON lexical nesting | 32 levels |
| String | 4,096 characters globally; tighter schema limits apply |
| Generic collection | 4,096 items |
| Reported validation errors | 128 plus one omission notice |
| Trials / thresholds / review exceptions | 64 / 128 / 32 |
| Warm-up or samples | 10,000,000 |
| Duration | 31,536,000 seconds |
| Artifact count | 256 |
| One artifact / aggregate artifacts | 64 MiB / 512 MiB |
| Artifact tree entries / depth | 2,304 / 8 components |

Input is strict UTF-8 with no BOM. Duplicate keys, trailing JSON, booleans used
as integers, `NaN`, infinity, excessive depth, nonobjects, unknown properties,
and noncanonical lowercase commit/digest identities fail closed. Validation is
offline and has no network, hardware, Git, credential, or remote-system path.
Timeout is not applicable because the tool starts no work and waits on no
external service; bounded bytes, collection counts, and streaming artifact-tree
enumeration constrain local work.

## Commands and output safety

```bash
python3 tools/qualification.py validate \
  --plan <campaign-plan.json> \
  --record <qualification-record.json> \
  --review <promotion-review.json> \
  --artifact-dir <retained-artifact-root>

python3 tools/qualification.py propose \
  --plan <campaign-plan.json> \
  --record <qualification-record.json> \
  --review <promotion-review.json> \
  --artifact-dir <retained-artifact-root> \
  --output <new-promotion-proposal.json>

python3 tools/qualification.py verify-proposal \
  --proposal <promotion-proposal.json> \
  --plan <campaign-plan.json> \
  --record <qualification-record.json> \
  --review <promotion-review.json> \
  --artifact-dir <retained-artifact-root>
```

All inputs and artifacts validate before output begins. `propose` requires an
explicit nonexisting file outside the artifact root and distinct from every
input. It writes and flushes a same-directory temporary file, publishes it with
an atomic non-overwriting hard link, flushes the directory where the platform
exposes directory `fsync`, and removes the temporary file on success, error, or
cancellation. Existing output is never
overwritten. Identical inputs produce byte-identical proposal bytes;
`verify-proposal` compares the exact canonical bytes.

The tool has no matrix path or mutation command. It never edits a support
matrix, release contract, plan, record, review, raw artifact, Git state, remote
service, hardware, firmware, driver, signing system, deployment, or production
setting.

## Review and rollback

Before matrix promotion, a human must inspect the schema/security result, the
exact tuple and workload, externally retained pre-run plan provenance, raw
artifacts, failed and recovery trials, resource stability, threshold math,
claim wording, and the separate matrix diff. Proposal generation alone is not
approval. Matrix changes remain separately reviewed and are absent from
M18-01.

To roll back M18-01, revert the four schemas, tool, synthetic fixtures/tests,
qualification documentation, checker integration, digest ledger, and retained
M18-01 evidence together. Retain version-1 readers when an external record is
still under review, or archive that record explicitly as non-promotable. Never
rewrite raw evidence. There is no runtime state, data migration, hardware
mutation, release, deployment, or support-matrix entry to undo.
