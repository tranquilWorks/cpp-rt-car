#!/usr/bin/env python3
"""Ordinary offline ownership, parser and CLI tests; no live device operation."""
import errno
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/host_profiles/profile.py"
spec = importlib.util.spec_from_file_location("host_profile", TOOL)
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)

GPU = "GPU-12345678-1234-1234-1234-123456789abc"
GPU_ROW = f"{GPU}, NVIDIA A100-SXM4-80GB, 00000000:3B:00.0, 570.124.06, 81920, 0, 0\n"
COMPILER = "Cuda compilation tools, release 12.8, V12.8.93\n"


class FakeNodes:
    def __init__(self):
        self.events = []
        self.failure = None
        self.index = 0

    def identity(self):
        return SimpleNamespace(st_mode=stat.S_IFCHR | 0o660,
            st_rdev=os.makedev(236, self.index), st_uid=1000, st_gid=1000)

    def lstat(self, path):
        self.index = profile.NODE_SUFFIXES.index(path.name.removeprefix("xdma0_"))
        result = self.identity()
        if self.failure == "regular": result.st_mode = stat.S_IFREG | 0o660
        return result

    def open(self, path):
        self.events.append(("open", path.name))
        if self.failure == "open": raise PermissionError(errno.EACCES, "permission denied")
        return 42

    def fstat(self, descriptor):
        self.events.append(("fstat", descriptor))
        if self.failure in ("fstat", "both"): raise OSError(errno.EIO, "fstat failed")
        result = self.identity()
        if self.failure == "replacement": result.st_rdev = os.makedev(236, 99)
        return result

    def close(self, descriptor):
        self.events.append(("close", descriptor))
        if self.failure in ("close", "both"): raise OSError(errno.EIO, "close failed")


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux sysfs/character-node observations")
class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sys = self.root / "sys"
        self.pci = self.sys / "bus/pci/devices/0000:05:00.0"
        self.pci.mkdir(parents=True)
        fields = {"vendor": "0x10ee", "device": "0x8038", "subsystem_vendor": "0x10ee",
            "subsystem_device": "0x0007", "current_link_speed": "8.0 GT/s PCIe",
            "current_link_width": "8", "max_link_speed": "8.0 GT/s PCIe",
            "max_link_width": "8", "numa_node": "0"}
        for key, value in fields.items(): (self.pci / key).write_text(value + "\n")
        driver = self.sys / "bus/pci/drivers/xdma"
        driver.mkdir(parents=True)
        (self.pci / "driver").symlink_to(driver)
        chars = self.sys / "dev/char"
        chars.mkdir(parents=True)
        for index, suffix in enumerate(profile.NODE_SUFFIXES):
            d = self.pci / "xdma" / f"xdma0_{suffix}"
            d.mkdir(parents=True)
            (d / "device").symlink_to(self.pci)
            (chars / f"236:{index}").symlink_to(d)
        self.nodes = FakeNodes()

    def discover(self, **kwargs):
        return profile.discover_xdma(sys_root=self.sys, dev_root=self.root / "dev",
                                     nodes=self.nodes, **kwargs)

    def test_metadata_does_not_open(self):
        result = self.discover()
        self.assertEqual(result["status"], "observed")
        self.assertEqual(self.nodes.events, [])
        self.assertEqual(len(result["nodes"]), 4)
        self.assertEqual(result["device_transfer"]["status"], "NOT RUN")
        self.assertFalse(any(result["claims"].values()))

    def test_open_fstat_close_exact_once(self):
        result = self.discover(probe_open=True)
        self.assertEqual(result["status"], "observed")
        self.assertEqual([e[0] for e in self.nodes.events], ["open", "fstat", "close"] * 4)
        self.assertTrue(all(n["open_close"] == "observed" for n in result["nodes"]))

    def test_wrong_node_binding_never_opened(self):
        node = self.pci / "xdma/xdma0_control/device"
        node.unlink(); node.symlink_to(self.root)
        result = self.discover(probe_open=True)
        self.assertEqual(result["status"], "failed")
        self.assertNotIn(("open", "xdma0_control"), self.nodes.events)
        self.assertEqual(result["nodes"][0]["open_close"], "NOT RUN")

    def test_wrong_node_name_never_opened(self):
        binding = self.sys / "dev/char/236:0"
        binding.unlink(); binding.symlink_to(self.pci / "xdma/xdma0_user")
        result = self.discover(probe_open=True)
        self.assertEqual(result["status"], "failed")
        self.assertNotIn(("open", "xdma0_control"), self.nodes.events)

    def test_missing_or_malformed_pci_stops_before_open(self):
        for key, value in (("vendor", "0x1234"), ("device", "x"),
                           ("current_link_width", "0"), ("numa_node", "bad")):
            with self.subTest(key=key):
                path = self.pci / key; previous = path.read_text(); path.write_text(value)
                result = self.discover(probe_open=True)
                path.write_text(previous)
                self.assertEqual(result["status"], "unavailable")
                self.assertEqual(self.nodes.events, [])
        (self.pci / "vendor").unlink()
        self.assertEqual(self.discover()["status"], "unavailable")

    def test_descriptor_error_closes_and_retains_failures(self):
        for failure in ("regular", "open", "fstat", "replacement", "close", "both"):
            with self.subTest(failure=failure):
                self.nodes = FakeNodes(); self.nodes.failure = failure
                result = self.discover(probe_open=True)
                self.assertEqual(result["status"], "failed")
                self.assertTrue(all(n["open_close"] == "NOT RUN" for n in result["nodes"]))
                closes = [x for x in self.nodes.events if x[0] == "close"]
                self.assertEqual(len(closes), 0 if failure in ("regular", "open") else 4)
                if failure == "both":
                    self.assertIn("descriptor_error", result["nodes"][0])
                    self.assertIn("close_error", result["nodes"][0])

    def test_native_open_flags(self):
        with patch.object(profile.os, "open", return_value=44) as call:
            self.assertEqual(profile.LocalNodes().open(Path("/dev/xdma0_control")), 44)
            call.assert_called_once_with(Path("/dev/xdma0_control"),
                os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)


class RemoteTests(unittest.TestCase):
    def runner(self, outputs, failure=None):
        self.calls = []
        def run(argv, **kwargs):
            self.calls.append((argv, kwargs))
            if failure: raise failure
            return SimpleNamespace(returncode=0, stdout=outputs[len(self.calls)-1], stderr="")
        return run

    def test_missing_route_never_connects(self):
        runner = self.runner([])
        result = profile.remote_inventory(runner=runner)
        self.assertEqual(result["status"], "NOT RUN")
        self.assertEqual(self.calls, [])

    def test_actual_identity_and_fixed_read_only_commands(self):
        result = profile.remote_inventory("owner@a100", runner=self.runner([GPU_ROW, "", COMPILER]))
        self.assertEqual(result["status"], "observed")
        self.assertTrue(result["a100_present"])
        self.assertEqual(result["cuda_workload"]["status"], "NOT RUN")
        self.assertFalse(any(result["claims"].values()))
        self.assertEqual([x[0][-1] for x in self.calls],
            [profile.GPU_COMMAND, profile.PROCESS_COMMAND, profile.COMPILER_COMMAND])
        for argv, kwargs in self.calls:
            self.assertIn("-oStrictHostKeyChecking=yes", argv)
            self.assertIn("-oBatchMode=yes", argv)
            self.assertIn("-oUpdateHostKeys=no", argv)
            self.assertEqual(kwargs["timeout"], 10)

    def test_occupied_gpu_retains_workload_blocker(self):
        for gpu, processes in ((GPU_ROW, f"{GPU}, 123, 256\n"),
                               (GPU_ROW.replace("81920, 0, 0", "81920, 1024, 50"), "")):
            result = profile.remote_inventory("a100", runner=self.runner([gpu, processes, COMPILER]))
            self.assertIn("preserve existing services", result["profiling_blocker"])
            self.assertEqual(result["cuda_workload"]["status"], "NOT RUN")

    def test_remote_error_and_timeout_are_not_passes(self):
        for failure in (FileNotFoundError("ssh missing"), subprocess.TimeoutExpired("ssh", 10)):
            result = profile.remote_inventory("a100", runner=self.runner([], failure))
            self.assertEqual(result["status"], "NOT RUN")
            self.assertEqual(len(self.calls), 1)
            self.assertIn("error", result["commands"][0])
        failed = lambda *a, **k: SimpleNamespace(returncode=255, stdout="", stderr="authentication failed")
        result = profile.remote_inventory("a100", runner=failed)
        self.assertEqual(result["commands"][0]["returncode"], 255)
        self.assertEqual(result["status"], "NOT RUN")

    def test_malformed_inventory_never_qualifies(self):
        for gpu in ("", GPU_ROW + GPU_ROW, GPU_ROW.replace("81920, 0, 0", "81920, 90000, 0"),
                    GPU_ROW.replace("81920, 0, 0", "81920, 0, 101"),
                    GPU_ROW.replace("81920, 0, 0", "N/A, 0, 0")):
            result = profile.remote_inventory("a100", runner=self.runner([gpu, "", COMPILER]))
            self.assertEqual(result["status"], "NOT RUN")
            self.assertFalse(any(result["claims"].values()))
        for processes, compiler in ((f"GPU-00000000, 3, 0\n", COMPILER), ("", "missing")):
            result = profile.remote_inventory("a100", runner=self.runner([GPU_ROW, processes, compiler]))
            self.assertEqual(result["status"], "NOT RUN")

    def test_invalid_configuration_rejected_before_connection(self):
        for destination in ("-oProxyCommand=bad", "owner@host;touch x", "bad host"):
            with self.assertRaises(ValueError):
                profile.remote_inventory(destination, runner=self.runner([]))
            self.assertEqual(self.calls, [])
        for timeout in (0, 31, float("nan"), float("inf")):
            with self.assertRaises(ValueError): profile.remote_inventory("a100", timeout=timeout)


class CopyAndCliTests(unittest.TestCase):
    def test_bounded_host_copy_integrity(self):
        result = profile.host_copy(size=4096, iterations=3)
        self.assertEqual(result["total_copied_bytes"], 12288)
        self.assertEqual(result["source_sha256"], result["destination_sha256"])
        self.assertEqual(len(result["copy_elapsed_ns"]), 3)
        for size, iterations in ((4095, 1), (1048577, 1), (4096, 0), (4096, 257)):
            with self.assertRaises(ValueError): profile.host_copy(size=size, iterations=iterations)

    def test_real_cli_missing_remote_report_and_no_clobber(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            args = [sys.executable, str(TOOL), "cuda", "--output", str(output)]
            result = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            report = json.loads(output.read_text())
            self.assertEqual(report["status"], "NOT RUN")
            before = output.read_bytes()
            result = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(output.read_bytes(), before)

    def test_cli_does_not_act_when_output_exists(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"; output.write_text("original")
            with patch.object(profile, "remote_inventory") as remote:
                self.assertEqual(profile.main(["cuda", "--ssh-destination", "a100", "--output", str(output)]), 2)
                remote.assert_not_called()

    def test_invalid_cli_configuration_creates_no_report_or_connection(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            with patch.object(profile, "remote_inventory") as remote:
                for destination in ("bad host", "-oProxyCommand=bad"):
                    self.assertEqual(profile.main(["cuda", "--ssh-destination=" + destination,
                        "--output", str(output)]), 2)
                    self.assertFalse(output.exists())
                remote.assert_not_called()

    @unittest.skipUnless(sys.platform.startswith("linux"), "Live-root Linux absent-device CLI")
    def test_absent_device_real_cli_no_endpoint_access(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "absent.json"
            result = subprocess.run([sys.executable, str(TOOL), "xdma", "--bdf", "ffff:ff:ff.7",
                "--output", str(output)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(json.loads(output.read_text())["status"], "unavailable")


if __name__ == "__main__": unittest.main(verbosity=2)
