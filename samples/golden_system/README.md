# Portable golden-system reference

The M26-02 source kit executes the frozen [M26-01 contract](contract.json):
256-capacity integer SoA state, eight phases, five active rates/channels, native or
independent host jobs, typed controls, checkpoint recovery and trusted active replay.
An optional separate controller uses the sample-owned vector shared-memory protocol.
CUDA/XDMA, the lever showcase and final audit remain later batches.

From an installed SDK (adjust the data directory if customized):

```sh
python3 /path/to/sdk/share/rtfw/examples/golden_system/run.py \
  --prefix /path/to/sdk --build ./golden-build --output ./golden-evidence --mode external
```

Use `--mode native` or `--mode host` for internal control. The runner builds against
public installed targets and validates every state field independently. A successful
24-tick default run executes108 callbacks and replays24 frames. `run.json`,
`state.bin`, `replay.json`, raw Runtime artifacts and execution diagnostics retain
source/configuration/contract identity and checked cleanup. See the installed
`golden_system.md` guide two directories above this directory.

Direct executable options include `--count 1..256`, `--ticks 1..1024`,
`--workers 1..3`, `--grain 1|4|16|64`, `--campaign NAME` and `--output DIRECTORY`.
Campaigns are `nominal`, `overload`, `stale_input`, `control_rejected`,
`control_replaced` and `peer_missing`. Non-nominal campaigns require at least19
ticks. External process supervision is handled by the runner. A failed peer or
missing stop acknowledgement remains a failed invocation with retained evidence.

Source embedding uses `-DGOLDEN_RTFW_SOURCE=/path/to/checkout`. From a repository
checkout, the root build supplies `sample_golden_system`,
`sample_golden_controller` and `golden_system_*` CTests. Frozen constant validation:

```sh
python3 tools/check_golden_contract.py
python3 tools/generate_golden_config.py --check
```

The design checker still reports `scenario_executed: false`: it validates the
immutable specification rather than manufacturing execution evidence. Domain
budgets are divided among their phases before public Runtime registration.
Active replay stays bound to its originating owner; fresh-owner recovery uses a
paired checkpoint. External control uses the last completed sensor snapshot, so
its explicit pipeline delay has its own numerical oracle. Checksums are corruption
checks, not peer authentication. Linux/Windows portable evidence does not establish
hardware, real-time, performance, Unreal or release qualification.
