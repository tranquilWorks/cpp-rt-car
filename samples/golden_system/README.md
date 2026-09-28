# Golden-system specification

M26-01 freezes [contract.json](contract.json) before the runtime sample is
implemented. This directory has no executable or CMake target yet. The
[design guide](../../docs/golden_system.md) explains ownership, phases, channels,
controls, numerical rules, fault expectations and the later delivery batches.

From the repository root:

```sh
python3 tools/check_golden_contract.py
python3 -m unittest discover -s tests/golden_system -p 'test_*.py'
```

Expected validation summary: 27 reference records per six-tick supercycle,
108 phase calls over the default 24 ticks, 11 planned faults and 15 planned
benchmark cases. The validator reports `scenario_executed: false`. These are
calculated design properties, not measured Runtime execution results.

The canonical contract digest uses sorted-key, compact ASCII JSON. The validator
pins it; any semantic or prose change requires an explicit reviewed identity
update. JSON whitespace is immaterial. Version 1 and scenario identity are
frozen for the first implementation; incompatible changes need a new version
and updated consumers/artifacts. Do not quietly update the digest to mask drift.
