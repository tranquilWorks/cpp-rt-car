# New full-horizon sanitizer harness budget

The complete new 1024-tick XDMA parity test retains all frames, validates every
state field, and performs full trusted replay in its originating Runtime owner.
The initial new 120-second process harness timed out for CPU and kernel TSan
variants. A diagnostic repeat completed the unchanged O1 kernel case in121.09s
against the Debug/O0 repaired Runtime, with all assertions and sanitizer checks.
An O2 fixture experiment also timed out; it is retained but not adopted.

Only the new maximum-horizon CTest cases9/18 receive a300-second timeout when
SIM_SANITIZERS is selected. Other new tests keep120, ordinary long cases keep120,
and every prior test and timeout is preserved. This is a functional sanitizer
process budget, not a relaxation of the frozen safe8ms logical/backend timeout,
the explicit5s simulator watchdog, or any Runtime deadline. No workload, replay,
allocation instrumentation or assertion is removed. These tests make no host
latency or controlled-performance claim. Fresh complete local/hosted sanitizer
gates must pass; earlier failed attempts remain failures in retained evidence.

The new test fixture emits an entry marker before parsing or executing cases.
Local sanitizer startup retries cannot classify a crash after that marker as
an empty before-main loader failure; any entered functional failure stops.
