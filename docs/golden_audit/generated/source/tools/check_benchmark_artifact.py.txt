#!/usr/bin/env python3
"""Offline M23 v1 schema, canonical-byte, digest, and arithmetic validation.

This is not a general JSON Schema engine: only the closed keyword subset used
by the two shipped schemas is accepted. No references or network are loaded.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
from typing import Any

MAX_BYTES = 32 * 1024 * 1024
MAX_INT = (1 << 63) - 1
FILES = {"descriptor.json", "raw.json", "result.json"}
ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def canonical(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, ensure_ascii=True, allow_nan=False,
                       separators=(",", ":")) + "\n").encode("ascii")


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        require(name not in result, "duplicate JSON field")
        result[name] = value
    return result


def _no_float(_: str) -> None:
    raise ValueError("non-integer JSON number")


def decode(raw: bytes, *, canonical_required: bool = True) -> Any:
    require(len(raw) <= MAX_BYTES, "artifact too large")
    depth = 0
    quoted = escaped = False
    for char in raw:
        if quoted:
            if escaped:
                escaped = False
            elif char == 92:
                escaped = True
            elif char == 34:
                quoted = False
        elif char == 34:
            quoted = True
        elif char in (91, 123):
            depth += 1
            require(depth <= 32, "JSON nesting limit")
        elif char in (93, 125):
            depth -= 1
    value = json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs,
                       parse_float=_no_float, parse_constant=_no_float)
    if canonical_required:
        require(canonical(value) == raw, "noncanonical JSON bytes")
    return value


def validate_schema(value: Any, schema: dict[str, Any]) -> None:
    """Validate the frozen, deliberately limited v1 schema vocabulary."""
    allowed = {"$schema", "$id", "$defs", "type", "const", "enum", "oneOf", "required",
               "properties", "additionalProperties", "propertyNames", "items", "minimum", "maximum",
               "minItems", "maxItems", "minLength", "maxLength", "pattern", "minProperties", "maxProperties"}
    require(set(schema) <= allowed, "unsupported schema keyword")
    if "oneOf" in schema:
        successes = 0
        for choice in schema["oneOf"]:
            try:
                validate_schema(value, choice)
                successes += 1
            except ValueError:
                pass
        require(successes == 1, "schema oneOf mismatch")
    if "const" in schema:
        require(type(value) is type(schema["const"]) and value == schema["const"], "schema constant mismatch")
    if "enum" in schema:
        require(any(type(value) is type(item) and value == item for item in schema["enum"]), "schema enum mismatch")
    if "type" in schema:
        types = {"object": dict, "array": list, "string": str, "integer": int, "boolean": bool, "null": type(None)}
        require(schema["type"] in types and type(value) is types[schema["type"]], "schema type mismatch")
    if type(value) is int:
        require(schema.get("minimum", 0) <= value <= schema.get("maximum", MAX_INT), "integer out of range")
    if type(value) is str:
        require(schema.get("minLength", 0) <= len(value) <= schema.get("maxLength", 256), "string extent")
        if "pattern" in schema:
            require(re.fullmatch(schema["pattern"], value) is not None, "string format")
    if type(value) is list:
        require(schema.get("minItems", 0) <= len(value) <= schema.get("maxItems", 10000), "array extent")
        for item in value:
            validate_schema(item, schema.get("items", {}))
    if type(value) is dict:
        require(schema.get("minProperties", 0) <= len(value) <= schema.get("maxProperties", 64), "object extent")
        require(set(schema.get("required", [])) <= set(value), "missing required field")
        properties = schema.get("properties", {})
        for key, item in value.items():
            if "propertyNames" in schema:
                validate_schema(key, schema["propertyNames"])
            if key in properties:
                validate_schema(item, properties[key])
            else:
                extra = schema.get("additionalProperties", True)
                require(extra is not False, "unknown field")
                if isinstance(extra, dict):
                    validate_schema(item, extra)


def _regular_file(fd: int) -> bytes:
    info = os.fstat(fd)
    require(stat.S_ISREG(info.st_mode) and info.st_size <= MAX_BYTES, "not a bounded regular file")
    with os.fdopen(os.dup(fd), "rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
    require(len(raw) <= MAX_BYTES, "artifact too large")
    return raw


def _windows_bundle(root: Path) -> dict[str, bytes]:
    # Hold every ancestor against rename; OPEN_REPARSE_POINT checks the object
    # itself before reading, so a junction or leaf symlink is never followed.
    import ctypes
    from ctypes import wintypes
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    create.restype = wintypes.HANDLE
    close = kernel.CloseHandle
    close.argtypes = (wintypes.HANDLE,)
    query = kernel.GetFileInformationByHandleEx
    query.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
    class Tag(ctypes.Structure):
        _fields_ = [("attributes", wintypes.DWORD), ("tag", wintypes.DWORD)]
    handles: list[Any] = []
    def open_checked(path: Path, directory: bool) -> Any:
        access = 0x80 if directory else 0x80000000  # attributes / GENERIC_READ
        handle = create(str(path), access, 1, None, 3, 0x00200000 | 0x02000000, None)
        require(handle != ctypes.c_void_p(-1).value, "cannot open artifact object")
        handles.append(handle)
        info = Tag()
        require(query(handle, 9, ctypes.byref(info), ctypes.sizeof(info)) != 0, "cannot inspect artifact object")
        require(not info.attributes & 0x400, "reparse point forbidden")
        require(bool(info.attributes & 0x10) == directory, "wrong artifact object type")
        return handle
    try:
        current = Path(root.anchor)
        for part in root.parts[1:]:
            current /= part
            open_checked(current, True)
        require({p.name for p in root.iterdir()} == FILES, "incomplete or unlisted artifact files")
        output = {}
        for name in sorted(FILES):
            handle = open_checked(root / name, False)
            fd = msvcrt.open_osfhandle(int(handle), os.O_RDONLY | os.O_BINARY)
            handles.remove(handle)  # fd now owns the HANDLE
            try:
                output[name] = _regular_file(fd)
            finally:
                os.close(fd)
        require({p.name for p in root.iterdir()} == FILES, "artifact listing changed")
        return output
    finally:
        for handle in reversed(handles):
            close(handle)


def read_bundle(root: Path) -> dict[str, bytes]:
    require(".." not in root.parts, "parent traversal forbidden")
    root = root.absolute()
    if os.name == "nt":
        require(not str(root).startswith("\\\\"), "UNC/device path forbidden")
        return _windows_bundle(root)
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    fd = os.open(root.anchor, flags)
    try:
        for part in root.parts[1:]:
            new = os.open(part, flags, dir_fd=fd)
            os.close(fd)
            fd = new
        require(set(os.listdir(fd)) == FILES, "incomplete or unlisted artifact files")
        output = {}
        for name in sorted(FILES):
            leaf = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=fd)
            try:
                output[name] = _regular_file(leaf)
            finally:
                os.close(leaf)
        require(set(os.listdir(fd)) == FILES, "artifact listing changed")
        return output
    finally:
        os.close(fd)


def validate_bundle(root: Path, schema_root: Path | None = None) -> dict[str, Any]:
    schemas = schema_root or ROOT / "bench/schemas"
    ds = decode((schemas / "descriptor.schema.json").read_bytes(), canonical_required=False)
    rs = decode((schemas / "result.schema.json").read_bytes(), canonical_required=False)
    files = read_bundle(root)
    d, raw, result = (decode(files[name]) for name in ("descriptor.json", "raw.json", "result.json"))
    validate_schema(d, ds)
    validate_schema(raw, rs["$defs"]["raw"])
    validate_schema(result, rs)
    for fields in (d["parameters"], d["counters"]):
        require(len({p["name"] for p in fields}) == len(fields), "duplicate parameter/counter")
        for p in fields:
            require(p["minimum"] <= p["maximum"], "reversed range")
            if "value" in p:
                require(p["minimum"] <= p["value"] <= p["maximum"], "parameter outside range")
    require(d["parameters"] == sorted(d["parameters"], key=lambda p: p["name"]), "noncanonical parameter order")
    descriptor_hash = digest(files["descriptor.json"])
    require(result["descriptor_sha256"] == raw["descriptor_sha256"] == descriptor_hash, "descriptor digest mismatch")
    require(result["raw_sha256"] == digest(files["raw.json"]), "raw digest mismatch")
    unhashed = {k: v for k, v in result.items() if k != "result_sha256"}
    require(result["result_sha256"] == digest(canonical(unhashed)), "result digest mismatch")
    context = {"descriptor_sha256": descriptor_hash, "identity": result["identity"], "start_utc": result["start_utc"]}
    # C++ context object intentionally has no trailing LF; file digests do.
    require(result["run_context_sha256"] == raw["run_context_sha256"] == digest(canonical(context)[:-1]), "cross-run context mismatch")
    require(result["workload_sha256"] == d["workload_sha256"], "workload mismatch")
    expected_class = "structural_fixture" if d["clock"] == "fake" else "portable_characterization"
    require(result["clock"] == d["clock"] and result["evidence_class"] == d["evidence_class"] == expected_class, "clock/evidence mismatch")
    for field in ("start_utc", "end_utc"):
        if result[field] != "not_available":
            datetime.datetime.strptime(result[field], "%Y-%m-%dT%H:%M:%SZ")
    if d["clock"] == "fake":
        require(result["start_utc"] == result["end_utc"] == "1970-01-01T00:00:00Z", "fake UTC mismatch")
    elif "not_available" not in (result["start_utc"], result["end_utc"]):
        require(result["start_utc"] <= result["end_utc"], "reversed UTC")
    samples = raw["samples"]
    require(result["measured_completed"] == len(samples) <= d["repetitions"], "sample count mismatch")
    require(result["warmup_completed"] <= d["warmup"], "warmup count mismatch")
    if samples:
        require(result["warmup_completed"] == d["warmup"], "measured before warmup complete")
    counters = {p["name"]: p for p in d["counters"]}
    totals = dict.fromkeys(counters, 0)
    durations = []
    previous = 0
    for n, sample in enumerate(samples):
        require(sample["index"] == n and previous <= sample["start_ns"] <= sample["end_ns"], "sample timeline mismatch")
        require(sample["duration_ns"] == sample["end_ns"] - sample["start_ns"], "duration mismatch")
        previous = sample["end_ns"]
        durations.append(sample["duration_ns"])
        require(set(sample["counters"]) == set(counters), "counter inventory mismatch")
        for name, value in sample["counters"].items():
            require(counters[name]["minimum"] <= value <= counters[name]["maximum"], "counter invariant failed")
            totals[name] += value
            require(totals[name] <= MAX_INT, "counter total overflow")
    require(sum(durations) <= MAX_INT, "duration total overflow")
    diagnostics = {"ok": {""}, "provider_error": {"provider callback threw", "provider invocation failed"},
                   "clock_error": {"invalid start clock", "invalid end clock", "clock callback threw"},
                   "invariant_failed": {"counter or correctness invariant failed", "counter aggregate overflow", "duration aggregate overflow"},
                   "not_run": {"prerequisite unavailable"}}
    require(result["diagnostic"] in diagnostics[result["status"]], "unsupported diagnostic")
    if result["status"] == "ok":
        require(len(samples) == d["repetitions"] and result["warmup_completed"] == d["warmup"], "incomplete successful run")
        require(result["diagnostic"] == "", "successful run has diagnostic")
        ordered = sorted(durations)
        expected = {"counter_totals": totals, "min_ns": min(ordered), "max_ns": max(ordered),
                    "total_ns": sum(ordered), "percentile_method": "nearest_rank"}
        for p in (50, 95, 99):
            expected[f"p{p}_ns"] = ordered[(len(ordered) * p + 99) // 100 - 1]
        require(result["statistics"] == expected, "summary arithmetic mismatch")
    else:
        require(result["statistics"] is None and result["diagnostic"] != "", "failed run has successful summary")
        require(len(samples) < d["repetitions"], "failure after claimed completion")
        if result["status"] == "not_run":
            require(not samples and result["warmup_completed"] == 0 and result["diagnostic"] == "prerequisite unavailable", "invalid not-run result")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--schema-root", type=Path)
    args = parser.parse_args()
    try:
        result = validate_bundle(args.artifact_root, args.schema_root)
        print(f"Benchmark artifact valid: status={result['status']} evidence={result['evidence_class']}")
        return 0 if result["status"] == "ok" else 3 if result["status"] == "not_run" else 1
    except (ValueError, OSError, RecursionError, UnicodeError):
        # Do not echo parsed user content or potentially private path strings.
        print("Benchmark artifact rejected", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
