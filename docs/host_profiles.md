# Nonpersistent host observations

`tools/host_profiles/profile.py` is an optional non-RT host tool and Python API.
It adds no default SDK target/header, Runtime operation, native backend change,
service, workflow, support tuple, qualification record or automatic hardware run.
Existing authority for `M18-XDMA-PROFILE-01` permits metadata, open/close and
host staging. The A100 profile authority permits configured nonpersistent probes;
this tool supplies inventory only. Normal tests use temporary records and injected
operations, never live endpoints or remote hosts.

## XDMA discovery and open/close

```sh
python3 tools/host_profiles/profile.py xdma --output discovery.json
python3 tools/host_profiles/profile.py xdma --open-close --host-copy \
  --bdf 0000:05:00.0 --prefix xdma0 --output access-and-host-copy.json
```

Outputs must be new files. Argument validation and exclusive output creation
precede any native or remote operation. Parent directories must already exist.
Exit 0 means the requested inventory/open-close observations succeeded. Exit 2
means failed/unavailable/NOT RUN or invalid configuration; inspect the report and
stderr. No output is overwritten and an existing report prevents probing.

The tool selects one exact Xilinx `10ee:8038` PCI function bound to `xdma`, retains
subsystem/link/NUMA metadata and examines four explicit nodes: control, user,
H2C0 and C2H0. It checks each direct character-node major/minor against its sysfs
name and PCI binding before optional `O_RDONLY|O_NONBLOCK|O_CLOEXEC` open. It
checks descriptor type/identity and binding again, then closes once. Fstat and
close failures are retained separately if both occur. A failed Linux close is
not blindly retried because its descriptor may already have been released.

This is a snapshot, without a hotplug lock or exclusive hardware lease. Open may
invoke kernel-driver behavior despite nonblocking flags. The tool never calls
endpoint read, write, mmap or ioctl, submits DMA, reads/writes a BAR, resets or
rebinds a driver, loads a design or touches output controls. It does not call the
production XDMA backend, whose initialization/worker/transfer contract is broader.
No uploaded manifest or command-line flag can enable a transfer. The loaded FPGA
design and a self-identifying non-output loopback contract remain unknown; device
transfer is explicitly NOT RUN.

`--host-copy` performs ordinary host copies with SHA-256 integrity checks. The
default is 64 copies of 65536 bytes, bounded to 1–256 copies of 4096–1048576 bytes.
The report retains each copy wall observation, overall wall duration and input
digest. These Python memory observations establish neither pinned/DMA storage nor
device bandwidth, controlled performance, RT deadlines or physical endurance.

Python callers may import `discover_xdma` and `host_copy`. Alternate sysfs/device
roots and injected node operations are offline test seams. Fixture results must
never be recorded as actual hardware evidence.

## Configured A100 inventory

```sh
python3 tools/host_profiles/profile.py cuda --output unavailable.json
python3 tools/host_profiles/profile.py cuda --ssh-destination owner@configured-a100 \
  --timeout 10 --output a100-inventory.json
```

Without a configured destination, the first command writes NOT RUN and exits 2
without connecting. The tool never guesses a username/wake route or changes SSH
known hosts. A supplied destination uses batch authentication, strict existing
host-key checks, disabled persistent control sockets and three fixed read-only
commands: NVIDIA device inventory, active compute-process inventory and compiler
version. Each command has a 1–30-second timeout, with command, exit/error and wall
observations retained. No CUDA workload, installation, service change or remote
temporary file is created.

The parser binds unique device UUIDs, PCI buses, driver and compiler versions,
capacities/utilization and process ownership. Missing/invalid responses stay NOT
RUN. Occupied devices retain an explicit profiling blocker; the tool never evicts
a workload. Even an idle A100 snapshot supplies no lease or correctness/transfer/
launch characterization. Those actual disposable workload steps remain open under
the original profile card and require its configured access and non-disruption
conditions. Remote A100 and local XDMA are not a co-resident combined tuple.

Every report records tool SHA-256, host/kernel, UTC time and false qualification
claims. This is observation evidence, not authenticated provenance, hardware/RT
promotion or production release authority. See [remaining batches](remaining_batches.md).
