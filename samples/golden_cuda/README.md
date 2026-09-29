# Golden CUDA source kit

Run the fixed M26 scenario using CPU fallback, injected-driver CUDA kernel or
CUDA Graph physics. The other seven stages, controls, channels and independent
state oracles are shared with the sibling `golden_system` kit.

```sh
python3 run.py --prefix /path/to/sdk --dispatch graph --mode host
python3 run.py --prefix /path/to/sdk --dispatch kernel --mode external
python3 run.py --prefix /path/to/sdk --fault reset_failure
```

Keep both sibling source directories together. `run.py` builds public SDK
consumers, runs the scenario, checks every state field with the scalar Python
oracle, verifies actual device counts and publishes source-bound run/replay
artifacts. Modes `native` and `host` select CPU execution ownership; both use the
injected Driver API unless the separate real host is selected.

For optional physical execution, configure `-DGOLDEN_CUDA_REAL=ON` against a CUDA
Driver-enabled SDK and toolkit, then run `golden_cuda_real --help`. Missing
hardware exits 3 with NOT_RUN. Native replay is unsupported. Portable protocol
success and a compiled real host do not qualify physical CUDA, RT or performance.

See the installed `golden_cuda.md` for ownership, timing and fault semantics.
