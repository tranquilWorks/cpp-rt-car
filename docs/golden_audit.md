# Golden system and feature-completion audit

Start with the [offline source-linked manual](golden_audit/generated/index.html).
It covers all 100 frozen scenario mapping entries and all 112 canonical
CAP-M15 through CAP-M26 scenario, automated-evidence, manual-gate and claim-limit
requirements. Each disposition links to actual source, tests and retained evidence.

The [canonical requirement snapshot](golden_audit/requirements.yaml) is bound to
control plan579; the original requirements and frozen scenario are unchanged.
The [capability audit](golden_audit/generated/coverage.json) distinguishes portable
implementation, remaining engineering and unperformed gates. It is not a single
percentage or blanket qualification claim.

The default SDK ships the manual under its data directory at
`rtfw/golden_audit/index.html`. A benchmark-enabled SDK also ships
`rtfw/examples/golden_audit/run.py`. One command builds the preserved public kits,
runs the full showcase, and executes the bounded multi-variant soak:

```sh
python3 /sdk/share/rtfw/examples/golden_audit/run.py --prefix /sdk \
  --build "/tmp/golden build" --output "/tmp/new golden evidence" \
  --cycles 16 --clock steady
```

The default 16 cycles run 288 nominal and 96 fault/recovery sessions in addition
to the complete showcase. Nominal work totals 6,912 logical ticks. The report
retains measured wall duration separately; this is portable regression coverage,
not long-duration thermal/endurance, deadline or controlled-performance evidence.
Every external variant starts a real controller process, every owner uses checked
cleanup, and the validator recomputes full state, paired replay and counters.

Revalidate with the same source and executables:

```sh
python3 /sdk/share/rtfw/examples/golden_audit/run.py \
  --verify "/tmp/new golden evidence" --binaries-build "/tmp/golden build"
```

Missing cycles, failed records, changed source/binaries, forged reports, altered
state/counters and incomplete cleanup fail validation. The output directory must
be new; failed prefixes remain available for diagnosis. Multi-configuration builds
use the same explicit `--config` in both commands. Missing optional components
fail rather than silently substituting a different workload.

M26 portable composition and audit delivery do not complete every capability.
Remaining work includes M18 physical/RT and native XDMA host integration as needed,
M19 Unreal, M20 controlled performance/security/signing/native telemetry exporters/
platform/migration/release, M25 unfamiliar-consumer review, and the retained
timing-sensitive fixture and foreign-replay findings. The manual lists their
concrete next actions. No subsequent batch or qualification activity is activated.
