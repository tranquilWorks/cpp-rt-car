# Golden XDMA and combined source kit

Run the frozen M26 integer scenario through Runtime sampled-I/O and actual native
XDMA workers with an owned simulated card driver. `cpu` selects CPU physics;
`kernel` and `graph` add the native CUDA backend with its owned simulated driver.
Both backends remain unmodified. All results are injected-driver protocol
evidence; physical XDMA/CUDA/HIL and RT qualification are **NOT_RUN**.

From this installed directory, with Python 3, CMake and a C++20 compiler:

```sh
python3 run.py --prefix "/path/to/SDK with spaces" --build "/tmp/golden build" --output "/tmp/golden evidence" --mode external --dispatch graph
```

Use `--mode native` or `--mode host` for native Runtime workers or independent
host jobs. `--fault underflow|overrun|stop_failure` exercises the frozen tick-6
sampled faults; `device_loss|reset_failure` exercises the CUDA side of combined
execution. The default is `none`. Faults require a nominal, nonexternal run of
at least 19 ticks. `--campaign` preserves the CPU kit's six campaigns.
`golden_xdma` itself accepts `--count 1..256 --ticks 1..1024 --workers 1..3
--grain 1|4|16|64`. Run `artifacts.py DIRECTORY --provenance provenance.json
--publish` after direct CLI execution to independently validate and publish the
paired run/replay summaries. The validator rejects changed source hashes,
configuration, state, transcripts, counters, safety and physical claims.

`CMakeLists.txt` supports public `find_package(rtfw)` and
`-DGOLDEN_RTFW_SOURCE=/path/to/checkout` embedding. Keep the installed
`golden_system` and `golden_cuda` sibling directories intact. Provenance hashes
every regular source file in all three kits. The original sibling files are
reused unchanged; private `session`, `runner`, `replay`, `telemetry`, `oracle`,
`state.py`, build and script files derive their structure from those kits.
`io.hpp` derives the complete HAL forwarding pattern from CUDA's adapter and
accepts only this kit's typed owned simulated driver. Explicit native-policy
inspection disables both the mock capability and simulator watchdog.

The graph has eight logical phases and ten physical callbacks, with separate
sensor and actuator completions. Four actual sampled channels use ring capacity
4 and 120-byte public headers; the ordinary plant aggregate remains CPU-only.
The codec explicitly adapts the distinct frozen 120-byte application envelope.
The native CUDA queue has capacity 1, XDMA capacity 2, and only combined execution
selects aggregate Runtime reservation 3 with per-backend admission. Device
completion budgets are 500 microseconds; the simulator host watchdog is finite
at five seconds and safe-transition acknowledgement stays at eight milliseconds.
These logical values are not measured hardware timing claims.

Underflow omits commands at ticks 6 and 9, transfers zero effort, and clears at
12; trusted replay uses the same owner. Overrun rejects a duplicate publication
without overwriting it. Missing stop acknowledgement leaves safety unknown and
retains Runtime, borrowed buffers, drivers, jobs and memory until checked retry.
Overload, overrun, stop failure and CUDA faults recover from the compatible tick-5
checkpoint into a fresh owner and replay the suffix beginning at tick 6. Trusted
replay always belongs to its originating Runtime; identities are never rewritten.

`execution.json` reports execution telemetry before replay. Driver counters sum
execution, replay and lifecycle across all owners. Sampled/control counters are
from the final owner before replay, including restored checkpoint state. Logical
phase counts describe the successful scenario; physical callback totals include
the two completion callbacks. Failed-prefix telemetry is retained separately and
added to execution totals. `state.bin`, `checkpoint.bin`, `active.bin` and
`trusted.bin` retain their original bytes and pairing. `run.json` and `replay.json`
are independently derived summaries, not substitutes for those artifacts.
