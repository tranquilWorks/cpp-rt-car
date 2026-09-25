#!/usr/bin/env python3
"""Append extension/analysis consumption after the unchanged device wrapper."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
for name in ("work-directory", "artifact-output", "cpu-artifact-output", "runtime-artifact-output", "device-artifact-output"):
    parser.add_argument("--" + name, type=Path, required=True)
args = parser.parse_args()
work = args.work_directory.resolve()


def run(*command):
    subprocess.run([str(x) for x in command], cwd=ROOT, check=True, timeout=1800)


predecessor = [sys.executable, ROOT / "tests/benchmark_fixtures/device_consumer/verify_package.py"]
for name, value in vars(args).items():
    predecessor += ["--" + name.replace("_", "-"), value.resolve()]
run(*predecessor)
prefix = work / "relocated/stage"
for mode in ("installed-extension", "embedded-extension"):
    option = f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}" if mode == "installed-extension" else f"-DRTFW_SOURCE_DIR={ROOT.as_posix()}"
    build = work / mode
    run("cmake", "-S", ROOT / "tests/benchmark_fixtures/extension_consumer", "-B", build,
        "-DCMAKE_BUILD_TYPE=Release", option)
    run("cmake", "--build", build, "--config", "Release", "--target", "extension_lifecycle", "--parallel", "2")
    run("ctest", "--test-dir", build, "-C", "Release", "--output-on-failure")

# Build the actual shipped template, not a checkout-relative stand-in.
build = work / "installed-template"
run("cmake", "-S", prefix / "share/rtfw/bench/examples/extension", "-B", build,
    "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}")
run("cmake", "--build", build, "--config", "Release", "--parallel", "2")
executable = build / ("rtfw-bench-template.exe" if sys.platform == "win32" else "rtfw-bench-template")
if not executable.is_file():
    executable = build / "Release" / executable.name
data = work / "analysis-evidence"
data.mkdir()
share = prefix / "share/rtfw"
policy = share / "bench/analysis/portable-policy.json"
tool = share / "tools/compare_benchmarks.py"
for role in ("baseline", "candidate"):
    run(executable, "--clock", "fake", "--output", data / role)
    run(sys.executable, share / "tools/check_benchmark_artifact.py", "--artifact-root", data / role)
    plan = json.loads((share / "bench/analysis/example-plan.json").read_bytes())
    plan["runs"][0]["bundle"] = role
    plan_path = data / f"{role}-plan.json"
    plan_path.write_text(json.dumps(plan), encoding="utf-8")
    run(sys.executable, tool, "capture", "--role", role, "--plan", plan_path,
        "--policy", policy, "--run-root", data, "--output", data / f"{role}.json")
run(sys.executable, tool, "compare", "--policy", policy, "--baseline", data / "baseline.json",
    "--candidate", data / "candidate.json", "--baseline-root", data, "--candidate-root", data,
    "--output", data / "report.json")
report = json.loads((data / "report.json").read_bytes())
assert report["status"] == "structural_match" and report["qualification"] == "none"
assert report["cases"][0]["performance"] is None
print("Extension and analysis installed/archive-relocated/embedded consumers passed")
