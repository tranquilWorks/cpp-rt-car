# Golden benchmark, control and fault showcase

This optional public-SDK kit executes fifteen `rtfw.golden` ProviderV1 cases,
all thirteen finite levers, all eleven frozen faults, CPU and simulated CUDA/
XDMA/combined variants, independent host jobs and real controller processes.

Install an SDK built with `-DRTFW_BUILD_BENCHMARKS=ON`, then run:

```sh
python3 run.py --prefix /path/to/sdk --output /path/to/new-evidence
```

Use `--clock steady` for portable host characterization. The default fake clock
produces structural evidence. Both retain two warmups and all five measured
samples. Neither establishes controlled performance or hardware qualification.
The output directory must be new. Failures retain their partial evidence.

The SDK must include `runtime`, `cuda_backend`, `xdma_backend`, and `benchmark`.
No GPU, FPGA, driver, device node or privileged access is needed or probed.
To embed source, configure this folder with `-DGOLDEN_RTFW_SOURCE=/checkout`.
The source owner, borrowed buffers, host jobs and drivers outlive checked stop.

For an existing build, use `--binaries-build /path/to/build`. Offline verification
requires the same seven executable files and the source provenance:

```sh
python3 report.py /path/to/evidence --binaries-build build --provenance provenance.json
```

See the installed `golden_showcase.md` guide for timing scopes, requested/effective
settings, capacity outcomes, telemetry loss and the remaining M26-06 audit.

Use `--config Debug` or `--config RelWithDebInfo` when selecting an existing
multi-configuration build. CTest passes its selected CMake configuration. Without
`--config`, discovery accepts exactly one matching executable and rejects ambiguous
builds; a requested missing configuration never falls back to another one.
