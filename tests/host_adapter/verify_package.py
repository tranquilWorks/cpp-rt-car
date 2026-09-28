#!/usr/bin/env python3
"""Exercise shipped onboarding from a relocated SDK and a source checkout."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = "host_adapter: mode={mode} frames=16 instances=2 checksums=2176,6528 telemetry=32/32 lost=0 jobs=balanced memory=6/6 stopped=ok\n"


def run(*command, failure=None):
    command = list(map(str, command))
    print("+", *command, flush=True)
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=600)
    print(result.stdout, end="", flush=True)
    if failure is None:
        if result.returncode:
            raise RuntimeError(f"command failed ({result.returncode}): {command}")
    elif result.returncode == 0 or failure not in result.stdout:
        raise RuntimeError(f"expected failure containing {failure!r}: {command}")
    return result.stdout


def build_test(source, build, *options):
    run("cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *options)
    run("cmake", "--build", build, "--config", "Release", "--parallel", "2")
    run("ctest", "--test-dir", build, "-C", "Release", "--output-on-failure")


def executable(build):
    candidates = [build / "host_adapter", build / "Release/host_adapter.exe",
                  build / "host_adapter.exe"]
    return next(path for path in candidates if path.is_file())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-directory", required=True, type=Path)
    args = parser.parse_args()
    work = args.work_directory.resolve()
    work.mkdir(parents=True, exist_ok=True)
    # A fresh prefix avoids stale installed files masking an omitted install rule.
    with tempfile.TemporaryDirectory(prefix="host original ", dir=work) as original_dir:
        original = Path(original_dir) / "sdk"
        sdk = work / "sdk build"
        run("cmake", "-S", ROOT, "-B", sdk, "-DCMAKE_BUILD_TYPE=Release",
            "-DENABLE_TESTS=OFF", "-DRTFW_BUILD_EXAMPLES=OFF",
            "-DRTFW_BUILD_RUNTIME_DEMO=OFF", "-DRTFW_BUILD_EXPERIMENTAL=OFF",
            "-DRTFW_BUILD_BENCHMARKS=OFF", "-DSIM_SANITIZERS=", "-DSIM_WERROR=ON",
            "-DCMAKE_INSTALL_INCLUDEDIR=sdk/include", "-DCMAKE_INSTALL_DATADIR=custom/data")
        run("cmake", "--build", sdk, "--config", "Release", "--parallel", "2")
        run("cmake", "--install", sdk, "--config", "Release", "--prefix", original)
        archive = shutil.make_archive(str(work / "host-sdk"), "zip", original)
        shutil.rmtree(original)
        with tempfile.TemporaryDirectory(prefix="host relocated ") as temporary:
            isolated = Path(temporary)
            prefix = isolated / "sdk"
            shutil.unpack_archive(archive, prefix)
            data = prefix / "custom/data/rtfw"
            kit = data / "examples/host_adapter"
            expected_files = {"CMakeLists.txt", "check_output.cmake", "main.cpp",
                              "host.hpp", "jobs.hpp", "memory.hpp"}
            if {p.name for p in kit.iterdir()} != expected_files:
                raise RuntimeError("shipped host source inventory mismatch")
            for name in expected_files:
                if (kit / name).read_bytes() != (ROOT / "samples/host_adapter" / name).read_bytes():
                    raise RuntimeError(f"shipped source differs: {name}")
            if (data / "host_adapter.md").read_bytes() != (ROOT / "docs/host_adapter.md").read_bytes():
                raise RuntimeError("shipped first-use guide differs")
            if (prefix / "sdk/include/rt/sdk.hpp").read_bytes() != (ROOT / "rt/include/rt/sdk.hpp").read_bytes():
                raise RuntimeError("shipped SDK header differs")
            prefix_option = f"-DCMAKE_PREFIX_PATH={prefix}"
            consumer = isolated / "consumer"
            build_test(kit, consumer, prefix_option)
            for mode in ("native", "host"):
                if run(executable(consumer), mode) != EXPECTED.format(mode=mode):
                    raise RuntimeError("independent host output oracle failed")
            commands = consumer / "compile_commands.json"
            if commands.exists():
                for entry in json.loads(commands.read_text()):
                    command = entry.get("command", " ".join(entry.get("arguments", [])))
                    normalized = command.replace("\\", "/")
                    for forbidden in (ROOT, sdk, original):
                        if forbidden.as_posix() in normalized:
                            raise RuntimeError(f"private/original path leaked: {forbidden}")
            # Run the complete existing C/C++ package suite, including the new kit.
            build_test(ROOT / "tests/package_consumer", isolated / "all consumers", prefix_option)
            build_test(kit, isolated / "embedded", f"-DHOST_RTFW_SOURCE={ROOT}",
                       "-DSIM_SANITIZERS=", "-DSIM_WERROR=ON")
            # Explicitly opting into repository examples must not collide with the host target.
            run("cmake", "-S", kit, "-B", isolated / "embedded",
                f"-DHOST_RTFW_SOURCE={ROOT}", "-DRTFW_BUILD_EXAMPLES=ON")
            run("cmake", "--build", isolated / "embedded", "--config", "Release",
                "--target", "host_adapter", "sample_host_adapter", "--parallel", "2")
            run("cmake", "-S", kit, "-B", isolated / "missing package",
                "-DCMAKE_DISABLE_FIND_PACKAGE_rtfw=ON", failure="REQUIRED")
            run("cmake", "-S", kit, "-B", isolated / "missing source",
                f"-DHOST_RTFW_SOURCE={isolated / 'absent'}",
                failure="HOST_RTFW_SOURCE must name an RTFW source checkout")
            # Independent output oracle must reject a false success transcript.
            variant = isolated / "oracle source"
            shutil.copytree(kit, variant)
            source = (variant / "main.cpp").read_text()
            before = "<< first.world.checksum"
            if source.count(before) != 1:
                raise RuntimeError("ambiguous oracle mutation")
            (variant / "main.cpp").write_text(source.replace(before, "<< (first.world.checksum + 1)"))
            build = isolated / "oracle build"
            run("cmake", "-S", variant, "-B", build, "-DCMAKE_BUILD_TYPE=Release", prefix_option)
            run("cmake", "--build", build, "--config", "Release", "--parallel", "2")
            run("ctest", "--test-dir", build, "-C", "Release", "--output-on-failure",
                failure="host adapter oracle mismatch")
            # Missing a shipped header must fail an actual build.
            missing = isolated / "missing header source"
            shutil.copytree(kit, missing)
            (missing / "jobs.hpp").unlink()
            build = isolated / "missing header build"
            run("cmake", "-S", missing, "-B", build, "-DCMAKE_BUILD_TYPE=Release", prefix_option)
            run("cmake", "--build", build, "--config", "Release", "--parallel", "2", failure="jobs.hpp")
            print("PASS: host relocated/embedded/full SDK, isolation and four negative controls", flush=True)


if __name__ == "__main__":
    main()
