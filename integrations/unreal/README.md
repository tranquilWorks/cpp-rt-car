# Unreal integration source candidate

See [implementation and validation guide](../../docs/unreal_adapter.md).
The optional plugin, shared portable orchestration, fifteen commandlet tests,
real two-instance PIE automation and opt-in engine sample are supplied as source.
Actual current Unreal builds/automation/PIE/package validation are NOT RUN.

Prepare a disposable project without Unreal:

```sh
python3 integrations/unreal/prepare.py stage --sdk-root /absolute/sdk --output /new/stage
```

Later run `scripts/verify-unreal.sh --help` on the existing exact engine tuple.
The installed kit includes `prepare.py`, `run.py`, plugin, portable headers and
project template under `share/rtfw/integrations/unreal`. Scripts refuse existing
outputs. No SDK target/header, Runtime source, release or support claim changes.
