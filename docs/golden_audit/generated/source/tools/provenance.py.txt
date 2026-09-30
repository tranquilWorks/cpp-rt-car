#!/usr/bin/env python3
"""Create unsigned candidate provenance and verify offline DSSE fixtures."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_ARTIFACT_BYTES = 8 * 1024 * 1024 * 1024
SHA256_DIGEST_INFO_PREFIX = bytes.fromhex(
    "3031300d060960864801650304020105000420"
)


class DuplicateKeyError(ValueError):
    pass


def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, value in pairs:
        if key in output:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        output[key] = value
    return output


def strict_json(path: pathlib.Path, *, maximum: int = MAX_JSON_BYTES) -> Any:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"not a regular file: {path}")
    if path.stat().st_size > maximum:
        raise ValueError(f"JSON input exceeds {maximum} bytes: {path}")
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot parse {path}: {exc}") from exc


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


def sha256(path: pathlib.Path) -> str:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"not a regular file: {path}")
    if path.stat().st_size > MAX_ARTIFACT_BYTES:
        raise ValueError(f"artifact exceeds byte limit: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validated_commit(value: str) -> str:
    normalized = value.lower()
    if re.fullmatch(r"[0-9a-f]{40}", normalized) is None:
        raise ValueError("source commit must be a complete 40-character Git SHA")
    return normalized


def validated_tree(value: str) -> str:
    normalized = value.lower()
    if re.fullmatch(r"[0-9a-f]{40,64}", normalized) is None:
        raise ValueError("source tree identity must be a complete Git object ID")
    return normalized


def load_policy(path: pathlib.Path) -> dict[str, Any]:
    value = strict_json(path)
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError("portable assurance policy schema_version must be 1")
    for key in (
        "project",
        "version",
        "source_repository",
        "predicate_type",
        "builder_policy",
        "build",
        "artifacts",
        "signed_fixture",
    ):
        if key not in value:
            raise ValueError(f"portable assurance policy lacks {key}")
    return value


def dependency_records(path: pathlib.Path) -> list[dict[str, Any]]:
    value = strict_json(path)
    dependencies = value.get("dependencies") if isinstance(value, dict) else None
    if not isinstance(dependencies, dict) or not dependencies:
        raise ValueError("dependency policy must contain dependencies")
    records = []
    for name, record in sorted(dependencies.items()):
        if not isinstance(record, dict):
            raise ValueError(f"dependency {name} must be an object")
        identity = (
            record.get("submodule_commit")
            or record.get("commit")
            or record.get("builtin_baseline")
        )
        if not isinstance(identity, str) or re.fullmatch(
            r"[0-9a-f]{40}", identity
        ) is None:
            raise ValueError(f"dependency {name} lacks a complete identity")
        records.append(
            {
                "name": name,
                "identity": identity,
                "purpose": record.get("purpose"),
                "license": record.get("license"),
            }
        )
    return records


def artifact_subjects(
    artifact_dir: pathlib.Path,
    policy: dict[str, Any],
) -> list[dict[str, Any]]:
    artifact_policy = policy["artifacts"]
    archive_suffix = artifact_policy["archive_suffix"]
    checksum_suffix = artifact_policy["checksum_suffix"]
    sbom_name = artifact_policy["sbom"]
    candidates = [
        path
        for path in artifact_dir.iterdir()
        if path.is_file() and not path.is_symlink()
    ]
    archives = [path for path in candidates if path.name.endswith(archive_suffix)]
    checksums = [path for path in candidates if path.name.endswith(checksum_suffix)]
    sbom = artifact_dir / sbom_name
    if len(archives) != 1 or len(checksums) != 1:
        raise ValueError("expected exactly one package archive and checksum")
    if checksums[0].name != archives[0].name + ".sha256":
        raise ValueError("checksum does not name the package archive")
    if sbom.is_symlink() or not sbom.is_file():
        raise ValueError("candidate SBOM is missing or not regular")
    return [
        {"name": path.name, "digest": {"sha256": sha256(path)}}
        for path in sorted((archives[0], checksums[0], sbom), key=lambda p: p.name)
    ]


def create_statement(
    artifact_dir: pathlib.Path,
    policy: dict[str, Any],
    dependency_policy: pathlib.Path,
    source_commit: str,
    source_tree: str,
    source_tree_state: str,
    compiler: str,
    cmake: str,
) -> dict[str, Any]:
    if source_tree_state not in {"clean", "dirty"}:
        raise ValueError("source tree state must be clean or dirty")
    return {
        "_type": "https://in-toto.io/Statement/v1",
        "subject": artifact_subjects(artifact_dir, policy),
        "predicateType": policy["predicate_type"],
        "predicate": {
            "authentication": False,
            "verificationOnly": True,
            "project": policy["project"],
            "version": policy["version"],
            "source": {
                "repository": policy["source_repository"],
                "commit": validated_commit(source_commit),
                "tree": validated_tree(source_tree),
                "treeState": source_tree_state,
            },
            "builder": {
                "policy": policy["builder_policy"],
                "compiler": compiler,
                "cmake": cmake,
            },
            "parameters": policy["build"],
            "dependencies": dependency_records(dependency_policy),
        },
    }


def write_new(path: pathlib.Path, value: Any) -> None:
    if path.exists() or path.is_symlink():
        raise ValueError(f"output already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_bytes(canonical_json(value) + b"\n")
        temporary.replace(path)
    except Exception:
        if temporary.exists() and not temporary.is_symlink():
            temporary.unlink()
        raise


def manifest_entries(path: pathlib.Path) -> dict[str, dict[str, Any]]:
    value = strict_json(path)
    entries = value.get("artifacts") if isinstance(value, dict) else None
    if not isinstance(entries, list):
        raise ValueError("release manifest lacks artifacts")
    output: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            raise ValueError("release manifest contains malformed artifact")
        name = entry["path"]
        if name in output:
            raise ValueError("release manifest contains a duplicate artifact")
        output[name] = entry
    return output


def verify_statement(
    statement_path: pathlib.Path,
    artifact_dir: pathlib.Path,
    manifest_path: pathlib.Path,
    policy: dict[str, Any],
    dependency_policy: pathlib.Path,
    source_commit: str,
    source_tree: str,
    source_tree_state: str,
    compiler: str,
    cmake: str,
) -> dict[str, Any]:
    statement = strict_json(statement_path)
    if canonical_json(statement) + b"\n" != statement_path.read_bytes():
        raise ValueError("provenance statement is not canonical JSON")
    expected = create_statement(
        artifact_dir,
        policy,
        dependency_policy,
        source_commit,
        source_tree,
        source_tree_state,
        compiler,
        cmake,
    )
    if statement != expected:
        raise ValueError("provenance statement does not match exact policy/input bytes")
    entries = manifest_entries(manifest_path)
    record = entries.get(statement_path.name)
    if record is None:
        raise ValueError("final manifest does not cover the provenance statement")
    if record.get("sha256") != sha256(statement_path):
        raise ValueError("manifest provenance digest mismatch")
    if record.get("size_bytes") != statement_path.stat().st_size:
        raise ValueError("manifest provenance size mismatch")
    return {
        "schema_version": 1,
        "content_verified": True,
        "authentication": False,
        "signed_target": False,
        "source_commit": validated_commit(source_commit),
    }


def dsse_pae(payload_type: bytes, payload: bytes) -> bytes:
    return (
        b"DSSEv1 "
        + str(len(payload_type)).encode("ascii")
        + b" "
        + payload_type
        + b" "
        + str(len(payload)).encode("ascii")
        + b" "
        + payload
    )


def rsa_pkcs1v15_sha256_verify(
    message: bytes,
    signature: bytes,
    modulus: int,
    exponent: int,
) -> bool:
    if modulus <= 0 or exponent <= 1:
        return False
    size = (modulus.bit_length() + 7) // 8
    if len(signature) != size:
        return False
    encoded = pow(int.from_bytes(signature, "big"), exponent, modulus).to_bytes(
        size, "big"
    )
    digest_info = SHA256_DIGEST_INFO_PREFIX + hashlib.sha256(message).digest()
    padding_size = size - len(digest_info) - 3
    if padding_size < 8:
        return False
    expected = b"\x00\x01" + b"\xff" * padding_size + b"\x00" + digest_info
    return encoded == expected


def decode_base64(value: Any, label: str) -> bytes:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be base64 text")
    try:
        return base64.b64decode(value, validate=True)
    except ValueError as exc:
        raise ValueError(f"{label} is invalid base64") from exc


def verify_fixture(root: pathlib.Path, policy: dict[str, Any]) -> dict[str, Any]:
    expected = policy["signed_fixture"]
    artifact = root / expected["artifact"]
    envelope_path = root / expected["envelope"]
    trust_path = root / expected["trust"]
    envelope = strict_json(envelope_path)
    trust = strict_json(trust_path)
    if not isinstance(envelope, dict) or set(envelope) != {
        "payloadType", "payload", "signatures"
    }:
        raise ValueError("DSSE envelope has an unexpected shape")
    if envelope["payloadType"] != expected["payload_type"]:
        raise ValueError("DSSE payload type mismatch")
    signatures = envelope["signatures"]
    if not isinstance(signatures, list) or len(signatures) != 1:
        raise ValueError("DSSE fixture must contain exactly one signature")
    signature_record = signatures[0]
    if not isinstance(signature_record, dict) or set(signature_record) != {
        "keyid", "sig"
    }:
        raise ValueError("DSSE signature record is malformed")
    if signature_record["keyid"] != expected["key_id"]:
        raise ValueError("DSSE key identity mismatch")
    if not isinstance(trust, dict) or trust.get("key_id") != expected["key_id"]:
        raise ValueError("public trust identity mismatch")
    if trust.get("algorithm") != "RSASSA-PKCS1-v1_5-SHA256":
        raise ValueError("unsupported public trust algorithm")
    try:
        modulus = int(trust["modulus_hex"], 16)
        exponent = int(trust["exponent"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("public trust material is malformed") from exc
    payload = decode_base64(envelope["payload"], "DSSE payload")
    signature = decode_base64(signature_record["sig"], "DSSE signature")
    payload_type = envelope["payloadType"].encode("utf-8")
    if not rsa_pkcs1v15_sha256_verify(
        dsse_pae(payload_type, payload), signature, modulus, exponent
    ):
        raise ValueError("DSSE cryptographic signature verification failed")
    try:
        statement = json.loads(
            payload.decode("utf-8"), object_pairs_hook=reject_duplicates
        )
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"DSSE payload is not strict JSON: {exc}") from exc
    if canonical_json(statement) != payload:
        raise ValueError("DSSE payload is not canonical JSON")
    if statement.get("_type") != "https://in-toto.io/Statement/v1":
        raise ValueError("fixture is not an in-toto Statement v1")
    if statement.get("predicateType") != expected["predicate_type"]:
        raise ValueError("fixture predicate type mismatch")
    subjects = statement.get("subject")
    if not isinstance(subjects, list) or len(subjects) != 1:
        raise ValueError("fixture must have exactly one subject")
    if subjects[0] != {
        "name": artifact.name,
        "digest": {"sha256": sha256(artifact)},
    }:
        raise ValueError("fixture artifact subject mismatch")
    if sha256(artifact) != expected["artifact_sha256"]:
        raise ValueError("fixture artifact policy digest mismatch")
    try:
        parameters = statement["predicate"]["buildDefinition"]["externalParameters"]
    except (KeyError, TypeError) as exc:
        raise ValueError("fixture predicate identity is missing") from exc
    for key in ("repository", "source_digest", "ref", "workflow", "issuer"):
        if parameters.get(key) != expected[key]:
            raise ValueError(f"fixture {key} identity mismatch")
    return {
        "schema_version": 1,
        "cryptographic_fixture_verified": True,
        "target_authenticated": False,
        "current_revocation_checked": False,
        "key_id": expected["key_id"],
    }


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--artifact-dir", type=pathlib.Path, required=True)
    parser.add_argument("--policy", type=pathlib.Path, required=True)
    parser.add_argument("--dependency-policy", type=pathlib.Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--source-tree-state", choices=("clean", "dirty"), required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cmake", required=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("create")
    add_common(create)
    create.add_argument("--output", type=pathlib.Path, required=True)
    verify = subparsers.add_parser("verify")
    add_common(verify)
    verify.add_argument("--statement", type=pathlib.Path, required=True)
    verify.add_argument("--manifest", type=pathlib.Path, required=True)
    fixture = subparsers.add_parser("verify-fixture")
    fixture.add_argument("--repo-root", type=pathlib.Path, required=True)
    fixture.add_argument("--policy", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)

    try:
        policy = load_policy(args.policy)
        if args.command == "verify-fixture":
            result = verify_fixture(args.repo_root.resolve(strict=True), policy)
        elif args.command == "create":
            statement = create_statement(
                args.artifact_dir.resolve(strict=True),
                policy,
                args.dependency_policy.resolve(strict=True),
                args.source_commit,
                args.source_tree,
                args.source_tree_state,
                args.compiler,
                args.cmake,
            )
            write_new(args.output.resolve(strict=False), statement)
            result = {"created": True, "authentication": False}
        else:
            result = verify_statement(
                args.statement.resolve(strict=True),
                args.artifact_dir.resolve(strict=True),
                args.manifest.resolve(strict=True),
                policy,
                args.dependency_policy.resolve(strict=True),
                args.source_commit,
                args.source_tree,
                args.source_tree_state,
                args.compiler,
                args.cmake,
            )
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"Provenance verification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
