[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Runtime Profiles

RTFW 1.1+ provides a bounded host-side loader for complete `rt::RuntimeConfig`
profiles. This is an implemented RT0 control-plane surface. It does not make
profile file I/O an RT-lane operation, discover hardware automatically, or
turn an autotune result into a latency qualification.

## Public API

Include `<rt/profile.hpp>` and call:

```cpp
rt::RuntimeConfig config;
rt::RuntimeProfileMetadata metadata;
rt::RuntimeProfileError error;

const rt::Status status =
    rt::parse_runtime_profile(json, config, metadata, error);
```

The caller owns the JSON bytes and performs any file I/O before starting the
runtime. Parsing is allocation-free, accepts at most 64 KiB, validates UTF-8
and JSON with at most 16 nested levels, and is transactional: `config` and
`metadata` are unchanged on failure. Error code, byte offset, and a bounded
contract path identify the rejection.

This is a C++ source API. Stable C ABI v8 and the device ABI are unchanged in
1.2.

## Schema and compatibility

[`tools/autotune/config.schema.json`](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/tools/autotune/config.schema.json) is
the authoring schema. A profile contains:

- `schema_version`: profile envelope schema, currently `1`;
- `profile_id`: a restricted 1–63 byte provenance identifier;
- `runtime_compatibility.major`: required runtime package major;
- `runtime_compatibility.minimum_minor`: oldest required minor in that major;
- `runtime_config_schema`: the exact typed runtime configuration schema,
  currently `7`;
- `runtime`: all 25 `RuntimeConfig` fields, resolved and type checked;
- optional `params`: an opaque JSON object retained as experiment provenance.

Profiles are complete configurations, not overlays. Missing runtime fields,
unknown or duplicate contract keys, wrong JSON types, invalid cross-field
combinations, incompatible schemas/versions, malformed input, and trailing
bytes fail closed. The runtime ignores `params` after bounded JSON validation.
The C++ validator remains authoritative for power-of-two and cross-field rules
that JSON Schema cannot express directly.

A 1.x runtime accepts schema-compatible profiles whose required major equals
its own and whose `minimum_minor` is not newer than the runtime. A newer
profile schema, runtime-config schema, package major, or minimum package minor
returns `rt::Status::incompatible_artifact`.

`profile_id` is provenance, not authentication. The checked generator derives
it from the complete resolved profile, but the loader permits deliberately
named hand-authored profiles. Sign or authenticate profiles in the deployment
control plane when tampering is in scope.

## Profile-driven demo

`rtfw_runtime_demo` is the production-path integration target. It loads a
profile, constructs four independent physics phases plus a dependency barrier,
uses nested `parallel_for()` work, and emits end-to-end frame timing and
executor counters:

```bash
./build/rtfw_runtime_demo \
  --config configs/default_safe.json \
  --run 1s \
  --rt \
  --metrics-json-interval
```

`--config` and `--profile` are aliases. If neither is present,
`RTFW_PROFILE` supplies the path. An explicit path takes precedence over the
environment. `--threads` is a demo-only final worker-count override and is
reported as `worker_override: true`; the library parser itself never merges or
silently overrides fields.

The `host_adapter` executor policy can be loaded by an embedding application
that attaches its own adapter, but the standalone demo rejects it because it
has no borrowed engine job system.

The runtime profile configures device capacities; it does not load a driver,
register a device backend, or select CUDA/XDMA hardware. Applications still
perform those lifecycle operations explicitly.

## Checked fixtures

- `configs/default.json`: small native throughput profile;
- `configs/default_fast.json`: wider illustrative throughput profile;
- `configs/default_safe.json`: single-worker static profile used by CI;
- `profiles/example-linux.json`: install-format example, not a host
  recommendation or hardware qualification record.

Generate profiles through
[`tools/autotune/make_config.py`](https://github.com/tranquilWorks/cpp-rt-car/blob/912ce878ad311d38e452a5d2916ee0daa6ee3f72/tools/autotune/make_config.py). The
production factor space intentionally exposes only supported runtime knobs:
`worker_count`, `executor_policy`, and `executor_queue_capacity`.
