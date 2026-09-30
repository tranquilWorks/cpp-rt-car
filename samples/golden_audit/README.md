# Installed golden-system audit

Build an SDK with `-DRTFW_BUILD_BENCHMARKS=ON`. With Python 3, CMake and a C++20
compiler, run this installed kit using an explicit SDK prefix and new output:

```sh
python3 run.py --prefix /sdk --build "/tmp/golden build" \
  --output "/tmp/new golden evidence" --cycles 16 --clock steady
```

The sibling showcase builds through installed public targets. The command runs
its complete benchmark/lever/fault/external-process workflow and then 16 cycles
of 18 nominal variants plus six fault/recovery paths. This is a bounded software
regression soak, not physical endurance. Every state, replay, count, configuration,
source and executable binding is independently checked. Failed runs retain their
logs and incomplete journal and cannot produce a successful soak report.

```sh
python3 run.py --verify "/tmp/new golden evidence" \
  --binaries-build "/tmp/golden build"
```

Use the same explicit `--config Debug` for both commands with a multi-configuration
build. `--cycles` accepts 2 through 32; the default acceptance run uses 16.
`--clock fake` labels structural fixtures; `steady` labels portable timings.
Changing these options does not qualify a real-time or physical deployment.

The adjacent data-directory `golden_audit/index.html` manual includes exact source
snapshots, 100 frozen scenario mappings and 112 CAP-M15 through CAP-M26 requirement
dispositions. It works offline. Private source snapshots are documentation, not
additional public SDK APIs. M18 physical/RT, M19 Unreal, M20 release engineering,
M25 human review and protected findings remain explicit open actions.
