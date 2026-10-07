#!/usr/bin/env python3
"""Install, relocate and build the standalone lifecycle kit with installed SDK only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--c-compiler", required=True)
    parser.add_argument("--cxx-compiler", required=True)
    parser.add_argument("--sanitizers", default="")
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    source = Path(__file__).resolve().parents[2]
    work = Path(tempfile.mkdtemp(prefix="host-lifecycle-package-", dir=build))
    records = []

    def run(argv):
        log = work / f"command-{len(records)}.log"
        with log.open("w", encoding="utf-8") as output:
            result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT, check=False)
        records.append({"argv": argv, "exit_code": result.returncode,
                        "log": str(log), "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (work / "commands.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        if result.returncode:
            print(log.read_text(encoding="utf-8", errors="replace"))
            raise RuntimeError(f"command failed: {argv}; retained {work}")

    original = work / "original-prefix"
    relocated = work / "relocated-prefix"
    config = args.config or "Release"
    run(["cmake", "--install", str(build), "--prefix", str(original), "--config", config])
    shutil.move(str(original), str(relocated))
    assert not original.exists()
    installed = relocated / "share/rtfw/integrations/host_lifecycle"
    expected = {"CMakeLists.txt", "README.md", "lifecycle.hpp", "time.hpp", "library.hpp", "main.cpp", "plugin.c"}
    assert {p.name for p in installed.iterdir() if p.is_file()} == expected
    hashes = {}
    for name in sorted(expected):
        shipped = (installed / name).read_bytes()
        assert shipped == (source / "integrations/host_lifecycle" / name).read_bytes(), name
        hashes[name] = hashlib.sha256(shipped).hexdigest()
    consumer = work / "consumer-source"
    shutil.copytree(installed, consumer)
    consumer_build = work / "consumer-build"
    # An instrumented installed static Runtime needs the same compiler/runtime
    # at its consumer link. Keep both the copied kit and module instrumented.
    toolchain = ["-DCMAKE_C_COMPILER=" + args.c_compiler,
                 "-DCMAKE_CXX_COMPILER=" + args.cxx_compiler]
    if args.sanitizers:
        flags = "-fsanitize=" + args.sanitizers + " -fno-omit-frame-pointer"
        if "cfi" in args.sanitizers.split(","):
            flags += " -flto"
        toolchain.extend("-D" + name + "=" + flags for name in
                         ["CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS",
                          "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS"])
    run(["cmake", "-S", str(consumer), "-B", str(consumer_build),
         "-DCMAKE_BUILD_TYPE=" + config, "-DCMAKE_PREFIX_PATH=" + str(relocated)] + toolchain)
    run(["cmake", "--build", str(consumer_build), "--config", config, "--parallel", "2"])
    run(["ctest", "--test-dir", str(consumer_build), "--build-config", config, "--output-on-failure"])
    (work / "result.json").write_text(json.dumps({"status": "pass", "kit_sha256": hashes,
        "commands": records, "original_prefix_removed": True,
        "actual_unreal_qualified": False, "hardware_qualified": False}, indent=2) + "\n", encoding="utf-8")
    print("host_lifecycle relocated package PASS; retained", work)


if __name__ == "__main__":
    main()
