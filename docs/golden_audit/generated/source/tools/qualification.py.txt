#!/usr/bin/env python3
"""Bounded offline validation and proposal tooling for RTFW qualification."""

from __future__ import annotations

import argparse
import decimal
import datetime
import hashlib
import json
import math
import os
import pathlib
import re
import stat
import sys
import tempfile
import unicodedata
from dataclasses import dataclass
from typing import Any, Callable, Iterable


SCHEMA_VERSION = 1
SCOPES = ("nvidia", "xdma", "combined", "rt1", "rt2")
EVIDENCE_CLASSES = ("synthetic_fixture", "qualification_campaign")
TRIAL_KINDS = (
    "functional",
    "endurance",
    "thermal",
    "saturation",
    "device_loss",
    "reset_rebind",
    "shutdown",
)
RAW_POPULATIONS = (
    "release",
    "wake",
    "compute",
    "submit",
    "poll",
    "completion",
    "slack",
    "miss",
    "temperature",
    "clock",
    "power",
    "memory",
    "queue",
    "error",
    "recovery",
)
MAX_JSON_BYTES = 1024 * 1024
MAX_NESTING_DEPTH = 32
MAX_STRING_CHARS = 4096
MAX_COLLECTION_ITEMS = 4096
MAX_ERRORS = 128
MAX_ARTIFACTS = 256
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024
MAX_ARTIFACT_TOTAL_BYTES = 512 * 1024 * 1024
MAX_ARTIFACT_TREE_DEPTH = 8
MAX_ARTIFACT_TREE_ENTRIES = MAX_ARTIFACTS * (MAX_ARTIFACT_TREE_DEPTH + 1)
MAX_DURATION_SECONDS = 31_536_000
MAX_SAMPLE_COUNT = 10_000_000
MAX_TRIALS = 64
MAX_THRESHOLDS = 128
MAX_EXCEPTIONS = 32
MAX_IDENTIFIER = 128
MAX_TEXT = 1024
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
COMMIT_PATTERN = re.compile(r"[0-9a-f]{40}")
IDENTIFIER_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,127}")
TIME_PATTERN = re.compile(
    r"\d{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12]\d|3[01])T"
    r"(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\dZ"
)


class DuplicateKeyError(ValueError):
    pass


class ValidationFailure(ValueError):
    def __init__(self, errors: Iterable[str]):
        self.errors = tuple(errors)
        super().__init__("; ".join(self.errors))


class ArtifactLimitError(ValueError):
    pass


class ErrorCollector:
    def __init__(self) -> None:
        self.errors: list[str] = []

    def add(self, message: str) -> None:
        if len(self.errors) < MAX_ERRORS:
            self.errors.append(message)
        elif len(self.errors) == MAX_ERRORS:
            self.errors.append("additional validation errors omitted")

    def require(self, condition: bool, message: str) -> bool:
        if not condition:
            self.add(message)
        return condition

    def finish(self) -> None:
        if self.errors:
            raise ValidationFailure(self.errors)


@dataclass(frozen=True)
class LoadedDocument:
    path: pathlib.Path
    raw: bytes
    value: dict[str, Any]
    sha256: str


@dataclass(frozen=True)
class ValidatedSet:
    plan: LoadedDocument
    record: LoadedDocument
    review: LoadedDocument
    artifact_manifest_sha256: str
    artifact_digests: tuple[dict[str, Any], ...]


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number is forbidden: {value}")


def _lexical_depth(raw: bytes) -> int:
    depth = 0
    maximum = 0
    in_string = False
    escaped = False
    for byte in raw:
        character = chr(byte)
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
        elif character == '"':
            in_string = True
        elif character in "[{":
            depth += 1
            maximum = max(maximum, depth)
            if maximum > MAX_NESTING_DEPTH:
                return maximum
        elif character in "]}":
            depth -= 1
    return maximum


def _bounded_walk(value: Any, path: str, errors: ErrorCollector, depth: int = 0) -> None:
    if depth > MAX_NESTING_DEPTH:
        errors.add(f"{path}: nesting exceeds {MAX_NESTING_DEPTH}")
        return
    if isinstance(value, str):
        errors.require(
            len(value) <= MAX_STRING_CHARS,
            f"{path}: string exceeds {MAX_STRING_CHARS} characters",
        )
    elif isinstance(value, list):
        if not errors.require(
            len(value) <= MAX_COLLECTION_ITEMS,
            f"{path}: array exceeds {MAX_COLLECTION_ITEMS} items",
        ):
            return
        for index, item in enumerate(value):
            _bounded_walk(item, f"{path}[{index}]", errors, depth + 1)
    elif isinstance(value, dict):
        if not errors.require(
            len(value) <= MAX_COLLECTION_ITEMS,
            f"{path}: object exceeds {MAX_COLLECTION_ITEMS} properties",
        ):
            return
        for key, item in value.items():
            if not isinstance(key, str):
                errors.add(f"{path}: object key must be a string")
                continue
            _bounded_walk(key, f"{path}.<key>", errors, depth + 1)
            _bounded_walk(item, f"{path}.{key}", errors, depth + 1)


def load_document(path: pathlib.Path, expected_type: str) -> LoadedDocument:
    errors = ErrorCollector()
    try:
        if path.is_symlink() or not path.is_file():
            raise ValueError("document must be a regular non-symlink file")
        size = path.stat().st_size
        if size > MAX_JSON_BYTES:
            raise ValueError(f"document exceeds {MAX_JSON_BYTES} bytes")
        raw = path.read_bytes()
        if raw.startswith(b"\xef\xbb\xbf"):
            raise ValueError("UTF-8 BOM is forbidden")
        if _lexical_depth(raw) > MAX_NESTING_DEPTH:
            raise ValueError(f"JSON nesting exceeds {MAX_NESTING_DEPTH}")
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=_reject_duplicate_keys,
            parse_float=decimal.Decimal,
            parse_int=int,
            parse_constant=_reject_constant,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError, ValueError) as exc:
        raise ValidationFailure((f"{expected_type}: cannot parse: {exc}",)) from exc
    if not isinstance(value, dict):
        errors.add(f"{expected_type}: top-level value must be an object")
    else:
        _bounded_walk(value, expected_type, errors)
        if value.get("document_type") != expected_type:
            errors.add(f"{expected_type}: document_type mismatch")
        if value.get("schema_version") != SCHEMA_VERSION:
            errors.add(f"{expected_type}: schema_version must be {SCHEMA_VERSION}")
    errors.finish()
    return LoadedDocument(path, raw, value, hashlib.sha256(raw).hexdigest())


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    return (
        (_is_int(value) or isinstance(value, decimal.Decimal))
        and decimal.Decimal(value).is_finite()
    )


def _closed_object(
    value: Any,
    path: str,
    required: set[str],
    optional: set[str],
    errors: ErrorCollector,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.add(f"{path}: must be an object")
        return {}
    keys = set(value)
    for key in sorted(required - keys):
        errors.add(f"{path}: missing required property {key}")
    for key in sorted(keys - required - optional):
        errors.add(f"{path}: unknown property {key}")
    return value


def _string(
    value: Any,
    path: str,
    errors: ErrorCollector,
    *,
    minimum: int = 1,
    maximum: int = MAX_TEXT,
    pattern: re.Pattern[str] | None = None,
) -> str:
    if not isinstance(value, str):
        errors.add(f"{path}: must be a string")
        return ""
    if not minimum <= len(value) <= maximum:
        errors.add(f"{path}: length must be between {minimum} and {maximum}")
    if any(ord(character) < 0x20 for character in value):
        errors.add(f"{path}: control characters are forbidden")
    if pattern is not None and pattern.fullmatch(value) is None:
        errors.add(f"{path}: has a noncanonical value")
    return value


def _integer(
    value: Any,
    path: str,
    errors: ErrorCollector,
    minimum: int,
    maximum: int,
) -> int:
    if not _is_int(value) or not minimum <= value <= maximum:
        errors.add(f"{path}: must be an integer in [{minimum}, {maximum}]")
        return minimum
    return value


def _boolean(value: Any, path: str, errors: ErrorCollector) -> bool:
    if not isinstance(value, bool):
        errors.add(f"{path}: must be a boolean")
        return False
    return value


def _identifier(value: Any, path: str, errors: ErrorCollector) -> str:
    return _string(
        value,
        path,
        errors,
        maximum=MAX_IDENTIFIER,
        pattern=IDENTIFIER_PATTERN,
    )


def _digest(value: Any, path: str, errors: ErrorCollector) -> str:
    return _string(value, path, errors, minimum=64, maximum=64, pattern=SHA256_PATTERN)


def _commit(value: Any, path: str, errors: ErrorCollector) -> str:
    return _string(value, path, errors, minimum=40, maximum=40, pattern=COMMIT_PATTERN)


def _timestamp(value: Any, path: str, errors: ErrorCollector) -> str:
    result = _string(value, path, errors, minimum=20, maximum=20, pattern=TIME_PATTERN)
    if TIME_PATTERN.fullmatch(result):
        try:
            datetime.datetime.strptime(result, "%Y-%m-%dT%H:%M:%SZ")
        except ValueError:
            errors.add(f"{path}: invalid UTC calendar time")
    return result


def _unique_ids(
    values: Any,
    path: str,
    errors: ErrorCollector,
    *,
    minimum: int,
    maximum: int,
) -> list[dict[str, Any]]:
    if not isinstance(values, list) or not minimum <= len(values) <= maximum:
        errors.add(f"{path}: item count must be in [{minimum}, {maximum}]")
        return []
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, value in enumerate(values):
        if not isinstance(value, dict):
            errors.add(f"{path}[{index}]: must be an object")
            continue
        identity = _identifier(value.get("id"), f"{path}[{index}].id", errors)
        if identity in seen:
            errors.add(f"{path}: duplicate identity {identity}")
        seen.add(identity)
        result.append(value)
    return result


def _validate_source(value: Any, path: str, errors: ErrorCollector) -> dict[str, Any]:
    obj = _closed_object(
        value,
        path,
        {"commit", "build_id", "product_version"},
        set(),
        errors,
    )
    _commit(obj.get("commit"), f"{path}.commit", errors)
    _identifier(obj.get("build_id"), f"{path}.build_id", errors)
    _string(
        obj.get("product_version"),
        f"{path}.product_version",
        errors,
        maximum=32,
        pattern=re.compile(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)"),
    )
    return obj


def _validate_host(value: Any, path: str, errors: ErrorCollector) -> dict[str, Any]:
    obj = _closed_object(
        value,
        path,
        {"cpu", "motherboard", "bios_firmware", "memory", "operating_system", "kernel", "compiler"},
        set(),
        errors,
    )
    for name in ("cpu", "motherboard", "bios_firmware", "memory", "operating_system", "compiler"):
        _string(obj.get(name), f"{path}.{name}", errors)
    kernel = _closed_object(
        obj.get("kernel"),
        f"{path}.kernel",
        {"version", "configuration_sha256", "boot_parameters"},
        set(),
        errors,
    )
    _string(kernel.get("version"), f"{path}.kernel.version", errors)
    _digest(kernel.get("configuration_sha256"), f"{path}.kernel.configuration_sha256", errors)
    _string(kernel.get("boot_parameters"), f"{path}.kernel.boot_parameters", errors)
    return obj


def _validate_topology(value: Any, path: str, errors: ErrorCollector) -> dict[str, Any]:
    obj = _closed_object(
        value,
        path,
        {"numa", "iommu", "irq", "power"},
        set(),
        errors,
    )
    for name in ("numa", "iommu", "irq", "power"):
        _string(obj.get(name), f"{path}.{name}", errors)
    return obj


def _validate_policy(
    value: Any,
    path: str,
    errors: ErrorCollector,
    scope: str,
) -> dict[str, Any]:
    obj = _closed_object(
        value,
        path,
        {"runtime", "threads", "memory", "time", "measured_host_policy", "rt2"},
        set(),
        errors,
    )
    for name in ("runtime", "threads", "memory", "time", "measured_host_policy"):
        _string(obj.get(name), f"{path}.{name}", errors)
    rt2 = _closed_object(
        obj.get("rt2"),
        f"{path}.rt2",
        {"applicable", "preempt_rt", "isolation", "irq_placement", "scheduling", "locking"},
        set(),
        errors,
    )
    applicable = _boolean(rt2.get("applicable"), f"{path}.rt2.applicable", errors)
    preempt_rt = _boolean(rt2.get("preempt_rt"), f"{path}.rt2.preempt_rt", errors)
    for name in ("isolation", "irq_placement", "scheduling", "locking"):
        _string(rt2.get(name), f"{path}.rt2.{name}", errors)
    if scope == "rt2":
        errors.require(applicable and preempt_rt, f"{path}.rt2: RT2 requires applicable PREEMPT_RT evidence")
    else:
        errors.require(not applicable and not preempt_rt, f"{path}.rt2: must be inapplicable outside RT2")
    return obj


def _validate_workload(value: Any, path: str, errors: ErrorCollector) -> dict[str, Any]:
    obj = _closed_object(value, path, {"id", "input_id", "input_sha256"}, set(), errors)
    _identifier(obj.get("id"), f"{path}.id", errors)
    _identifier(obj.get("input_id"), f"{path}.input_id", errors)
    _digest(obj.get("input_sha256"), f"{path}.input_sha256", errors)
    return obj


def _validate_measurement(value: Any, path: str, errors: ErrorCollector) -> dict[str, Any]:
    obj = _closed_object(
        value,
        path,
        {"warmup_samples", "duration_seconds", "sample_count"},
        set(),
        errors,
    )
    _integer(obj.get("warmup_samples"), f"{path}.warmup_samples", errors, 0, MAX_SAMPLE_COUNT)
    _integer(obj.get("duration_seconds"), f"{path}.duration_seconds", errors, 1, MAX_DURATION_SECONDS)
    _integer(obj.get("sample_count"), f"{path}.sample_count", errors, 1, MAX_SAMPLE_COUNT)
    return obj


def _validate_nvidia(value: Any, path: str, errors: ErrorCollector) -> dict[str, Any]:
    obj = _closed_object(
        value,
        path,
        {"gpu", "pci_bdf", "pcie_link", "cuda_driver", "cuda_toolkit", "firmware", "power_policy", "clock_policy"},
        set(),
        errors,
    )
    for name in obj:
        _string(obj.get(name), f"{path}.{name}", errors)
    return obj


def _validate_xdma(value: Any, path: str, errors: ErrorCollector) -> dict[str, Any]:
    obj = _closed_object(
        value,
        path,
        {"fpga_part", "pci_bdf", "pcie_link", "driver_revision", "module_parameters", "firmware_or_bitstream_sha256", "ip_configuration", "memory_map"},
        set(),
        errors,
    )
    for name in obj:
        if name == "firmware_or_bitstream_sha256":
            _digest(obj.get(name), f"{path}.{name}", errors)
        else:
            _string(obj.get(name), f"{path}.{name}", errors)
    return obj


def _validate_accelerator(
    value: Any,
    path: str,
    errors: ErrorCollector,
    scope: str,
) -> dict[str, Any]:
    obj = _closed_object(value, path, set(), {"nvidia", "xdma", "host_staging"}, errors)
    has_nvidia = "nvidia" in obj
    has_xdma = "xdma" in obj
    has_staging = "host_staging" in obj
    if has_nvidia:
        _validate_nvidia(obj["nvidia"], f"{path}.nvidia", errors)
    if has_xdma:
        _validate_xdma(obj["xdma"], f"{path}.xdma", errors)
    if has_staging:
        staging = _closed_object(
            obj["host_staging"],
            f"{path}.host_staging",
            {"path", "direct_peer_dma"},
            set(),
            errors,
        )
        _string(staging.get("path"), f"{path}.host_staging.path", errors)
        direct = _boolean(staging.get("direct_peer_dma"), f"{path}.host_staging.direct_peer_dma", errors)
        errors.require(not direct, f"{path}.host_staging.direct_peer_dma must be false")
    required = {
        "nvidia": (True, False, False),
        "xdma": (False, True, False),
        "combined": (True, True, True),
        "rt1": (False, False, False),
        "rt2": (False, False, False),
    }.get(scope)
    if required is not None:
        errors.require(
            (has_nvidia, has_xdma, has_staging) == required,
            f"{path}: accelerator identities do not match scope {scope}",
        )
    return obj


def _validate_trials(value: Any, path: str, errors: ErrorCollector, scope: str) -> list[dict[str, Any]]:
    trials = _unique_ids(value, path, errors, minimum=len(TRIAL_KINDS), maximum=MAX_TRIALS)
    kinds: set[str] = set()
    device_scope = scope in ("nvidia", "xdma", "combined")
    for index, trial in enumerate(trials):
        item = _closed_object(
            trial,
            f"{path}[{index}]",
            {"id", "kind", "required", "applicability", "rationale"},
            set(),
            errors,
        )
        kind = item.get("kind")
        if kind not in TRIAL_KINDS:
            errors.add(f"{path}[{index}].kind: invalid trial kind")
        elif kind in kinds:
            errors.add(f"{path}: duplicate trial kind {kind}")
        else:
            kinds.add(kind)
        required = _boolean(item.get("required"), f"{path}[{index}].required", errors)
        applicability = item.get("applicability")
        if applicability not in ("applicable", "not_applicable"):
            errors.add(f"{path}[{index}].applicability: invalid value")
        _string(item.get("rationale"), f"{path}[{index}].rationale", errors)
        if kind in ("device_loss", "reset_rebind") and not device_scope:
            errors.require(not required and applicability == "not_applicable", f"{path}[{index}]: device trial must be explicitly not applicable")
        else:
            errors.require(required and applicability == "applicable", f"{path}[{index}]: mandatory trial must be applicable and required")
    errors.require(kinds == set(TRIAL_KINDS), f"{path}: must cover every required trial kind exactly once")
    return trials


def _validate_thresholds(value: Any, path: str, errors: ErrorCollector) -> list[dict[str, Any]]:
    thresholds = _unique_ids(value, path, errors, minimum=1, maximum=MAX_THRESHOLDS)
    for index, threshold in enumerate(thresholds):
        item = _closed_object(
            threshold,
            f"{path}[{index}]",
            {"id", "metric", "unit", "population", "statistic", "comparison", "bound", "miss_allowance", "error_allowance"},
            set(),
            errors,
        )
        for name in ("metric", "unit", "population", "statistic"):
            _identifier(item.get(name), f"{path}[{index}].{name}", errors)
        if item.get("comparison") not in ("le", "lt", "ge", "gt", "eq"):
            errors.add(f"{path}[{index}].comparison: invalid comparison")
        if not _is_number(item.get("bound")):
            errors.add(f"{path}[{index}].bound: must be a finite number")
        _integer(item.get("miss_allowance"), f"{path}[{index}].miss_allowance", errors, 0, MAX_SAMPLE_COUNT)
        _integer(item.get("error_allowance"), f"{path}[{index}].error_allowance", errors, 0, MAX_SAMPLE_COUNT)
    return thresholds


def validate_plan(plan: dict[str, Any], errors: ErrorCollector) -> None:
    obj = _closed_object(
        plan,
        "plan",
        {"schema_version", "document_type", "evidence_class", "campaign_id", "tuple_id", "scope", "source", "host", "topology", "policy", "workload", "measurement", "accelerator", "trials", "thresholds"},
        set(),
        errors,
    )
    if obj.get("evidence_class") not in EVIDENCE_CLASSES:
        errors.add("plan.evidence_class: invalid value")
    _identifier(obj.get("campaign_id"), "plan.campaign_id", errors)
    _identifier(obj.get("tuple_id"), "plan.tuple_id", errors)
    scope = obj.get("scope")
    if scope not in SCOPES:
        errors.add("plan.scope: invalid value")
        scope = ""
    _validate_source(obj.get("source"), "plan.source", errors)
    _validate_host(obj.get("host"), "plan.host", errors)
    _validate_topology(obj.get("topology"), "plan.topology", errors)
    _validate_policy(obj.get("policy"), "plan.policy", errors, scope)
    _validate_workload(obj.get("workload"), "plan.workload", errors)
    _validate_measurement(obj.get("measurement"), "plan.measurement", errors)
    _validate_accelerator(obj.get("accelerator"), "plan.accelerator", errors, scope)
    _validate_trials(obj.get("trials"), "plan.trials", errors, scope)
    thresholds = _validate_thresholds(obj.get("thresholds"), "plan.thresholds", errors)
    populations = {threshold.get("population") for threshold in thresholds}
    if scope in ("rt1", "rt2"):
        errors.require("latency" in populations, "plan.thresholds: RT1/RT2 requires latency population")
        errors.require("deadline" in populations, "plan.thresholds: RT1/RT2 requires deadline population")


def _safe_relative_path(raw: Any) -> pathlib.PurePosixPath | None:
    if (
        not isinstance(raw, str)
        or not raw
        or len(raw) > 512
        or "\\" in raw
        or ":" in raw
        or "//" in raw
        or any(ord(character) < 0x20 for character in raw)
        or unicodedata.normalize("NFC", raw) != raw
    ):
        return None
    path = pathlib.PurePosixPath(raw)
    reserved = {"CON", "PRN", "AUX", "NUL"} | {
        f"{prefix}{index}" for prefix in ("COM", "LPT") for index in range(1, 10)
    }
    if (
        path.is_absolute()
        or path.as_posix() != raw
        or any(part in ("", ".", "..") for part in path.parts)
        or any(part.endswith((" ", ".")) for part in path.parts)
        or any(part.split(".", 1)[0].upper() in reserved for part in path.parts)
    ):
        return None
    if len(path.parts) > MAX_ARTIFACT_TREE_DEPTH:
        return None
    return path


def _canonical_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def _manifest_digest(entries: list[dict[str, Any]]) -> str:
    domain = b"rtfw-qualification-artifact-manifest-v1\0"
    return hashlib.sha256(domain + _canonical_bytes(entries)).hexdigest()


def _validate_manifest_shape(value: Any, path: str, errors: ErrorCollector) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not 1 <= len(value) <= MAX_ARTIFACTS:
        errors.add(f"{path}: artifact count must be in [1, {MAX_ARTIFACTS}]")
        return []
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    portable_seen: set[str] = set()
    previous = ""
    aggregate = 0
    for index, entry in enumerate(value):
        item = _closed_object(entry, f"{path}[{index}]", {"path", "size_bytes", "sha256"}, set(), errors)
        relative = _safe_relative_path(item.get("path"))
        raw = item.get("path") if isinstance(item.get("path"), str) else ""
        if relative is None:
            errors.add(f"{path}[{index}].path: unsafe or noncanonical path")
        if raw in seen:
            errors.add(f"{path}: duplicate artifact path {raw}")
        seen.add(raw)
        portable = unicodedata.normalize("NFC", raw).casefold()
        if portable in portable_seen:
            errors.add(f"{path}: nonportable artifact path collision {raw}")
        portable_seen.add(portable)
        if index and raw <= previous:
            errors.add(f"{path}: entries must be strictly sorted by path")
        previous = raw
        size = _integer(item.get("size_bytes"), f"{path}[{index}].size_bytes", errors, 0, MAX_ARTIFACT_BYTES)
        aggregate += size
        _digest(item.get("sha256"), f"{path}[{index}].sha256", errors)
        result.append(item)
    errors.require(aggregate <= MAX_ARTIFACT_TOTAL_BYTES, f"{path}: aggregate bytes exceed {MAX_ARTIFACT_TOTAL_BYTES}")
    return result


def _validate_raw_populations(value: Any, path: str, errors: ErrorCollector) -> list[dict[str, Any]]:
    items = _unique_ids(value, path, errors, minimum=len(RAW_POPULATIONS), maximum=len(RAW_POPULATIONS))
    names: set[str] = set()
    for index, item in enumerate(items):
        obj = _closed_object(item, f"{path}[{index}]", {"id", "artifact_path"}, set(), errors)
        name = obj.get("id")
        if name not in RAW_POPULATIONS:
            errors.add(f"{path}[{index}].id: unknown raw population")
        else:
            names.add(name)
        if _safe_relative_path(obj.get("artifact_path")) is None:
            errors.add(f"{path}[{index}].artifact_path: unsafe path")
    errors.require(names == set(RAW_POPULATIONS), f"{path}: incomplete raw population set")
    return items


def _validate_trends(value: Any, path: str, errors: ErrorCollector, *, thermal: bool) -> list[dict[str, Any]]:
    items = _unique_ids(value, path, errors, minimum=1, maximum=64)
    for index, item in enumerate(items):
        obj = _closed_object(item, f"{path}[{index}]", {"id", "start", "end", "maximum", "stable"}, set(), errors)
        values: dict[str, decimal.Decimal] = {}
        for name in ("start", "end", "maximum"):
            if not _is_number(obj.get(name)):
                errors.add(f"{path}[{index}].{name}: must be a finite number")
            else:
                values[name] = decimal.Decimal(obj[name])
        stable = _boolean(obj.get("stable"), f"{path}[{index}].stable", errors)
        errors.require(stable, f"{path}[{index}]: trend must be stable")
        if len(values) == 3:
            errors.require(
                values["maximum"] >= max(values["start"], values["end"]),
                f"{path}[{index}].maximum: must cover start and end",
            )
            if thermal:
                for name, measured in values.items():
                    errors.require(
                        decimal.Decimal("-100") <= measured <= decimal.Decimal("300"),
                        f"{path}[{index}].{name}: thermal value out of range",
                    )
            else:
                errors.require(
                    values["end"] <= values["start"],
                    f"{path}[{index}]: stable resource trend must not grow",
                )
    return items


def _threshold_passed(comparison: str, observed: decimal.Decimal, bound: decimal.Decimal) -> bool:
    return {
        "le": observed <= bound,
        "lt": observed < bound,
        "ge": observed >= bound,
        "gt": observed > bound,
        "eq": observed == bound,
    }.get(comparison, False)


def _exact_equal(left: Any, right: Any) -> bool:
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return set(left) == set(right) and all(
            _exact_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _exact_equal(a, b) for a, b in zip(left, right)
        )
    return left == right


def validate_record(record: dict[str, Any], plan: dict[str, Any], plan_digest: str, errors: ErrorCollector) -> None:
    obj = _closed_object(
        record,
        "record",
        {"schema_version", "document_type", "evidence_class", "campaign_id", "tuple_id", "scope", "plan_sha256", "source", "host", "topology", "policy", "workload", "accelerator", "started_at", "ended_at", "artifact_manifest", "evidence_manifest_sha256", "raw_populations", "resource_trends", "thermal_trends", "device_health", "trials", "thresholds", "overall_result"},
        set(),
        errors,
    )
    for name in ("evidence_class", "campaign_id", "tuple_id", "scope", "source", "host", "topology", "policy", "workload", "accelerator"):
        errors.require(obj.get(name) == plan.get(name), f"record.{name}: diverges from plan")
    _digest(obj.get("plan_sha256"), "record.plan_sha256", errors)
    errors.require(obj.get("plan_sha256") == plan_digest, "record.plan_sha256: exact plan digest mismatch")
    started = _timestamp(obj.get("started_at"), "record.started_at", errors)
    ended = _timestamp(obj.get("ended_at"), "record.ended_at", errors)
    errors.require(not started or not ended or started < ended, "record: start time must precede end time")
    manifest = _validate_manifest_shape(obj.get("artifact_manifest"), "record.artifact_manifest", errors)
    expected_manifest_digest = _manifest_digest(manifest)
    _digest(obj.get("evidence_manifest_sha256"), "record.evidence_manifest_sha256", errors)
    errors.require(obj.get("evidence_manifest_sha256") == expected_manifest_digest, "record.evidence_manifest_sha256: canonical manifest digest mismatch")
    raw = _validate_raw_populations(obj.get("raw_populations"), "record.raw_populations", errors)
    listed_paths = {entry.get("path") for entry in manifest}
    for entry in raw:
        errors.require(entry.get("artifact_path") in listed_paths, f"record.raw_populations: unlisted artifact reference {entry.get('artifact_path')}")
    _validate_trends(obj.get("resource_trends"), "record.resource_trends", errors, thermal=False)
    _validate_trends(obj.get("thermal_trends"), "record.thermal_trends", errors, thermal=True)
    health = _closed_object(obj.get("device_health"), "record.device_health", {"healthy", "losses", "recovered"}, set(), errors)
    healthy = _boolean(health.get("healthy"), "record.device_health.healthy", errors)
    losses = _integer(health.get("losses"), "record.device_health.losses", errors, 0, MAX_SAMPLE_COUNT)
    recovered = _boolean(health.get("recovered"), "record.device_health.recovered", errors)
    errors.require(healthy and losses == 0 and recovered, "record.device_health: passing record requires healthy recovered device state with zero unresolved losses")

    planned_trials = {item.get("id"): item for item in plan.get("trials", []) if isinstance(item, dict)}
    results = _unique_ids(obj.get("trials"), "record.trials", errors, minimum=len(planned_trials), maximum=MAX_TRIALS)
    errors.require({item.get("id") for item in results} == set(planned_trials), "record.trials: result identities differ from plan")
    for index, result in enumerate(results):
        trial = _closed_object(result, f"record.trials[{index}]", {"id", "status", "artifact_path", "recovery"}, set(), errors)
        identity = trial.get("id")
        planned = planned_trials.get(identity, {})
        status_value = trial.get("status")
        if status_value not in ("pass", "not_applicable"):
            errors.add(f"record.trials[{index}].status: passing record requires pass or not_applicable")
        expected_status = "pass" if planned.get("applicability") == "applicable" else "not_applicable"
        errors.require(status_value == expected_status, f"record.trials[{index}].status: conflicts with planned applicability")
        artifact_path = trial.get("artifact_path")
        if status_value == "pass":
            errors.require(_safe_relative_path(artifact_path) is not None and artifact_path in listed_paths, f"record.trials[{index}].artifact_path: must reference a listed artifact")
        elif artifact_path is not None:
            errors.add(f"record.trials[{index}].artifact_path: must be null when not applicable")
        recovery = _closed_object(trial.get("recovery"), f"record.trials[{index}].recovery", {"attempted", "success", "details"}, set(), errors)
        attempted = _boolean(recovery.get("attempted"), f"record.trials[{index}].recovery.attempted", errors)
        success = _boolean(recovery.get("success"), f"record.trials[{index}].recovery.success", errors)
        _string(recovery.get("details"), f"record.trials[{index}].recovery.details", errors)
        if planned.get("kind") == "reset_rebind" and planned.get("applicability") == "applicable":
            errors.require(attempted and success, f"record.trials[{index}].recovery: reset-rebind recovery must succeed")
        else:
            errors.require(success, f"record.trials[{index}].recovery: recovery state must be successful")

    planned_thresholds = {item.get("id"): item for item in plan.get("thresholds", []) if isinstance(item, dict)}
    evaluations = _unique_ids(obj.get("thresholds"), "record.thresholds", errors, minimum=len(planned_thresholds), maximum=MAX_THRESHOLDS)
    errors.require({item.get("id") for item in evaluations} == set(planned_thresholds), "record.thresholds: evaluation identities differ from plan")
    for index, evaluation in enumerate(evaluations):
        item = _closed_object(evaluation, f"record.thresholds[{index}]", {"id", "definition", "observed", "misses", "errors", "passed"}, set(), errors)
        planned = planned_thresholds.get(item.get("id"))
        errors.require(_exact_equal(item.get("definition"), planned), f"record.thresholds[{index}].definition: threshold drift")
        observed_value = item.get("observed")
        if not _is_number(observed_value):
            errors.add(f"record.thresholds[{index}].observed: must be a finite number")
            observed = decimal.Decimal(0)
        else:
            observed = decimal.Decimal(observed_value)
        misses = _integer(item.get("misses"), f"record.thresholds[{index}].misses", errors, 0, MAX_SAMPLE_COUNT)
        error_count = _integer(item.get("errors"), f"record.thresholds[{index}].errors", errors, 0, MAX_SAMPLE_COUNT)
        passed = _boolean(item.get("passed"), f"record.thresholds[{index}].passed", errors)
        computed = False
        if isinstance(planned, dict) and _is_number(planned.get("bound")):
            computed = (
                _threshold_passed(str(planned.get("comparison")), observed, decimal.Decimal(planned["bound"]))
                and misses <= planned.get("miss_allowance", -1)
                and error_count <= planned.get("error_allowance", -1)
            )
        errors.require(passed == computed, f"record.thresholds[{index}].passed: does not match predeclared comparison and allowances")
        errors.require(computed, f"record.thresholds[{index}]: threshold failed")
    errors.require(obj.get("overall_result") == "pass", "record.overall_result: passing review requires pass")


def validate_review(
    review: dict[str, Any],
    plan: dict[str, Any],
    record: dict[str, Any],
    plan_digest: str,
    record_digest: str,
    errors: ErrorCollector,
) -> None:
    obj = _closed_object(
        review,
        "review",
        {"schema_version", "document_type", "evidence_class", "scope", "tuple_id", "plan_sha256", "record_sha256", "evidence_manifest_sha256", "reviewer", "reviewed_at", "decision", "rationale", "exceptions", "pre_run_provenance_verified", "reviewer_authentication"},
        set(),
        errors,
    )
    for name in ("evidence_class", "scope", "tuple_id"):
        errors.require(obj.get(name) == plan.get(name) == record.get(name), f"review.{name}: identity mismatch")
    _digest(obj.get("plan_sha256"), "review.plan_sha256", errors)
    _digest(obj.get("record_sha256"), "review.record_sha256", errors)
    _digest(obj.get("evidence_manifest_sha256"), "review.evidence_manifest_sha256", errors)
    errors.require(obj.get("plan_sha256") == plan_digest, "review.plan_sha256: mismatch")
    errors.require(obj.get("record_sha256") == record_digest, "review.record_sha256: mismatch")
    errors.require(obj.get("evidence_manifest_sha256") == record.get("evidence_manifest_sha256"), "review.evidence_manifest_sha256: mismatch")
    _identifier(obj.get("reviewer"), "review.reviewer", errors)
    _timestamp(obj.get("reviewed_at"), "review.reviewed_at", errors)
    errors.require(obj.get("decision") == "pass", "review.decision: explicit passing decision required")
    _string(obj.get("rationale"), "review.rationale", errors)
    exceptions = obj.get("exceptions")
    if not isinstance(exceptions, list) or len(exceptions) > MAX_EXCEPTIONS:
        errors.add(f"review.exceptions: must be an array of at most {MAX_EXCEPTIONS}")
    elif exceptions:
        errors.add("review.exceptions: a proposal cannot carry exceptions")
    chronology = _boolean(obj.get("pre_run_provenance_verified"), "review.pre_run_provenance_verified", errors)
    errors.require(chronology, "review.pre_run_provenance_verified: human external pre-run verification is required")
    errors.require(obj.get("reviewer_authentication") == "attribution_only", "review.reviewer_authentication: must remain attribution_only")


def _sha256_file(path: pathlib.Path, aggregate_remaining: int) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as stream:
        status = os.fstat(stream.fileno())
        if not stat.S_ISREG(status.st_mode):
            raise ValueError("artifact changed to a nonregular file")
        if status.st_size > MAX_ARTIFACT_BYTES:
            raise ArtifactLimitError(
                f"artifact exceeds {MAX_ARTIFACT_BYTES} bytes"
            )
        if status.st_size > aggregate_remaining:
            raise ArtifactLimitError(
                f"aggregate bytes exceed {MAX_ARTIFACT_TOTAL_BYTES}"
            )
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            if size > MAX_ARTIFACT_BYTES:
                raise ArtifactLimitError(
                    f"artifact exceeds {MAX_ARTIFACT_BYTES} bytes"
                )
            if size > aggregate_remaining:
                raise ArtifactLimitError(
                    f"aggregate bytes exceed {MAX_ARTIFACT_TOTAL_BYTES}"
                )
            digest.update(chunk)
    return size, digest.hexdigest()


def _scan_artifact_tree(
    root: pathlib.Path,
    errors: ErrorCollector,
) -> tuple[tuple[pathlib.Path, str, int], ...]:
    stack: list[tuple[pathlib.Path, pathlib.PurePosixPath]] = [
        (root, pathlib.PurePosixPath())
    ]
    files: list[tuple[pathlib.Path, str, int]] = []
    entry_count = 0
    file_count = 0
    while stack:
        directory, relative_directory = stack.pop()
        try:
            names: list[str] = []
            with os.scandir(directory) as entries:
                for entry in entries:
                    entry_count += 1
                    if entry_count > MAX_ARTIFACT_TREE_ENTRIES:
                        errors.add(
                            "artifacts: tree entry count exceeds "
                            f"{MAX_ARTIFACT_TREE_ENTRIES}"
                        )
                        return tuple(files)
                    names.append(entry.name)
        except OSError as exc:
            display = relative_directory.as_posix() or "."
            errors.add(f"artifacts: cannot scan directory {display}: {exc}")
            continue

        for name in reversed(sorted(names)):
            child = directory / name
            relative = relative_directory / name
            raw = relative.as_posix()
            try:
                mode = child.lstat().st_mode
            except OSError as exc:
                errors.add(f"artifacts: cannot inspect directory entry {raw}: {exc}")
                continue
            if stat.S_ISLNK(mode):
                errors.add(f"artifacts: symlink entry {raw}")
                file_count += 1
                if file_count > MAX_ARTIFACTS:
                    errors.add(f"artifacts: file count exceeds {MAX_ARTIFACTS}")
                    return tuple(files)
                continue
            if stat.S_ISDIR(mode):
                if len(relative.parts) > MAX_ARTIFACT_TREE_DEPTH:
                    errors.add(
                        f"artifacts: directory depth exceeds {MAX_ARTIFACT_TREE_DEPTH}"
                    )
                    continue
                stack.append((child, relative))
                continue
            file_count += 1
            if file_count > MAX_ARTIFACTS:
                errors.add(f"artifacts: file count exceeds {MAX_ARTIFACTS}")
                return tuple(files)
            files.append((child, raw, mode))
    return tuple(files)


def verify_artifacts(
    artifact_dir: pathlib.Path,
    manifest: list[dict[str, Any]],
    errors: ErrorCollector,
) -> tuple[dict[str, Any], ...]:
    try:
        if artifact_dir.is_symlink() or not artifact_dir.is_dir():
            raise ValueError("artifact root must be a regular non-symlink directory")
        root = artifact_dir.resolve(strict=True)
    except (OSError, ValueError) as exc:
        errors.add(f"artifacts: invalid root: {exc}")
        return ()
    listed = {str(item.get("path")): item for item in manifest}
    present: set[str] = set()
    aggregate = 0
    for path, raw, mode in _scan_artifact_tree(root, errors):
        present.add(raw)
        try:
            resolved = path.resolve(strict=True)
            resolved.relative_to(root)
        except (OSError, ValueError) as exc:
            errors.add(f"artifacts: invalid file {raw}: {exc}")
            continue
        if not stat.S_ISREG(mode):
            errors.add(f"artifacts: {raw} is not a regular non-symlink file")
            continue
        if raw not in listed:
            errors.add(f"artifacts: unlisted file {raw}")
            continue
        try:
            size, digest = _sha256_file(
                path,
                MAX_ARTIFACT_TOTAL_BYTES - aggregate,
            )
        except ArtifactLimitError as exc:
            errors.add(f"artifacts: cannot hash {raw}: {exc}")
            break
        except (OSError, ValueError) as exc:
            errors.add(f"artifacts: cannot hash {raw}: {exc}")
            continue
        aggregate += size
        entry = listed[raw]
        if size != entry.get("size_bytes"):
            errors.add(f"artifacts: size mismatch for {raw}")
        if digest != entry.get("sha256"):
            errors.add(f"artifacts: digest mismatch for {raw}")
    missing = sorted(set(listed) - present)
    for raw in missing:
        errors.add(f"artifacts: listed file is missing: {raw}")
    errors.require(aggregate <= MAX_ARTIFACT_TOTAL_BYTES, f"artifacts: aggregate bytes exceed {MAX_ARTIFACT_TOTAL_BYTES}")
    return tuple(dict(entry) for entry in manifest)


def validate_set(
    plan_path: pathlib.Path,
    record_path: pathlib.Path,
    review_path: pathlib.Path,
    artifact_dir: pathlib.Path,
) -> ValidatedSet:
    plan = load_document(plan_path, "campaign_plan")
    record = load_document(record_path, "qualification_record")
    review = load_document(review_path, "promotion_review")
    errors = ErrorCollector()
    validate_plan(plan.value, errors)
    validate_record(record.value, plan.value, plan.sha256, errors)
    validate_review(review.value, plan.value, record.value, plan.sha256, record.sha256, errors)
    manifest = record.value.get("artifact_manifest")
    artifact_digests = verify_artifacts(artifact_dir, manifest if isinstance(manifest, list) else [], errors)
    if plan.value.get("scope") == "combined" and plan.value.get("evidence_class") != "synthetic_fixture":
        errors.add("combined qualification is blocked until the M17-05 native path prerequisite is repaired")
    errors.finish()
    return ValidatedSet(
        plan,
        record,
        review,
        str(record.value["evidence_manifest_sha256"]),
        artifact_digests,
    )


def build_proposal(validated: ValidatedSet) -> dict[str, Any]:
    plan = validated.plan.value
    record = validated.record.value
    review = validated.review.value
    evidence_class = str(plan["evidence_class"])
    return {
        "schema_version": SCHEMA_VERSION,
        "document_type": "promotion_proposal",
        "evidence_class": evidence_class,
        "claim_scope": plan["scope"],
        "tuple_id": plan["tuple_id"],
        "proposal_label": "proposal_only",
        "promotion_action": "human_matrix_change_required",
        "support_matrix_eligible": evidence_class == "qualification_campaign",
        "reviewer_authentication": "attribution_only",
        "chronology_proof": "external_human_verification_only",
        "source": plan["source"],
        "workload": plan["workload"],
        "digests": {
            "plan_sha256": validated.plan.sha256,
            "record_sha256": validated.record.sha256,
            "review_sha256": validated.review.sha256,
            "evidence_manifest_sha256": validated.artifact_manifest_sha256,
            "artifacts": list(validated.artifact_digests),
        },
        "decision": review["decision"],
        "overall_result": record["overall_result"],
    }


def _same_file(left: pathlib.Path, right: pathlib.Path) -> bool:
    try:
        return left.resolve(strict=True) == right.resolve(strict=True)
    except OSError:
        return left.absolute() == right.absolute()


def write_new_atomic(path: pathlib.Path, data: bytes, forbidden: Iterable[pathlib.Path]) -> None:
    if not path.name or path.name in (".", ".."):
        raise ValueError("output path must name a file")
    for source in forbidden:
        if _same_file(path, source):
            raise ValueError("output must not alias an input or artifact root")
    if path.exists() or path.is_symlink():
        raise FileExistsError("output path already exists")
    parent = path.parent
    if parent.is_symlink() or not parent.is_dir():
        raise ValueError("output parent must be an existing non-symlink directory")
    temporary_name: str | None = None
    published = False
    try:
        descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=parent)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary_name, path)
        published = True
        if os.name != "nt":
            directory_descriptor = os.open(parent, os.O_RDONLY)
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
    except BaseException:
        if published:
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        raise
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass


def validate_proposal_shape(value: dict[str, Any], errors: ErrorCollector) -> None:
    obj = _closed_object(
        value,
        "proposal",
        {"schema_version", "document_type", "evidence_class", "claim_scope", "tuple_id", "proposal_label", "promotion_action", "support_matrix_eligible", "reviewer_authentication", "chronology_proof", "source", "workload", "digests", "decision", "overall_result"},
        set(),
        errors,
    )
    errors.require(obj.get("evidence_class") in EVIDENCE_CLASSES, "proposal.evidence_class: invalid value")
    errors.require(obj.get("claim_scope") in SCOPES, "proposal.claim_scope: invalid value")
    _identifier(obj.get("tuple_id"), "proposal.tuple_id", errors)
    errors.require(obj.get("proposal_label") == "proposal_only", "proposal.proposal_label mismatch")
    errors.require(obj.get("promotion_action") == "human_matrix_change_required", "proposal.promotion_action mismatch")
    _boolean(obj.get("support_matrix_eligible"), "proposal.support_matrix_eligible", errors)
    errors.require(obj.get("reviewer_authentication") == "attribution_only", "proposal.reviewer_authentication mismatch")
    errors.require(obj.get("chronology_proof") == "external_human_verification_only", "proposal.chronology_proof mismatch")
    _validate_source(obj.get("source"), "proposal.source", errors)
    _validate_workload(obj.get("workload"), "proposal.workload", errors)
    digests = _closed_object(obj.get("digests"), "proposal.digests", {"plan_sha256", "record_sha256", "review_sha256", "evidence_manifest_sha256", "artifacts"}, set(), errors)
    for name in ("plan_sha256", "record_sha256", "review_sha256", "evidence_manifest_sha256"):
        _digest(digests.get(name), f"proposal.digests.{name}", errors)
    _validate_manifest_shape(digests.get("artifacts"), "proposal.digests.artifacts", errors)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("validate", "propose"):
        command = commands.add_parser(name)
        command.add_argument("--plan", type=pathlib.Path, required=True)
        command.add_argument("--record", type=pathlib.Path, required=True)
        command.add_argument("--review", type=pathlib.Path, required=True)
        command.add_argument("--artifact-dir", type=pathlib.Path, required=True)
        if name == "propose":
            command.add_argument("--output", type=pathlib.Path, required=True)
    verify = commands.add_parser("verify-proposal")
    verify.add_argument("--proposal", type=pathlib.Path, required=True)
    verify.add_argument("--plan", type=pathlib.Path, required=True)
    verify.add_argument("--record", type=pathlib.Path, required=True)
    verify.add_argument("--review", type=pathlib.Path, required=True)
    verify.add_argument("--artifact-dir", type=pathlib.Path, default=pathlib.Path("tests/qualification_fixtures/artifacts"))
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        validated = validate_set(args.plan, args.record, args.review, args.artifact_dir)
        proposal = build_proposal(validated)
        canonical = _canonical_bytes(proposal)
        if args.command == "validate":
            print(f"Qualification set OK ({validated.plan.value['scope']}, {validated.plan.value['evidence_class']})")
            return 0
        if args.command == "propose":
            artifact_root = args.artifact_dir.resolve()
            try:
                args.output.resolve().relative_to(artifact_root)
            except ValueError:
                pass
            else:
                raise ValueError("output must not be inside the artifact root")
            write_new_atomic(
                args.output,
                canonical,
                (args.plan, args.record, args.review, args.artifact_dir),
            )
            print("Promotion proposal created (proposal_only; human_matrix_change_required)")
            return 0

        loaded = load_document(args.proposal, "promotion_proposal")
        proposal_errors = ErrorCollector()
        validate_proposal_shape(loaded.value, proposal_errors)
        proposal_errors.require(loaded.raw == canonical, "proposal: bytes are not the canonical proposal for the supplied inputs")
        proposal_errors.finish()
        print("Promotion proposal OK (proposal_only; human_matrix_change_required)")
        return 0
    except (OSError, ValidationFailure, ValueError) as exc:
        print("Qualification failed:", file=sys.stderr)
        if isinstance(exc, ValidationFailure):
            for message in exc.errors:
                print(f"  - {message}", file=sys.stderr)
        else:
            print(f"  - {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
