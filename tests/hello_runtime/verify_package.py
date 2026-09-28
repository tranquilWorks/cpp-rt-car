#!/usr/bin/env python3
"""Exercise shipped onboarding from a relocated SDK and a source checkout."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = "hello_runtime: frames=3 produced=3 consumed=3 stopped=ok\n"


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
    candidates = [build / "hello_runtime", build / "Release/hello_runtime.exe",
                  build / "hello_runtime.exe"]
    return next(path for path in candidates if path.is_file())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-directory", required=True, type=Path)
    args = parser.parse_args()
    work = args.work_directory.resolve()
    work.mkdir(parents=True, exist_ok=True)
    # A fresh prefix avoids stale installed files masking an omitted install rule.
    with tempfile.TemporaryDirectory(prefix="hello original ", dir=work) as original_dir:
        original = Path(original_dir) / "sdk"
        sdk = work / "sdk build"
        run("cmake", "-S", ROOT, "-B", sdk, "-DCMAKE_BUILD_TYPE=Release",
            "-DENABLE_TESTS=OFF", "-DRTFW_BUILD_EXAMPLES=OFF",
            "-DRTFW_BUILD_RUNTIME_DEMO=OFF", "-DRTFW_BUILD_EXPERIMENTAL=OFF",
            "-DRTFW_BUILD_BENCHMARKS=OFF", "-DSIM_SANITIZERS=", "-DSIM_WERROR=ON",
            "-DCMAKE_INSTALL_INCLUDEDIR=sdk/include", "-DCMAKE_INSTALL_DATADIR=custom/data")
        run("cmake", "--build", sdk, "--config", "Release", "--parallel", "2")
        run("cmake", "--install", sdk, "--config", "Release", "--prefix", original)
        archive = shutil.make_archive(str(work / "hello-sdk"), "zip", original)
        shutil.rmtree(original)
        with tempfile.TemporaryDirectory(prefix="hello relocated ") as temporary:
            isolated = Path(temporary)
            prefix = isolated / "sdk"
            shutil.unpack_archive(archive, prefix)
            data = prefix / "custom/data/rtfw"
            kit = data / "examples/hello_runtime"
            expected_files = {"CMakeLists.txt", "check_output.cmake", "main.cpp"}
            if {p.name for p in kit.iterdir()} != expected_files:
                raise RuntimeError("shipped hello source inventory mismatch")
            for name in expected_files:
                if (kit / name).read_bytes() != (ROOT / "samples/hello_runtime" / name).read_bytes():
                    raise RuntimeError(f"shipped source differs: {name}")
            if (data / "getting_started.md").read_bytes() != (ROOT / "docs/getting_started.md").read_bytes():
                raise RuntimeError("shipped first-use guide differs")
            prefix_option = f"-DCMAKE_PREFIX_PATH={prefix}"
            consumer = isolated / "consumer"
            build_test(kit, consumer, prefix_option)
            if run(executable(consumer)) != EXPECTED:
                raise RuntimeError("independent hello output oracle failed")
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
            build_test(kit, isolated / "embedded", f"-DHELLO_RTFW_SOURCE={ROOT}",
                       "-DSIM_SANITIZERS=", "-DSIM_WERROR=ON")
            # Explicitly opting into repository examples must not collide with the host target.
            run("cmake", "-S", kit, "-B", isolated / "embedded",
                f"-DHELLO_RTFW_SOURCE={ROOT}", "-DRTFW_BUILD_EXAMPLES=ON")
            run("cmake", "--build", isolated / "embedded", "--config", "Release",
                "--target", "hello_runtime", "sample_hello_runtime", "--parallel", "2")
            run("cmake", "-S", kit, "-B", isolated / "missing package",
                "-DCMAKE_DISABLE_FIND_PACKAGE_rtfw=ON", failure="REQUIRED")
            run("cmake", "-S", kit, "-B", isolated / "missing source",
                f"-DHELLO_RTFW_SOURCE={isolated / 'absent'}",
                failure="HELLO_RTFW_SOURCE must name an RTFW source checkout")
            # Mutate private copies, never the shipped source or production runtime.
            source = (kit / "main.cpp").read_text()
            variants = {
                "capacity": ("config.callback_capacity = 2;", "config.callback_capacity = 1;",
                             "register consume failed (-4): configured capacity exceeded"),
                "step": ("++static_cast<State*>(user)->produced;\n    return rt::CallbackResult::ok;",
                         "(void)user;\n    return rt::CallbackResult::error;",
                         "step failed"),
                "oracle": ("frames=3 produced=3 consumed=3", "frames=3 produced=3 consumed=999", None),
            }
            for name, (before, after, diagnostic) in variants.items():
                if source.count(before) != 1:
                    raise RuntimeError(f"negative-control edit is ambiguous: {name}")
                variant = isolated / f"{name} source"
                shutil.copytree(kit, variant)
                changed = source.replace(before, after)
                if name == "step":
                    changed = changed.replace("if (!ok || !stopped) return 1;",
                        'if (stopped) std::cerr << "checked stop reached\\n";\n    if (!ok || !stopped) return 1;')
                (variant / "main.cpp").write_text(changed)
                build = isolated / f"{name} build"
                run("cmake", "-S", variant, "-B", build, "-DCMAKE_BUILD_TYPE=Release", prefix_option)
                run("cmake", "--build", build, "--config", "Release", "--parallel", "2")
                if diagnostic:
                    output = run(executable(build), failure=diagnostic)
                    if name == "step" and "checked stop reached" not in output:
                        raise RuntimeError("step failure did not reach successful checked stop")
                    if "stopped=ok" in output:
                        raise RuntimeError("failure emitted success output")
                else:
                    run("ctest", "--test-dir", build, "-C", "Release", "--output-on-failure",
                        failure="hello Runtime failed")
            print("PASS: hello relocated/embedded/full SDK, isolation and five negative controls", flush=True)


if __name__ == "__main__":
    main()
