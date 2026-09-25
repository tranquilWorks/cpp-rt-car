# Benchmark analysis and extension kit

M23-05 adds an offline Python standard-library tool and a complete C++ provider
source example. It preserves the original four-provider, 207-case runner and
all version-1 bundles. The supported release and stable ABIs remain unchanged.
No hardware measurement, threshold approval, profiler capture or qualification
is supplied by these tools or their synthetic tests.

## Try the complete portable workflow

```sh
cmake -S . -B build/analysis -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DRTFW_BUILD_EXPERIMENTAL=OFF -DRTFW_BUILD_BENCHMARKS=ON
cmake --build build/analysis --target rtfw-bench-template --parallel 2
mkdir -p build/analysis-evidence
build/analysis/bench/rtfw-bench-template --clock fake --output build/analysis-evidence/trial-01
python3 tools/check_benchmark_artifact.py --artifact-root build/analysis-evidence/trial-01
python3 tools/compare_benchmarks.py capture --role baseline --policy bench/analysis/portable-policy.json --plan bench/analysis/example-plan.json --run-root build/analysis-evidence --output build/analysis-evidence/baseline.json
python3 tools/compare_benchmarks.py capture --role candidate --policy bench/analysis/portable-policy.json --plan bench/analysis/example-plan.json --run-root build/analysis-evidence --output build/analysis-evidence/candidate.json
python3 tools/compare_benchmarks.py compare --policy bench/analysis/portable-policy.json --baseline build/analysis-evidence/baseline.json --candidate build/analysis-evidence/candidate.json --baseline-root build/analysis-evidence --candidate-root build/analysis-evidence --output build/analysis-evidence/report.json
python3 tools/compare_benchmarks.py compare --policy bench/analysis/portable-policy.json --baseline build/analysis-evidence/baseline.json --candidate build/analysis-evidence/candidate.json --baseline-root build/analysis-evidence --candidate-root build/analysis-evidence --format markdown --output build/analysis-evidence/report.md
```

This intentionally compares one fake-clock bundle to itself to demonstrate
structural matching. It cannot establish timing, independence or a performance
delta. Actual steady comparisons must use distinct independently collected runs.
Output files must be new and their parent directories must exist. One report
file is written atomically without replacing any existing file. Source bundles,
manifests, policies and reviews are never updated. Unsupported hard-link output
filesystems fail; there is no overwrite fallback.

## Plans, manifests and compatibility

`bench/analysis/contracts.schema.json` defines closed version-1 `$defs` for
`policy`, `plan`, `manifest`, `review`, `profiler` and `report`. Each is validated
by the tool using the unchanged offline validator's schema vocabulary. JSON
keys cannot repeat; floats, nonfinite numbers, unknown fields and versions are
rejected. Output JSON is canonical, integer/rational-only, with a final newline.
Input document hashes bind the exact bytes, including formatting.

A run plan declares collection controls, unique trial IDs, relative bundle
directories and an exact public command-token array for each run. Tokens are
records, never executed commands. Use portable executable names and relative
output tokens; keep private paths, environment expansions and payloads out of
the plan. Retain the actual command in your experiment records. `capture`
validates every existing bundle and binds all three file digests and the exact
policy bytes. Relocation changes only the supplied root, not those references.
Captured wall-clock time is a local observation, not a trusted timestamp.

Limits are 256 declared cases, 512 run entries per manifest, 64 runs per case
per population, 10,000 samples per run, 32 MiB per input/output file and 512 MiB
of bundle bytes across a comparison. These are offline host allocations.
Absent cases remain report rows. Invalid files abort with exit 2 and no report;
valid failed or NOT RUN evidence remains visible without a successful delta.

Descriptors must match completely, including provider version, configuration,
workload digest, parameters, repetitions, warmup and ordered counter inventory.
Correctness-token sequences and explicitly selected `invariant_counters` must
match sample by sample across runs. All other counter totals remain visible;
polling or scheduler counters need not be declared invariant. A missing named
counter fails comparison. No counter value is invented, normalized or dropped.

All identity fields must match except source commit/tree. Each population must
contain exactly one source commit/tree. Steady comparisons require clean,
known source identities and all host/compiler/build/backend/thread/memory facts;
only the frozen v1 unavailable temperature/frequency fields are exempt. For a
CPU-only backend use an explicit `not-applicable` driver label, not a missing
observation. Collection host labels must match the bundle identities. Changed
compiler flags, host, kernel, CPU, policy, backend or driver require a separate
baseline. Automatic CLI capture leaves some facts unavailable; supply honest
caller-declared labels through your host/provider when collecting comparable
runs. A sanitized label is not proof of a controlled machine.

## Statistical policy

The analyzed quantity is the population median of independent **run medians**.
Within-run repeated invocations are not independent experimental replicates.
Use fresh process runs under a predeclared collection protocol. Independence,
stationarity, thermal handling, affinity, frequency and background load remain
external declarations. The tool does not prove them or change host policy.

For each population with `n` independent runs, sort its run medians. Choose the
largest positive `k` for which

`2 * sum(comb(n, i), i=0..k-1) / 2**n <= (1-confidence)/(2*declared_cases)`.

The interval is `[X[k], X[n-k+1]]` using one-based order statistics. If no finite
interval meets the requested coverage, the result is inconclusive. This is the
non-interpolated binomial order-statistic construction described in
[NIST TN 2119](https://nvlpubs.nist.gov/nistpubs/TechnicalNotes/NIST.TN.2119.pdf).
The additional union-bound allocation covers both populations and every
predeclared case. It does not require independence across cases. The IID-run
assumption is still necessary; autocorrelated trials cannot claim this coverage.

Candidate/baseline bounds are `[candidate_low/baseline_high,
candidate_high/baseline_low]`. Exact rational arithmetic avoids rounding a
threshold decision. Reports retain each run's p50/p95/p99/max, totals and sample
count, but make no confidence or release-threshold claim about tail percentiles.
Even-sized medians average the middle observations exactly. Zero or insufficient
populations are inconclusive. Minimum counts and the entire case family are
predeclared; failed/missing cases never reduce the multiple-comparison budget.

No raw sample is removed. Apply the same two rules to every run's durations
and to each population of run medians:

- Median absolute deviation divided by median must not exceed `max_mad_ppm`.
- An observation whose absolute deviation exceeds `outlier_relative_ppm` times
  the median is flagged; any flagged observation makes the result inconclusive.

Both limits are integer parts per million. Zero MAD does not suppress a distant
outlier. Limits at equality are accepted. These are explicit data-quality rules,
not a promise that unflagged data is independent or stationary. Correct the
collection problem and retain a new complete experiment; do not trim spikes or
keep rerunning until a favorable result appears.

## Threshold review

Portable policies require null thresholds. Valid steady results are descriptive
`characterization`, never a release pass/fail. Fake clocks produce
`structural_match` only in portable mode; controlled mode remains inconclusive.
CI tests structural behavior under generous CTest timeouts, not latency budgets.

For controlled mode, predeclare a nonnegative regression allowance for each
case, as parts per million. After collecting the baseline, generate a proposal:

```sh
python3 tools/compare_benchmarks.py review-proposal --baseline baseline.json --policy controlled-policy.json --output review-proposal.json
```

It is always `proposal_only`, with no approver or approval time. A responsible
reviewer must independently assess the baseline, policy, case family, collection
controls and proposed allowance, then provide a separate review artifact.
`compare --review review.json` checks its exact baseline/policy/collection
digests, explicit decision and reviewer declaration. Approval must be at or
after baseline capture and strictly before **every** candidate trial. Changed
thresholds or reformatting the policy invalidate its old binding. Missing,
rejected or proposal-only reviews cannot yield a controlled pass.

The tool does not authenticate people, signatures or timestamps. Reports label
the review `external_unverified_declaration`; operational approval and controlled
runner evidence remain the owner's responsibility. No real threshold approval
is checked into this kit. Synthetic approvals in tests exercise parsing only.

A reviewed case passes when its upper ratio bound is at or below `1+allowance`;
it regresses when its lower bound is strictly above that value. Overlap is
inconclusive. These conclusions apply only to the declared median metric.
No report changes the original `portable_characterization` evidence class,
support matrix, RT tier or release state. Every report says qualification none.

| Exit | Meaning |
| --- | --- |
| 0 | Document created, portable structural match/characterization, or reviewed controlled pass |
| 1 | Reviewed regression |
| 2 | Invalid/incompatible evidence or a failed run |
| 3 | Valid NOT RUN evidence |
| 4 | Inconclusive comparison or missing threshold review |

Check `mode` and `status` together, not exit 0 alone. Aggregation prioritizes
failed, incompatible, regression, NOT RUN, inconclusive, characterization,
structural match, then pass. Each case and run remains available for inspection.

## Profiler correlation and remediation

Optional `--profiler correlations.json` consumes the `profiler` schema. A link
names provider/case, the exact result-file and run-context hashes, tool/version,
retained trace digest, inclusive sample-index range and clock domain. A declared
same-clock or mapped-clock relationship requires a separate correlation-record
digest; otherwise mark it `uncorrelated` with a null correlation record. Missing
links remain an empty array and null profiler digest. Trace bytes are not opened,
interpreted or verified here; these are externally retained references, not a
claim that a capture exists or explains a slowdown.

Start with the exact run and case showing an admissible change. Reproduce its
command and public identity on the named host. Capture a separately identified
profiler run, retaining instrumentation effects and clock mapping. Correlate the
relevant sample window, inspect the suspected subsystem, make a bounded change,
then collect an uninstrumented candidate under the original reviewed policy.
Do not combine profiler-instrumented and ordinary timings under one identity or
infer causality from a time overlap alone.

## Extend a third-party subsystem

`bench/extension` is a complete two-case integer-transform provider and CLI:
64/1024 elements, fixed per-owner storage, metadata-only description, explicit
prepare and checked finish, per-element oracle, exact byte/element counters and
an independent sum checksum. Each invocation includes input fill, transform and
validation. It does not claim transform-only timing or allocation-free runner
serialization. Source callbacks and their user object stay borrowed until
successful finish. Failed invokes poison the session; failed cleanup retains
ownership and disallows further invocation until cleanup succeeds. The host
must keep the provider/user alive and retry finish. Missing prerequisites yield
NOT RUN before warmup. Repeated runs require finish then prepare.

`run_to_directory` is the actual CLI transaction used by failure tests. It
checks the destination before preparation, settles ownership before publishing,
and never writes a successful bundle after a transform or cleanup failure.
Independent owners share no mutable state. The default transform uses no device
or extra thread; concurrency tests create threads only in their host harness.
Replace the callback with your workload and retain an independent oracle.

The optional install places the source under `share/rtfw/bench/examples/extension`:

```sh
cmake --install build/analysis --prefix build/analysis-install
cmake -S build/analysis-install/share/rtfw/bench/examples/extension -B build/my-extension -DCMAKE_PREFIX_PATH="$PWD/build/analysis-install"
cmake --build build/my-extension --config Release --parallel 2
```

Build the full optional project before installing it. `rtfw::benchmark` is the
only linked SDK target; no Runtime-private header or runner modification is
needed. The template is source-owned, not an exported provider ABI or default
header. The analysis scripts, schemas and guide are installed only with the
benchmark component. Relocated, archived, unrequested and add-subdirectory
consumers retain all predecessor package checks.
