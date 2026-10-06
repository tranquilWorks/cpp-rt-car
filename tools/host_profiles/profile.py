#!/usr/bin/env python3
"""Optional host-side observations. Never performs endpoint traffic or qualification."""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import stat
import subprocess
import sys
import time
from typing import Any

SCHEMA = "rtfw.host-profile.v1"
CLAIMS = {"hardware_qualified": False, "RT1_qualified": False,
          "RT2_qualified": False, "release_qualified": False}
BDF = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]\Z")
NODE_SUFFIXES = ("control", "user", "h2c_0", "c2h_0")
GPU_COMMAND = ("LC_ALL=C nvidia-smi --query-gpu=uuid,name,pci.bus_id,driver_version,"
               "memory.total,memory.used,utilization.gpu --format=csv,noheader,nounits")
PROCESS_COMMAND = ("LC_ALL=C nvidia-smi --query-compute-apps=gpu_uuid,pid,used_memory "
                   "--format=csv,noheader,nounits")
COMPILER_COMMAND = "LC_ALL=C nvcc --version"


class LocalNodes:
    """Only node metadata, nonblocking open, descriptor metadata and close."""
    lstat = staticmethod(os.lstat)
    fstat = staticmethod(os.fstat)
    close = staticmethod(os.close)

    @staticmethod
    def open(path: Path) -> int:
        if platform.system() != "Linux":
            raise ValueError("native XDMA open requires Linux")
        return os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)


def error_record(exc: Exception) -> dict[str, Any]:
    return {"type": type(exc).__name__, "errno": getattr(exc, "errno", None),
            "message": str(exc)}


def read_field(path: Path) -> str:
    # Kernel-generated attributes are short. Reject incomplete/oversized input.
    with path.open("r", encoding="ascii") as stream:
        value = stream.read(4097)
    if not value.strip() or len(value) > 4096:
        raise ValueError(f"empty or oversized attribute: {path.name}")
    return value.strip()


def discover_xdma(*, bdf: str = "0000:05:00.0", prefix: str = "xdma0",
                  sys_root: Path = Path("/sys"), dev_root: Path = Path("/dev"),
                  probe_open: bool = False, nodes: Any = None) -> dict[str, Any]:
    """Bind four expected nodes to one PCI function before optional open/close.

    Alternate roots and node operations are dependency injection for offline tests.
    The CLI uses real roots. No fabricated loaded-design record enables DMA.
    """
    if not BDF.fullmatch(bdf) or not re.fullmatch(r"xdma[0-9]{1,3}", prefix):
        raise ValueError("invalid explicit PCI BDF or XDMA prefix")
    nodes = nodes or LocalNodes()
    report: dict[str, Any] = {"schema": SCHEMA, "kind": "xdma", "claims": dict(CLAIMS),
        "status": "unavailable", "bdf": bdf, "prefix": prefix, "nodes": [],
        "device_transfer": {"status": "NOT RUN", "reason": "No independently verified loaded-design/non-output transfer contract; tool has no transfer operation."},
        "limitations": ["Snapshot only; no exclusive hardware ownership or hotplug lock.",
                        "Open/close does not validate FPGA design, protocol, DMA, timing or outputs."]}
    pci = sys_root / "bus/pci/devices" / bdf
    try:
        resolved_pci = pci.resolve(strict=True)
        fields = {key: read_field(pci / key) for key in
                  ("vendor", "device", "subsystem_vendor", "subsystem_device",
                   "current_link_speed", "current_link_width", "max_link_speed",
                   "max_link_width", "numa_node")}
        for key in ("vendor", "device", "subsystem_vendor", "subsystem_device"):
            if not re.fullmatch(r"0x[0-9a-fA-F]{4}", fields[key]):
                raise ValueError(f"malformed PCI identity: {key}")
        for key in ("current_link_width", "max_link_width"):
            if not fields[key].isdigit() or not 1 <= int(fields[key]) <= 32:
                raise ValueError(f"invalid link width: {key}")
        if not re.fullmatch(r"-?[0-9]+", fields["numa_node"]):
            raise ValueError("invalid NUMA node")
        driver = (pci / "driver").resolve(strict=True)
        report["pci"] = fields | {"driver": driver.name, "sysfs_path": str(resolved_pci)}
        if fields["vendor"].lower() != "0x10ee" or fields["device"].lower() != "0x8038" or driver.name != "xdma":
            raise ValueError("selected tuple is not Xilinx 10ee:8038 bound to xdma")
    except (OSError, ValueError) as exc:
        report["error"] = error_record(exc)
        return report
    failures = False
    for suffix in NODE_SUFFIXES:
        node = dev_root / f"{prefix}_{suffix}"
        entry: dict[str, Any] = {"path": str(node), "status": "failed",
                                 "open_close": "NOT RUN"}
        try:
            identity = nodes.lstat(node)
            if not stat.S_ISCHR(identity.st_mode):
                raise ValueError("endpoint is not a direct character device")
            major, minor = os.major(identity.st_rdev), os.minor(identity.st_rdev)
            device = sys_root / "dev/char" / f"{major}:{minor}"
            if (device / "device").resolve(strict=True) != resolved_pci:
                raise ValueError("character endpoint belongs to another PCI function")
            if device.resolve(strict=True).name != node.name:
                raise ValueError("character endpoint name disagrees with sysfs")
            entry.update(major=major, minor=minor, mode=oct(stat.S_IMODE(identity.st_mode)),
                         uid=identity.st_uid, gid=identity.st_gid, pci_binding=bdf,
                         status="observed")
            if probe_open:
                started = time.perf_counter_ns()
                descriptor = nodes.open(node)
                try:
                    opened = nodes.fstat(descriptor)
                    if not stat.S_ISCHR(opened.st_mode) or opened.st_rdev != identity.st_rdev:
                        raise ValueError("opened descriptor identity changed")
                    if (device / "device").resolve(strict=True) != resolved_pci:
                        raise ValueError("PCI binding changed during open")
                except (OSError, ValueError) as exc:
                    entry["descriptor_error"] = error_record(exc)
                    raise
                finally:
                    # A failed close is retained; retry may affect a reused Linux fd.
                    try:
                        nodes.close(descriptor)
                    except OSError as exc:
                        entry["close_error"] = error_record(exc)
                        raise
                    finally:
                        entry["open_close_elapsed_ns"] = time.perf_counter_ns() - started
                entry["open_close"] = "observed"
        except (OSError, ValueError) as exc:
            entry["status"] = "failed"
            entry["error"] = error_record(exc)
            failures = True
        report["nodes"].append(entry)
    report["status"] = "failed" if failures else "observed"
    return report


def host_copy(*, size: int = 65536, iterations: int = 64) -> dict[str, Any]:
    """Ordinary host memory copies with integrity checks, not DMA or RT work."""
    if not 4096 <= size <= 1048576 or not 1 <= iterations <= 256:
        raise ValueError("host copy requires 4096..1048576 bytes and 1..256 iterations")
    source = bytes((i % 251 for i in range(size)))
    destination = bytearray(size)
    expected = hashlib.sha256(source).hexdigest()
    started = time.perf_counter_ns()
    copy_durations = []
    for _ in range(iterations):
        before = time.perf_counter_ns()
        destination[:] = source
        copy_durations.append(time.perf_counter_ns() - before)
        if hashlib.sha256(destination).hexdigest() != expected:
            raise RuntimeError("host-copy integrity failed")
    return {"status": "observed", "kind": "host_copy_only", "bytes_per_copy": size,
            "iterations": iterations, "total_copied_bytes": size * iterations,
            "source_sha256": expected, "destination_sha256": expected,
            "copy_elapsed_ns": copy_durations,
            "wall_elapsed_ns": time.perf_counter_ns() - started,
            "limitations": "Ordinary Python host memory and wall observations; no pinning, device DMA, controlled performance or RT/endurance qualification."}


def parse_gpu_inventory(text: str) -> list[dict[str, Any]]:
    rows = []
    uuids = set()
    for raw in csv.reader(io.StringIO(text)):
        if len(rows) >= 16 or len(raw) != 7:
            raise ValueError("invalid GPU row count or fields")
        uuid, name, bus, driver, total, used, utilization = [s.strip() for s in raw]
        if not re.fullmatch(r"GPU-[0-9a-fA-F-]{8,64}", uuid) or uuid in uuids:
            raise ValueError("invalid or duplicate GPU identity")
        if not name or len(name) > 160 or not re.fullmatch(r"[0-9A-Fa-f]{8}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-7]", bus):
            raise ValueError("invalid GPU name or bus identity")
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", driver):
            raise ValueError("invalid NVIDIA driver version")
        if not all(x.isdigit() for x in (total, used, utilization)):
            raise ValueError("GPU capacity/utilization unavailable")
        total_i, used_i, utilization_i = map(int, (total, used, utilization))
        if total_i <= 0 or used_i > total_i or utilization_i > 100:
            raise ValueError("invalid GPU capacity/utilization")
        uuids.add(uuid)
        rows.append({"uuid": uuid, "name": name, "pci_bus_id": bus, "driver_version": driver,
                     "memory_total_mib": total_i, "memory_used_mib": used_i,
                     "utilization_percent": utilization_i})
    if not rows:
        raise ValueError("no NVIDIA device records")
    return rows


def parse_process_inventory(text: str, uuids: set[str]) -> list[dict[str, Any]]:
    rows = []
    for raw in csv.reader(io.StringIO(text)):
        if len(rows) >= 128 or len(raw) != 3:
            raise ValueError("invalid GPU process row count or fields")
        uuid, pid, memory = [s.strip() for s in raw]
        if uuid not in uuids or not pid.isdigit() or int(pid) <= 0 or not memory.isdigit():
            raise ValueError("invalid GPU process record")
        rows.append({"gpu_uuid": uuid, "pid": int(pid), "memory_mib": int(memory)})
    return rows


def validate_remote_parameters(destination: str | None, timeout: float) -> None:
    if not math.isfinite(timeout) or not 1 <= timeout <= 30:
        raise ValueError("remote timeout must be finite and within 1..30 seconds")
    if destination is not None and not re.fullmatch(
            r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}(?:@[A-Za-z0-9][A-Za-z0-9.-]{0,252})?", destination):
        raise ValueError("invalid explicit SSH destination")


def remote_inventory(destination: str | None = None, *, timeout: float = 10,
                     runner: Any = subprocess.run) -> dict[str, Any]:
    """Inventory only; never starts CUDA work or guesses a wake/SSH route."""
    validate_remote_parameters(destination, timeout)
    report: dict[str, Any] = {"schema": SCHEMA, "kind": "remote_cuda", "claims": dict(CLAIMS),
        "status": "NOT RUN", "commands": [], "destination": destination,
        "cuda_workload": {"status": "NOT RUN", "reason": "Inventory tool performs no CUDA workload; idle snapshots confer no workload ownership."}}
    if destination is None:
        report["reason"] = "No explicitly configured SSH destination or wake route supplied."
        return report
    outputs = []
    for operation, command in (("gpu", GPU_COMMAND), ("processes", PROCESS_COMMAND), ("compiler", COMPILER_COMMAND)):
        argv = ["ssh", "-T", "-oBatchMode=yes", "-oConnectTimeout=5",
                "-oStrictHostKeyChecking=yes", "-oUpdateHostKeys=no",
                "-oControlMaster=no", "-oControlPath=none", "--", destination, command]
        entry: dict[str, Any] = {"operation": operation, "argv": argv, "timeout_seconds": timeout}
        report["commands"].append(entry)
        started = time.perf_counter_ns()
        try:
            result = runner(argv, capture_output=True, text=True, timeout=timeout, check=False)
            entry.update(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
            if len(result.stdout) > 65536 or len(result.stderr) > 65536:
                entry["stdout"] = result.stdout[:65536]
                entry["stderr"] = result.stderr[:65536]
                raise ValueError("remote inventory output oversized")
            if result.returncode:
                report["reason"] = f"{operation} inventory command failed"
                return report
            outputs.append(result.stdout)
        except (OSError, subprocess.TimeoutExpired, ValueError) as exc:
            entry["error"] = error_record(exc)
            report["reason"] = f"{operation} inventory unavailable"
            return report
        finally:
            entry["wall_elapsed_ns"] = time.perf_counter_ns() - started
    try:
        gpus = parse_gpu_inventory(outputs[0])
        processes = parse_process_inventory(outputs[1], {g['uuid'] for g in gpus})
        version = re.search(r"release ([0-9]+\.[0-9]+), V([0-9]+\.[0-9]+\.[0-9]+)", outputs[2])
        if not version:
            raise ValueError("CUDA compiler version unavailable")
        report.update(status="observed", gpus=gpus, processes=processes,
                      cuda_release=version.group(1), nvcc_version=version.group(2))
        a100 = [g for g in gpus if re.search(r"\bA100\b", g["name"])]
        report["a100_present"] = bool(a100)
        report["profiling_blocker"] = ("No named A100 observed" if not a100 else
            "GPU active or memory in use; preserve existing services" if processes or any(
                g["memory_used_mib"] or g["utilization_percent"] for g in a100) else
            "Idle snapshot only; controlled disposable workload remains separate")
    except ValueError as exc:
        report["error"] = error_record(exc)
        report["reason"] = "inventory response validation failed"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("xdma", "cuda"))
    parser.add_argument("--output", type=Path, required=True, help="New report file; existing output is refused")
    parser.add_argument("--bdf", default="0000:05:00.0")
    parser.add_argument("--prefix", default="xdma0")
    parser.add_argument("--open-close", action="store_true", help="Explicitly open/fstat/close selected XDMA nodes; no endpoint traffic")
    parser.add_argument("--host-copy", action="store_true")
    parser.add_argument("--copy-bytes", type=int, default=65536)
    parser.add_argument("--copy-iterations", type=int, default=64)
    parser.add_argument("--ssh-destination", help="Explicit configured authenticated destination; no automatic wake")
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args(argv)
    try:
        if args.kind == "cuda" and (args.open_close or args.host_copy):
            raise ValueError("XDMA/copy actions cannot be selected for CUDA inventory")
        if args.kind == "xdma" and args.ssh_destination:
            raise ValueError("remote destination cannot be selected for XDMA")
        if args.kind == "xdma" and platform.system() != "Linux":
            raise ValueError("live XDMA observation requires Linux")
        # Validate arguments and reserve output before any native/remote operation.
        if not BDF.fullmatch(args.bdf) or not re.fullmatch(r"xdma[0-9]{1,3}", args.prefix):
            raise ValueError("invalid explicit PCI BDF or XDMA prefix")
        if args.host_copy and not (4096 <= args.copy_bytes <= 1048576 and 1 <= args.copy_iterations <= 256):
            raise ValueError("host-copy bounds exceeded")
        validate_remote_parameters(args.ssh_destination, args.timeout)
        with args.output.open("x", encoding="utf-8") as stream:
            report = (discover_xdma(bdf=args.bdf, prefix=args.prefix, probe_open=args.open_close)
                      if args.kind == "xdma" else remote_inventory(args.ssh_destination, timeout=args.timeout))
            if args.host_copy:
                report["host_copy"] = host_copy(size=args.copy_bytes, iterations=args.copy_iterations)
            report["host"] = {"system": platform.system(), "kernel": platform.release(), "machine": platform.machine()}
            report["tool_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
            report["recorded_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            json.dump(report, stream, indent=2)
            stream.write("\n")
        print(f"{args.kind}: {report['status']}; report={args.output}")
        return 0 if report["status"] == "observed" else 2
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"host profile failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
