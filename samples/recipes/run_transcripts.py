#!/usr/bin/env python3
"""Run the exact documented recipe argv vectors; no shell evaluation."""
import argparse
import json
from pathlib import Path
import subprocess


def execute(commands, values, *, timeout=600):
    for command in commands:
        if set(command) != {"argv", "contains"} or not command["argv"]:
            raise ValueError("invalid transcript command")
        argv = [word.format_map(values) for word in command["argv"]]
        print("+", json.dumps(argv), flush=True)
        result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        print(result.stdout, end="", flush=True)
        if result.returncode != 0:
            raise RuntimeError(f"transcript command failed: {result.returncode}")
        if command["contains"] not in result.stdout:
            raise RuntimeError(f"transcript output mismatch: {command['contains']!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--benchmark", choices=["ON", "OFF"], default="OFF")
    args = parser.parse_args()
    kit = Path(__file__).resolve().parent
    data = json.loads((kit / "transcripts.json").read_text())
    if data["schema_version"] != 1:
        raise ValueError("unsupported transcript schema")
    execute(data["commands"], {"kit": str(kit), "build": str(args.build.resolve()),
            "prefix": str(args.prefix.resolve()), "source": str(args.source.resolve()) if args.source else "",
            "benchmark": args.benchmark})


if __name__ == "__main__":
    main()
