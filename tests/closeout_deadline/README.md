# Offline native deadline investigation

These Linux developer tools investigate the retained M27-07 `--frame` timeout.
They are not installed, linked into shipped Runtime targets, or substituted for
any original test. `../golden_xdma_native/test_native.cpp` and all its assertions
remain unchanged. The original uninstrumented source must be run separately.

`instrument.py` accepts only the reviewed DeviceManager SHA-256 and creates an
explicitly marked copy outside the checkout. It retains the exact patch and
source bindings. The copy adds fixed-capacity timestamp observations; it changes
no deadlines, capabilities, statuses or control flow other than optional test
scheduling delay. `trace.hpp` uses relaxed event reservations without adding
cross-lane synchronization. Each writer owns its record; output happens after
the original probe's checked destruction joins its owners. It uses no frame heap
allocation. Its clock reads and instrumentation still perturb execution.

Generate and build an ordinary diagnostic against a matching Runtime archive:

```sh
python3 tests/closeout_deadline/instrument.py --output /tmp/native-deadline-copy
g++ -std=c++20 -O1 -g1 -pthread -Irt/include -Iinclude -Icore/include \
  -Irt/src -Itests/closeout_deadline tests/closeout_deadline/main.cpp \
  tests/golden_cuda/allocation.cpp /tmp/native-deadline-copy/device_manager.cpp \
  rt/src/xdma_backend.cpp build/agent-full/librtfw_runtime.a \
  -o /tmp/native-deadline-probe
/tmp/native-deadline-probe --frame > /tmp/native-frame.log 2>&1
/tmp/native-deadline-probe --delay-submit > /tmp/native-delay.log 2>&1
python3 tests/closeout_deadline/analyze.py /tmp/native-frame.log /tmp/native-delay.log
```

The diagnostic object precedes the static Runtime archive, so its DeviceManager
replaces only that archive member in this offline executable. Ordinary and
sanitizer archives must match reviewed sources and instrumentation. The retained
M28-04 commands bind the archive/member hashes and unchanged Runtime/core tree;
fixture, allocation observer, XDMA backend and diagnostic manager are freshly
compiled. Never mix sanitizer modes or infer coverage from an uninstrumented
library. No diagnostic object belongs in CMake product or package targets.

`--frame` returns the original test's exit status. `--delay-submit` holds the
first **500us frame** submission for 2ms after Runtime acquires its slot; it does
not delay the separate 8ms startup submissions. Its expected original exit is 1,
with `device_timeout` and zero allocations. The analyzer independently checks
expiry during that hold, quarantine, no later successful finish for that batch,
and slot release after checked backend shutdown. This is a causal delay control,
not a passing original full-frame result. A first draft delayed startup instead;
its unsuccessful control remains in the evidence.

Records carry wall time and per-thread CPU time. Compare CPU deltas only on the
same thread. Source states: queued2, submitting3, submitted4, owned6, quarantine
owned9, quarantined10. `frame_queued` identifies the unchanged 500000ns budget;
other queue records include the original startup/stop safe-transition batches.
Batch IDs restart in each original Session; the analyzer tracks generations.
Original assertions, full logs, initialized sanitizer outcomes and pre-main
startup failures are retained separately. An actual sanitizer report requires
investigation; a timeout alone is not a race. Absence of a report is not proof of
race freedom. These observations do not establish controlled performance,
physical XDMA timing, RT qualification, or the cause of every historical failure.
