#!/usr/bin/env python3
"""Verify immutable inputs and create/verify deterministic SPDX 2.3 SBOMs."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any


MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_FILES = 256
MAX_TOTAL_BYTES = 8 * 1024 * 1024 * 1024


class DuplicateKeyError(ValueError):
    pass


def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, value in pairs:
        if key in output:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        output[key] = value
    return output


def strict_json(path: pathlib.Path, maximum: int = MAX_JSON_BYTES) -> Any:
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
    if path.stat().st_size > MAX_TOTAL_BYTES:
        raise ValueError(f"file exceeds byte limit: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_commit(value: str) -> str:
    normalized = value.lower()
    if re.fullmatch(r"[0-9a-f]{40}", normalized) is None:
        raise ValueError("source commit must be a complete 40-character Git SHA")
    return normalized


def load_dependency_policy(path: pathlib.Path) -> dict[str, Any]:
    value = strict_json(path)
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError("dependency policy schema_version must be 1")
    dependencies = value.get("dependencies")
    actions = value.get("workflow_actions")
    if not isinstance(dependencies, dict) or not isinstance(actions, dict):
        raise ValueError("dependency policy lacks dependencies/workflow_actions")
    return value


def load_assurance_policy(path: pathlib.Path) -> dict[str, Any]:
    value = strict_json(path)
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError("portable assurance policy schema_version must be 1")
    return value


def git_output(arguments: list[str], cwd: pathlib.Path) -> str:
    try:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=cwd,
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ValueError(f"git {' '.join(arguments)} failed: {exc}") from exc
    return completed.stdout.strip()


def verify_dependencies(
    root: pathlib.Path,
    policy_path: pathlib.Path,
) -> dict[str, Any]:
    policy = load_dependency_policy(policy_path)
    dependencies = policy["dependencies"]
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    modules = (root / ".gitmodules").read_text(encoding="utf-8")
    vcpkg = strict_json(root / "vcpkg.json")
    lock = strict_json(root / "vcpkg-lock.json")

    googletest = dependencies["googletest"]
    gtest_path = root / googletest["submodule_path"]
    if googletest["repository"] not in modules:
        raise ValueError("GoogleTest submodule URL does not match policy")
    if not (gtest_path / ".git").exists():
        raise ValueError("GoogleTest submodule is missing or uninitialized")
    if git_output(["rev-parse", "HEAD"], gtest_path) != googletest["submodule_commit"]:
        raise ValueError("GoogleTest submodule commit does not match policy")
    if git_output(["status", "--porcelain", "--untracked-files=all"], gtest_path):
        raise ValueError("GoogleTest submodule is dirty or contains extra files")
    for token in (
        f"URL {googletest['fallback_url']}",
        f"URL_HASH SHA256={googletest['fallback_sha256']}",
    ):
        if token not in cmake:
            raise ValueError(f"GoogleTest fallback pin is missing: {token}")

    rapidcheck = dependencies["rapidcheck"]
    for token in (
        f"GIT_REPOSITORY {rapidcheck['repository']}",
        f"GIT_TAG {rapidcheck['commit']}",
    ):
        if token not in cmake:
            raise ValueError(f"RapidCheck immutable pin is missing: {token}")
    fetch_names = re.findall(r"FetchContent_Declare\(\s*([A-Za-z0-9_.+-]+)", cmake)
    if sorted(fetch_names) != ["googletest", "rapidcheck"]:
        raise ValueError(f"unlisted FetchContent dependencies: {fetch_names}")

    vcpkg_policy = dependencies["vcpkg"]
    if vcpkg.get("builtin-baseline") != vcpkg_policy["builtin_baseline"]:
        raise ValueError("vcpkg baseline does not match policy")
    if vcpkg.get("dependencies") != []:
        raise ValueError("vcpkg manifest contains unlisted dependencies")
    if lock != {"version": 1, "packages": {}}:
        raise ValueError("vcpkg lock contains unlisted packages")

    expected_actions = policy["workflow_actions"]
    found_actions: dict[str, str] = {}
    workflow_paths = sorted((root / ".github/workflows").glob("*.y*ml"))
    if not workflow_paths:
        raise ValueError("no workflows found")
    for path in workflow_paths:
        text = path.read_text(encoding="utf-8")
        for reference in re.findall(
            r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", text, re.MULTILINE
        ):
            if reference.startswith("./"):
                continue
            if reference.count("@") != 1:
                raise ValueError(f"invalid action reference in {path}: {reference}")
            action, revision = reference.rsplit("@", 1)
            if expected_actions.get(action) != revision:
                raise ValueError(f"unreviewed action reference: {reference}")
            if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
                raise ValueError(f"action is not pinned to a full commit: {reference}")
            previous = found_actions.setdefault(action, revision)
            if previous != revision:
                raise ValueError(f"action has inconsistent revisions: {action}")
    if set(found_actions) != set(expected_actions):
        raise ValueError(
            "workflow action inventory differs from policy: "
            f"missing={sorted(set(expected_actions) - set(found_actions))}, "
            f"extra={sorted(set(found_actions) - set(expected_actions))}"
        )
    ci = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    if "continue-on-error: true" in ci:
        raise ValueError("CI contains a success-for-failure fallback")
    if "actions/dependency-review-action@" not in ci or "fail-on-severity: high" not in ci:
        raise ValueError("blocking high-severity dependency review is missing")

    return {
        "schema_version": 1,
        "identity_only": True,
        "vulnerability_clearance": False,
        "dependencies": [
            {
                "name": name,
                "identity": record.get("submodule_commit")
                or record.get("commit")
                or record.get("builtin_baseline"),
                "license": record.get("license"),
                "purpose": record.get("purpose"),
            }
            for name, record in sorted(dependencies.items())
        ],
        "workflow_actions": [
            {"name": name, "commit": revision}
            for name, revision in sorted(expected_actions.items())
        ],
    }


def candidate_files(
    artifact_dir: pathlib.Path,
    policy: dict[str, Any],
) -> tuple[pathlib.Path, pathlib.Path]:
    artifact = policy["artifacts"]
    archive_suffix = artifact["archive_suffix"]
    checksum_suffix = artifact["checksum_suffix"]
    paths = list(artifact_dir.iterdir())
    if len(paths) > int(artifact["maximum_files"]):
        raise ValueError("artifact directory exceeds file-count policy")
    total = 0
    for path in paths:
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"artifact inventory entry is not regular: {path.name}")
        total += path.stat().st_size
    if total > int(artifact["maximum_total_bytes"]):
        raise ValueError("artifact directory exceeds byte policy")
    allowed_named = {
        artifact["sbom"],
        artifact["provenance"],
        artifact["manifest"],
    }
    archives = [path for path in paths if path.name.endswith(archive_suffix)]
    checksums = [path for path in paths if path.name.endswith(checksum_suffix)]
    unexpected = [
        path.name
        for path in paths
        if path not in archives and path not in checksums and path.name not in allowed_named
    ]
    if unexpected:
        raise ValueError(f"artifact directory contains unlisted files: {sorted(unexpected)}")
    if len(archives) != 1 or len(checksums) != 1:
        raise ValueError("expected exactly one package archive and checksum")
    if checksums[0].name != archives[0].name + ".sha256":
        raise ValueError("checksum sidecar does not match the archive name")
    words = checksums[0].read_text(encoding="utf-8").strip().split()
    if not words or words[0].lower() != sha256(archives[0]):
        raise ValueError("checksum sidecar does not match archive bytes")
    return archives[0], checksums[0]


def spdx_id(name: str) -> str:
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.-]", "-", name)


def create_sbom(
    artifact_dir: pathlib.Path,
    policy: dict[str, Any],
    dependency_policy: dict[str, Any],
    source_commit: str,
) -> dict[str, Any]:
    commit = checked_commit(source_commit)
    archive, checksum = candidate_files(artifact_dir, policy)
    version = policy["version"]
    root_id = "SPDXRef-Package-RTFW"
    files = []
    for path in sorted((archive, checksum), key=lambda value: value.name):
        files.append(
            {
                "SPDXID": spdx_id("File-" + path.name),
                "fileName": "./" + path.name,
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": sha256(path)}
                ],
                "fileTypes": ["ARCHIVE" if path == archive else "OTHER"],
            }
        )
    identity = hashlib.sha256(
        canonical_json({"commit": commit, "files": files})
    ).hexdigest()
    packages = [
        {
            "SPDXID": root_id,
            "name": "rtfw",
            "versionInfo": version,
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "Apache-2.0",
            "licenseDeclared": "Apache-2.0",
            "copyrightText": "NOASSERTION",
            "externalRefs": [
                {
                    "referenceCategory": "OTHER",
                    "referenceType": "vcs",
                    "referenceLocator": f"git+{policy['source_repository']}@{commit}",
                }
            ],
        }
    ]
    relationships = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": root_id,
        }
    ]
    for file_record in files:
        relationships.append(
            {
                "spdxElementId": root_id,
                "relationshipType": "GENERATES",
                "relatedSpdxElement": file_record["SPDXID"],
            }
        )
    for name, record in sorted(dependency_policy["dependencies"].items()):
        package_id = spdx_id("Dependency-" + name)
        identity_value = (
            record.get("submodule_commit")
            or record.get("commit")
            or record.get("builtin_baseline")
        )
        packages.append(
            {
                "SPDXID": package_id,
                "name": name,
                "versionInfo": identity_value,
                "downloadLocation": record.get("repository", "NOASSERTION"),
                "filesAnalyzed": False,
                "licenseConcluded": record["license"],
                "licenseDeclared": record["license"],
                "copyrightText": "NOASSERTION",
                "comment": record["purpose"],
            }
        )
        relationship = (
            "TEST_DEPENDENCY_OF"
            if "test" in record["purpose"]
            else "BUILD_DEPENDENCY_OF"
        )
        relationships.append(
            {
                "spdxElementId": package_id,
                "relationshipType": relationship,
                "relatedSpdxElement": root_id,
            }
        )
    return {
        "SPDXID": "SPDXRef-DOCUMENT",
        "spdxVersion": policy["spdx"]["version"],
        "dataLicense": policy["spdx"]["data_license"],
        "name": f"rtfw-{version}-portable-candidate",
        "documentNamespace": f"https://rtfw.dev/spdx/rtfw/{version}/{commit}/{identity}",
        "creationInfo": {
            "created": policy["spdx"]["creation_time"],
            "creators": ["Tool: rtfw-sbom-v1"],
            "licenseListVersion": "3.25",
        },
        "documentDescribes": [root_id],
        "packages": packages,
        "files": files,
        "relationships": relationships,
    }


def validate_spdx_shape(value: Any) -> None:
    if not isinstance(value, dict):
        raise ValueError("SBOM top-level value must be an object")
    required = {
        "SPDXID", "spdxVersion", "dataLicense", "name", "documentNamespace",
        "creationInfo", "documentDescribes", "packages", "files", "relationships",
    }
    if set(value) != required:
        raise ValueError("SBOM top-level inventory differs from policy")
    if value["SPDXID"] != "SPDXRef-DOCUMENT" or value["spdxVersion"] != "SPDX-2.3":
        raise ValueError("SBOM is not an SPDX 2.3 document")
    if not isinstance(value["packages"], list) or not value["packages"]:
        raise ValueError("SBOM packages must be non-empty")
    if not isinstance(value["files"], list) or len(value["files"]) != 2:
        raise ValueError("SBOM must cover the archive and checksum files")
    identifiers = [
        record.get("SPDXID")
        for collection in (value["packages"], value["files"])
        for record in collection
        if isinstance(record, dict)
    ]
    if len(identifiers) != len(set(identifiers)) or any(
        not isinstance(item, str) or not item.startswith("SPDXRef-")
        for item in identifiers
    ):
        raise ValueError("SBOM contains duplicate or invalid SPDX identifiers")
    if any(
        pathlib.PurePosixPath(record.get("fileName", "")).is_absolute()
        or ".." in pathlib.PurePosixPath(record.get("fileName", "")).parts
        for record in value["files"]
    ):
        raise ValueError("SBOM contains an unsafe file path")


def verify_schema_identity(root: pathlib.Path, policy: dict[str, Any]) -> None:
    schema = root / policy["spdx"]["schema_path"]
    if sha256(schema) != policy["spdx"]["schema_sha256"]:
        raise ValueError("retained official SPDX schema digest mismatch")
    schema_value = strict_json(schema)
    if not isinstance(schema_value, dict) or "SPDX" not in str(schema_value.get("title", "")):
        raise ValueError("retained SPDX schema identity is malformed")


def write_new(path: pathlib.Path, value: Any) -> None:
    if path.exists() or path.is_symlink():
        raise ValueError(f"output already exists: {path}")
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_bytes(canonical_json(value) + b"\n")
        temporary.replace(path)
    except Exception:
        if temporary.exists() and not temporary.is_symlink():
            temporary.unlink()
        raise


def verify_sbom(
    root: pathlib.Path,
    sbom_path: pathlib.Path,
    artifact_dir: pathlib.Path,
    policy: dict[str, Any],
    dependency_policy: dict[str, Any],
    source_commit: str,
) -> dict[str, Any]:
    verify_schema_identity(root, policy)
    value = strict_json(sbom_path)
    validate_spdx_shape(value)
    if canonical_json(value) + b"\n" != sbom_path.read_bytes():
        raise ValueError("SBOM is not canonical deterministic JSON")
    expected = create_sbom(
        artifact_dir, policy, dependency_policy, source_commit
    )
    if value != expected:
        raise ValueError("SBOM does not match exact package/source/dependency inputs")
    return {
        "schema_version": 1,
        "spdx_version": "SPDX-2.3",
        "schema_sha256": policy["spdx"]["schema_sha256"],
        "sbom_sha256": sha256(sbom_path),
        "deterministic": True,
        "test_dependencies_shipped": False,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    dependencies = subparsers.add_parser("verify-dependencies")
    dependencies.add_argument("--policy", type=pathlib.Path, required=True)
    dependencies.add_argument(
        "--repo-root", type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1]
    )
    dependencies.add_argument("--output", type=pathlib.Path)
    for name in ("create", "verify"):
        command = subparsers.add_parser(name)
        command.add_argument("--artifact-dir", type=pathlib.Path, required=True)
        command.add_argument("--policy", type=pathlib.Path, required=True)
        command.add_argument("--dependency-policy", type=pathlib.Path, required=True)
        command.add_argument("--source-commit", required=True)
        command.add_argument("--repo-root", type=pathlib.Path, required=True)
        if name == "create":
            command.add_argument("--output", type=pathlib.Path, required=True)
        else:
            command.add_argument("--sbom", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "verify-dependencies":
            result = verify_dependencies(
                args.repo_root.resolve(strict=True),
                args.policy.resolve(strict=True),
            )
            if args.output:
                write_new(args.output.resolve(strict=False), result)
        else:
            root = args.repo_root.resolve(strict=True)
            policy = load_assurance_policy(args.policy.resolve(strict=True))
            dependency_policy = load_dependency_policy(
                args.dependency_policy.resolve(strict=True)
            )
            artifact_dir = args.artifact_dir.resolve(strict=True)
            verify_schema_identity(root, policy)
            if args.command == "create":
                value = create_sbom(
                    artifact_dir, policy, dependency_policy, args.source_commit
                )
                validate_spdx_shape(value)
                write_new(args.output.resolve(strict=False), value)
                result = {"created": True, "spdx_version": "SPDX-2.3"}
            else:
                result = verify_sbom(
                    root,
                    args.sbom.resolve(strict=True),
                    artifact_dir,
                    policy,
                    dependency_policy,
                    args.source_commit,
                )
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, UnicodeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Portable assurance failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
