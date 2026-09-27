#!/usr/bin/env python3
"""Build, install, archive-relocate and compile the actual shipped source kit."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(*command):
    print("+", *map(str, command), flush=True)
    subprocess.run(list(map(str, command)), check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-directory", required=True, type=Path)
    args = parser.parse_args()
    work = args.work_directory.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source_build = work / "sdk"
    original = work / "original-prefix"
    run("cmake", "-S", ROOT, "-B", source_build, "-DCMAKE_BUILD_TYPE=Release",
        "-DENABLE_TESTS=OFF", "-DRTFW_BUILD_EXAMPLES=OFF", "-DRTFW_BUILD_EXPERIMENTAL=OFF",
        "-DRTFW_BUILD_RUNTIME_DEMO=OFF", "-DRTFW_BUILD_BENCHMARKS=OFF", "-DSIM_SANITIZERS=", "-DSIM_WERROR=ON",
        "-DCMAKE_INSTALL_DATADIR=custom/data")
    run("cmake", "--build", source_build, "--config", "Release", "--parallel", "2")
    run("cmake", "--install", source_build, "--config", "Release", "--prefix", original)
    archive = shutil.make_archive(str(work / "physics-sdk"), "zip", original)
    shutil.rmtree(original)
    with tempfile.TemporaryDirectory(prefix="rtfw-physics-relocated-") as temporary:
        relocated = Path(temporary) / "sdk"
        shutil.unpack_archive(archive, relocated)
        kit = relocated / "custom/data/rtfw/examples/cuda_physics"
        consumer = Path(temporary) / "consumer"
        run("cmake", "-S", kit, "-B", consumer, "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_PREFIX_PATH={relocated}", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
        run("cmake", "--build", consumer, "--config", "Release", "--parallel", "2")
        run("ctest", "--test-dir", consumer, "-C", "Release", "--output-on-failure")
        commands = consumer / "compile_commands.json"
        if commands.exists():
            for entry in json.loads(commands.read_text()):
                assert str(ROOT) not in entry["command"], entry
                assert str(original) not in entry["command"], entry
        # Same shipped files, explicitly embedded through public targets.
        embedded = Path(temporary) / "embedded"
        run("cmake", "-S", kit, "-B", embedded, "-DCMAKE_BUILD_TYPE=Release",
            f"-DPHYSICS_RTFW_SOURCE={ROOT}", "-DSIM_SANITIZERS=", "-DSIM_WERROR=ON")
        run("cmake", "--build", embedded, "--config", "Release", "--parallel", "2")
        run("ctest", "--test-dir", embedded, "-C", "Release", "--output-on-failure")
    print("PASS: shipped-source relocated and embedded consumers")


if __name__ == "__main__":
    main()
