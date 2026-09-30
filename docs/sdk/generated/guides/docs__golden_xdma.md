[Manual](../README.md) · [Recipes](../recipes.md) · [API reference](../api/index.html)

> Archived authoritative guide. Local links open shipped material; HTTPS links identify repository-only or external material. Commands requiring a checkout, optional component or real device retain those prerequisites. Historical evidence is not new qualification.

# Golden XDMA sampled-I/O reference

The installed `examples/golden_xdma` kit executes the frozen integer sensor and
actuator scenario through Runtime sampled-I/O, native XDMA worker batches and an
owned simulated card. Choose CPU physics or combined CUDA kernel/Graph physics,
native workers or independent host jobs, and an actual external CIL controller.
The kit is portable injected-driver protocol evidence. Physical hardware, HIL,
RT1/RT2, controlled performance, Unreal and release qualification remain NOT_RUN.

```sh
python3 "/sdk/custom/data/rtfw/examples/golden_xdma/run.py" \
  --prefix "/sdk" --build "/tmp/golden build" --output "/tmp/golden evidence" \
  --mode external --dispatch graph
```

See the installed kit's README for complete configuration, graph budgets,
ownership, fault/recovery boundaries, provenance and counter scopes. Four actual
sampled channels retain frozen identities 26001..26004, ring capacity 4 and
120-byte headers; plant_aggregate 26005 remains ordinary CPU cross-rate data.
Safe output is reported only after actual terminal acknowledgement. The
8-millisecond safe timeout, native CUDA1/XDMA2 capacities, and frozen model are
unchanged. The simulator alone selects the finite five-second host watchdog.

The independent Python validator checks all active and inactive integer state,
channel metadata, generation/checksum binding, nested replay pairing, exact
backend operations, resource conservation and source hashes of all three kits.
Underflow clears at tick 12 and uses originating-owner replay. Other recoverable
faults resume from tick 5 in a compatible fresh owner; trusted replay itself
rejects foreign owners and variants before side effects.

The [CPU reference](docs__golden_system.md) and [CUDA reference](docs__golden_cuda.md) remain
separate unchanged kits. M26-05 delivers the broader showcase; the
[M26-06 audit](docs__golden_audit.md) records current capability dispositions and
remaining gates. This guide does not claim physical or release qualification.
