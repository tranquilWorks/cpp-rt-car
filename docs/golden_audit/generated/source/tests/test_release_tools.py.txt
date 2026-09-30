#!/usr/bin/env python3
"""Negative and round-trip tests for the M12 release tooling."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_module(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {relative}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


release_manifest = load_module(
    "rtfw_release_manifest",
    "tools/release_manifest.py",
)
release_contract = load_module(
    "rtfw_release_contract",
    "tools/check_release_contract.py",
)
hardware_evidence = load_module(
    "rtfw_hardware_evidence",
    "tools/check_hardware_evidence.py",
)
release_staging = load_module(
    "rtfw_release_staging",
    "tools/stage_release_artifacts.py",
)
release_extraction = load_module(
    "rtfw_release_extraction",
    "tools/extract_release_archive.py",
)
qualification = load_module(
    "rtfw_qualification",
    "tools/qualification.py",
)
portable_sbom = load_module(
    "rtfw_portable_sbom",
    "tools/sbom.py",
)
portable_provenance = load_module(
    "rtfw_portable_provenance",
    "tools/provenance.py",
)
portable_static = load_module(
    "rtfw_portable_static",
    "tools/check_static_analysis.py",
)
portable_fuzz = load_module(
    "rtfw_portable_fuzz",
    "tools/run_fuzz_smoke.py",
)


class ReleaseManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        (self.artifacts / "rtfw-linux.tar.gz").write_bytes(b"linux")
        nested = self.artifacts / "windows"
        nested.mkdir()
        (nested / "rtfw-windows.zip").write_bytes(b"windows")
        self.version = self.root / "VERSION.txt"
        self.version.write_text("1.0.0\n", encoding="utf-8")
        self.manifest = self.artifacts / "rtfw-release-manifest.json"
        self.commit = "0123456789abcdef0123456789abcdef01234567"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def create(self) -> None:
        value = release_manifest.create_manifest(
            self.artifacts,
            self.manifest,
            self.version,
            self.commit,
        )
        release_manifest.write_manifest(self.manifest, value)

    def test_round_trip_and_corruption_rejection(self) -> None:
        self.create()
        self.assertEqual(
            release_manifest.verify_manifest(
                self.manifest,
                self.artifacts,
                self.version,
                self.commit,
            ),
            [],
        )
        (self.artifacts / "rtfw-linux.tar.gz").write_bytes(b"corrupt")
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("digest mismatch" in error for error in errors))

    def test_unlisted_and_unsafe_artifacts_are_rejected(self) -> None:
        self.create()
        (self.artifacts / "unexpected.bin").write_bytes(b"unexpected")
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("unlisted" in error for error in errors))

        data = json.loads(self.manifest.read_text(encoding="utf-8"))
        data["artifacts"][0]["path"] = "../escape"
        self.manifest.write_text(
            json.dumps(data),
            encoding="utf-8",
        )
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("unsafe path" in error for error in errors))

    def test_partial_commit_and_empty_directory_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "40-character"):
            release_manifest.create_manifest(
                self.artifacts,
                self.manifest,
                self.version,
                "deadbeef",
            )
        for path in sorted(self.artifacts.rglob("*"), reverse=True):
            if path.is_file():
                path.unlink()
            else:
                path.rmdir()
        with self.assertRaisesRegex(ValueError, "no release artifacts"):
            release_manifest.create_manifest(
                self.artifacts,
                self.manifest,
                self.version,
                self.commit,
            )

    def test_missing_duplicate_order_and_nonportable_paths_are_rejected(
        self,
    ) -> None:
        self.create()
        data = json.loads(self.manifest.read_text(encoding="utf-8"))

        missing_path = self.artifacts / data["artifacts"][0]["path"]
        missing_path.unlink()
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("missing" in error for error in errors))

        missing_path.write_bytes(b"linux")
        duplicate = copy.deepcopy(data)
        duplicate["artifacts"].append(
            copy.deepcopy(duplicate["artifacts"][0])
        )
        self.manifest.write_text(
            json.dumps(duplicate),
            encoding="utf-8",
        )
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("duplicate" in error for error in errors))

        reversed_entries = copy.deepcopy(data)
        reversed_entries["artifacts"].reverse()
        self.manifest.write_text(
            json.dumps(reversed_entries),
            encoding="utf-8",
        )
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("sorted" in error for error in errors))

        self.assertIsNone(
            release_manifest.safe_relative_path(r"..\escape")
        )
        self.assertIsNone(
            release_manifest.safe_relative_path("control\u0001name")
        )
        self.assertIsNone(
            release_manifest.safe_relative_path("C:/escape")
        )

    def test_noncanonical_duplicate_json_and_overwrite_are_rejected(self) -> None:
        self.create()
        canonical = self.manifest.read_text(encoding="utf-8")

        self.manifest.write_text(canonical + "\n", encoding="utf-8")
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("canonical" in error for error in errors))

        self.manifest.write_text(
            canonical.replace(
                '"schema_version": 1,',
                '"schema_version": 1,\n  "schema_version": 1,',
                1,
            ),
            encoding="utf-8",
        )
        errors = release_manifest.verify_manifest(
            self.manifest,
            self.artifacts,
            self.version,
            self.commit,
        )
        self.assertTrue(any("duplicate JSON key" in error for error in errors))

        self.manifest.write_text(canonical, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "already exists"):
            release_manifest.write_manifest(
                self.manifest,
                release_manifest.create_manifest(
                    self.artifacts,
                    self.manifest,
                    self.version,
                    self.commit,
                ),
            )


class ReleaseStagingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.cpack = self.root / "cpack-output"
        self.cpack.mkdir()
        (self.cpack / "_CPack_Packages").mkdir()
        self.archive = (
            self.cpack /
            "rtfw-1.0.0-Linux-x86_64-GNU-11.4.0.tar.gz"
        )
        self.archive.write_bytes(b"archive")
        self.sidecar = self.archive.with_name(
            self.archive.name + ".sha256"
        )
        self.sidecar.write_text(
            f"{release_staging.sha256(self.archive)}  {self.archive.name}\n",
            encoding="utf-8",
        )
        self.version = self.root / "VERSION.txt"
        self.version.write_text("1.0.0\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_stages_only_verified_archive_and_sidecar(self) -> None:
        destination = self.root / "artifacts"
        staged = release_staging.stage(
            self.cpack,
            destination,
            "TGZ",
            self.version,
        )
        self.assertEqual(
            [path.name for path in staged],
            [self.archive.name, self.sidecar.name],
        )
        self.assertEqual(
            sorted(path.name for path in destination.iterdir()),
            sorted((self.archive.name, self.sidecar.name)),
        )

    def test_rejects_corruption_stale_output_and_unexpected_files(self) -> None:
        self.sidecar.write_text("0" * 64 + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "does not match"):
            release_staging.stage(
                self.cpack,
                self.root / "corrupt",
                "TGZ",
                self.version,
            )

        self.sidecar.write_text(
            f"{release_staging.sha256(self.archive)}\n",
            encoding="utf-8",
        )
        (self.cpack / "unexpected.txt").write_text(
            "unexpected",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "unexpected top-level"):
            release_staging.stage(
                self.cpack,
                self.root / "unexpected",
                "TGZ",
                self.version,
            )

        (self.cpack / "unexpected.txt").unlink()
        unexpected_directory = self.cpack / "unexpected-directory"
        unexpected_directory.mkdir()
        with self.assertRaisesRegex(ValueError, "unexpected top-level"):
            release_staging.stage(
                self.cpack,
                self.root / "unexpected-directory-output",
                "TGZ",
                self.version,
            )
        unexpected_directory.rmdir()

        stale = self.root / "stale"
        stale.mkdir()
        with self.assertRaisesRegex(ValueError, "must not already exist"):
            release_staging.stage(
                self.cpack,
                stale,
                "TGZ",
                self.version,
            )


class ReleaseExtractionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def add_tar_file(
        archive: tarfile.TarFile,
        name: str,
        content: bytes,
    ) -> None:
        member = tarfile.TarInfo(name)
        member.size = len(content)
        member.mode = 0o644
        archive.addfile(member, io.BytesIO(content))

    def test_extracts_flat_tgz_with_safe_library_symlink(self) -> None:
        artifacts = self.root / "tgz-artifacts"
        artifacts.mkdir()
        archive_path = artifacts / "rtfw-1.0.0-test.tar.gz"
        with tarfile.open(archive_path, mode="w:gz") as archive:
            self.add_tar_file(
                archive,
                "include/rtfw/version.h",
                b"#define RTFW_VERSION_MAJOR 1\n",
            )
            self.add_tar_file(
                archive,
                "lib/librtfw.so.8",
                b"library",
            )
            link = tarfile.TarInfo("lib/librtfw.so")
            link.type = tarfile.SYMTYPE
            link.linkname = "librtfw.so.8"
            archive.addfile(link)

        destination = self.root / "tgz-prefix"
        release_extraction.extract(artifacts, destination)
        self.assertEqual(
            (destination / "lib/librtfw.so.8").read_bytes(),
            b"library",
        )
        self.assertTrue((destination / "lib/librtfw.so").is_symlink())

    def test_extracts_flat_zip(self) -> None:
        artifacts = self.root / "zip-artifacts"
        artifacts.mkdir()
        archive_path = artifacts / "rtfw-1.0.0-test.zip"
        with zipfile.ZipFile(archive_path, mode="w") as archive:
            archive.writestr(
                "include/rtfw/version.h",
                "#define RTFW_VERSION_MAJOR 1\n",
            )
            archive.writestr(
                "lib/cmake/rtfw/rtfwConfig.cmake",
                "set(RTFW_VERSION 1.0.0)\n",
            )

        destination = self.root / "zip-prefix"
        release_extraction.extract(artifacts, destination)
        self.assertTrue(
            (destination / "lib/cmake/rtfw/rtfwConfig.cmake").is_file()
        )

    def test_rejects_traversal_and_removes_partial_destination(self) -> None:
        artifacts = self.root / "unsafe-artifacts"
        artifacts.mkdir()
        archive_path = artifacts / "rtfw-1.0.0-unsafe.tar.gz"
        with tarfile.open(archive_path, mode="w:gz") as archive:
            self.add_tar_file(archive, "../escape", b"unsafe")

        destination = self.root / "unsafe-prefix"
        with self.assertRaisesRegex(ValueError, "unsafe path"):
            release_extraction.extract(artifacts, destination)
        self.assertFalse(destination.exists())
        self.assertFalse((self.root / "escape").exists())

    def test_rejects_escaping_and_cyclic_links(self) -> None:
        unsafe_artifacts = self.root / "unsafe-link-artifacts"
        unsafe_artifacts.mkdir()
        unsafe_archive = unsafe_artifacts / "rtfw-1.0.0-unsafe-link.tar.gz"
        with tarfile.open(unsafe_archive, mode="w:gz") as archive:
            self.add_tar_file(archive, "lib/target", b"target")
            link = tarfile.TarInfo("lib/link")
            link.type = tarfile.SYMTYPE
            link.linkname = "../../escape"
            archive.addfile(link)
        with self.assertRaisesRegex(ValueError, "unsafe link"):
            release_extraction.extract(
                unsafe_artifacts,
                self.root / "unsafe-link-prefix",
            )

        cyclic_artifacts = self.root / "cyclic-link-artifacts"
        cyclic_artifacts.mkdir()
        cyclic_archive = cyclic_artifacts / "rtfw-1.0.0-cycle.tar.gz"
        with tarfile.open(cyclic_archive, mode="w:gz") as archive:
            first = tarfile.TarInfo("lib/first")
            first.type = tarfile.SYMTYPE
            first.linkname = "second"
            archive.addfile(first)
            second = tarfile.TarInfo("lib/second")
            second.type = tarfile.SYMTYPE
            second.linkname = "first"
            archive.addfile(second)
        with self.assertRaisesRegex(ValueError, "cyclic link"):
            release_extraction.extract(
                cyclic_artifacts,
                self.root / "cyclic-link-prefix",
            )


class ReleaseContractTests(unittest.TestCase):
    def fixture(self) -> pathlib.Path:
        destination = pathlib.Path(self.temporary.name) / "repo"
        required = {
            "VERSION.txt",
            "vcpkg.json",
            "CMakeLists.txt",
            "README.md",
            "CHANGELOG.md",
            "include/rtfw/version.h",
            "docs/portable_support_matrix.json",
            "docs/cuda_support_matrix.json",
            "docs/xdma_support_matrix.json",
            "docs/release_policy.md",
            "release/rtfw-release-contract.json",
            ".github/workflows/ci.yml",
            ".github/workflows/release.yml",
            ".github/workflows/cuda-qualification.yml",
            ".github/workflows/xdma-qualification.yml",
            "tools/extract_release_archive.py",
            "tools/stage_release_artifacts.py",
            "tests/package_consumer/CMakeLists.txt",
            "tests/test_device_runtime.cpp",
        } | release_contract.HASHED_CONTRACT_PATHS
        for relative in sorted(required):
            source = ROOT / relative
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        return destination

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_repository_contract_and_negative_mutations(self) -> None:
        fixture = self.fixture()
        self.assertEqual(release_contract.validate_repository(fixture), [])

        (fixture / "VERSION.txt").write_text("1.0\n", encoding="utf-8")
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("MAJOR.MINOR.PATCH" in error for error in errors))

        shutil.copy2(ROOT / "VERSION.txt", fixture / "VERSION.txt")
        matrix_path = fixture / "docs/portable_support_matrix.json"
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        matrix["supported_tuples"][0]["rt_tier"] = "RT2"
        matrix_path.write_text(json.dumps(matrix), encoding="utf-8")
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("may claim only RT0" in error for error in errors))

        shutil.copy2(
            ROOT / "docs/portable_support_matrix.json",
            matrix_path,
        )
        header = fixture / "rt/include/rt/c_api.h"
        header.write_text(
            header.read_text(encoding="utf-8") + "\n/* mutation */\n",
            encoding="utf-8",
        )
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("digest mismatch" in error for error in errors))

    def test_malformed_nested_contract_values_fail_closed(self) -> None:
        fixture = self.fixture()

        portable_path = fixture / "docs/portable_support_matrix.json"
        portable = json.loads(portable_path.read_text(encoding="utf-8"))
        portable["supported_tuples"][0]["validated_surfaces"] = [{}]
        portable_path.write_text(json.dumps(portable), encoding="utf-8")

        cuda_path = fixture / "docs/cuda_support_matrix.json"
        cuda = json.loads(cuda_path.read_text(encoding="utf-8"))
        cuda["required_tuple_fields"] = [{}]
        cuda_path.write_text(json.dumps(cuda), encoding="utf-8")

        contract_path = fixture / "release/rtfw-release-contract.json"
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
        contract["cmake_package"]["always_components"] = [{}]
        contract_path.write_text(json.dumps(contract), encoding="utf-8")

        errors = release_contract.validate_repository(fixture)
        self.assertTrue(any("lacks package surfaces" in error for error in errors))
        self.assertTrue(
            any("required_tuple_fields is incomplete" in error for error in errors)
        )
        self.assertTrue(
            any("CMake component contract mismatch" in error for error in errors)
        )

    def test_movable_or_uncontracted_workflow_is_rejected(self) -> None:
        fixture = self.fixture()
        workflow_path = fixture / ".github/workflows/ci.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        workflow_path.write_text(
            workflow.replace(
                (
                    "actions/checkout@"
                    "11d5960a326750d5838078e36cf38b85af677262"
                ),
                "actions/checkout@v4",
                1,
            ),
            encoding="utf-8",
        )
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(
            any("unreviewed action reference" in error for error in errors)
        )

        shutil.copy2(ROOT / ".github/workflows/ci.yml", workflow_path)
        rogue = fixture / ".github/workflows/uncontracted.yaml"
        rogue.write_text(
            "name: uncontracted\non: workflow_dispatch\njobs: {}\n",
            encoding="utf-8",
        )
        errors = release_contract.validate_repository(fixture)
        self.assertTrue(
            any("workflow set differs" in error for error in errors)
        )


class PortableAssuranceTests(unittest.TestCase):
    commit = "0123456789abcdef0123456789abcdef01234567"
    tree = "89abcdef0123456789abcdef0123456789abcdef"

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.policy_path = ROOT / "release/portable-assurance-policy.json"
        self.dependency_path = ROOT / "tools/sbom_expected.json"
        self.policy = portable_sbom.load_assurance_policy(self.policy_path)
        self.dependencies = portable_sbom.load_dependency_policy(
            self.dependency_path
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def candidate(self, name: str) -> pathlib.Path:
        candidate = self.root / name
        candidate.mkdir()
        archive = candidate / "rtfw-1.2.1-test.tar.gz"
        archive.write_bytes(b"candidate archive")
        sidecar = candidate / (archive.name + ".sha256")
        sidecar.write_text(
            f"{portable_sbom.sha256(archive)}  {archive.name}\n",
            encoding="utf-8",
        )
        return candidate

    def create_sbom(self, candidate: pathlib.Path) -> pathlib.Path:
        path = candidate / "rtfw.spdx.json"
        value = portable_sbom.create_sbom(
            candidate,
            self.policy,
            self.dependencies,
            self.commit,
        )
        portable_sbom.validate_spdx_shape(value)
        portable_sbom.write_new(path, value)
        return path

    def test_portable_assurance_sbom_is_deterministic_and_fail_closed(
        self,
    ) -> None:
        first = self.candidate("first")
        second = self.candidate("second")
        first_sbom = self.create_sbom(first)
        second_sbom = self.create_sbom(second)
        self.assertEqual(first_sbom.read_bytes(), second_sbom.read_bytes())
        result = portable_sbom.verify_sbom(
            ROOT,
            first_sbom,
            first,
            self.policy,
            self.dependencies,
            self.commit,
        )
        self.assertEqual(result["spdx_version"], "SPDX-2.3")
        self.assertFalse(result["test_dependencies_shipped"])

        data = json.loads(first_sbom.read_text(encoding="utf-8"))
        data["files"][0]["fileName"] = "../escape"
        first_sbom.write_text(
            json.dumps(data, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "unsafe file path"):
            portable_sbom.verify_sbom(
                ROOT,
                first_sbom,
                first,
                self.policy,
                self.dependencies,
                self.commit,
            )

        clean = self.candidate("source-mismatch")
        clean_sbom = self.create_sbom(clean)
        with self.assertRaisesRegex(ValueError, "exact package/source"):
            portable_sbom.verify_sbom(
                ROOT,
                clean_sbom,
                clean,
                self.policy,
                self.dependencies,
                "f" * 40,
            )

        extra = self.candidate("extra")
        (extra / "unlisted.bin").write_bytes(b"unlisted")
        with self.assertRaisesRegex(ValueError, "unlisted files"):
            portable_sbom.create_sbom(
                extra,
                self.policy,
                self.dependencies,
                self.commit,
            )

        duplicate_json = self.root / "duplicate.json"
        duplicate_json.write_text('{"a":1,"a":2}\n', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            portable_sbom.strict_json(duplicate_json)

    def test_portable_assurance_unsigned_statement_and_manifest_binding(
        self,
    ) -> None:
        candidate = self.candidate("candidate")
        self.create_sbom(candidate)
        statement_path = candidate / "rtfw.provenance.json"
        statement = portable_provenance.create_statement(
            candidate,
            self.policy,
            self.dependency_path,
            self.commit,
            self.tree,
            "clean",
            "Ubuntu clang version 14.0.0",
            "cmake version 3.22.1",
        )
        portable_provenance.write_new(statement_path, statement)
        manifest_path = candidate / "rtfw-release-manifest.json"
        version_path = self.root / "VERSION.txt"
        version_path.write_text("1.2.1\n", encoding="utf-8")
        manifest = release_manifest.create_manifest(
            candidate,
            manifest_path,
            version_path,
            self.commit,
        )
        release_manifest.write_manifest(manifest_path, manifest)
        self.assertEqual(
            release_manifest.verify_manifest(
                manifest_path,
                candidate,
                version_path,
                self.commit,
            ),
            [],
        )
        result = portable_provenance.verify_statement(
            statement_path,
            candidate,
            manifest_path,
            self.policy,
            self.dependency_path,
            self.commit,
            self.tree,
            "clean",
            "Ubuntu clang version 14.0.0",
            "cmake version 3.22.1",
        )
        self.assertFalse(result["authentication"])
        self.assertFalse(result["signed_target"])
        errors = release_manifest.verify_manifest(
            manifest_path,
            candidate,
            version_path,
            "f" * 40,
        )
        self.assertTrue(any("expected source" in error for error in errors))

    def test_portable_assurance_public_fixture_and_identity_mutations(
        self,
    ) -> None:
        result = portable_provenance.verify_fixture(ROOT, self.policy)
        self.assertTrue(result["cryptographic_fixture_verified"])
        self.assertFalse(result["target_authenticated"])

        fixture_root = self.root / "fixture-root"
        fixture_path = fixture_root / "tests/provenance_fixtures/public"
        shutil.copytree(
            ROOT / "tests/provenance_fixtures/public",
            fixture_path,
        )
        artifact = fixture_path / "example-artifact.txt"
        artifact.write_bytes(artifact.read_bytes() + b"mutation")
        with self.assertRaisesRegex(ValueError, "artifact subject mismatch"):
            portable_provenance.verify_fixture(fixture_root, self.policy)

        shutil.copy2(
            ROOT / "tests/provenance_fixtures/public/example-artifact.txt",
            artifact,
        )
        envelope_path = fixture_path / "dsse-envelope.json"
        envelope = json.loads(envelope_path.read_text(encoding="utf-8"))
        envelope["signatures"][0]["sig"] = (
            "A" + envelope["signatures"][0]["sig"][1:]
        )
        envelope_path.write_text(json.dumps(envelope), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "cryptographic signature"):
            portable_provenance.verify_fixture(fixture_root, self.policy)

        for field in (
            "repository",
            "source_digest",
            "ref",
            "workflow",
            "issuer",
            "predicate_type",
        ):
            with self.subTest(field=field):
                mutated = copy.deepcopy(self.policy)
                mutated["signed_fixture"][field] = "mutated"
                with self.assertRaises(ValueError):
                    portable_provenance.verify_fixture(ROOT, mutated)

        shutil.copy2(
            ROOT / "tests/provenance_fixtures/public/dsse-envelope.json",
            envelope_path,
        )
        trust_path = fixture_path / "public-trust.json"
        trust = json.loads(trust_path.read_text(encoding="utf-8"))
        trust["modulus_hex"] = "01" + trust["modulus_hex"][2:]
        trust_path.write_text(json.dumps(trust), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "cryptographic signature"):
            portable_provenance.verify_fixture(fixture_root, self.policy)

    def test_portable_assurance_static_manifest_reconciliation(self) -> None:
        database = self.root / "compile_commands.json"
        source = ROOT / "rt/src/runtime_profile.cpp"
        entry = {
            "directory": str(self.root),
            "file": str(source),
            "command": (
                "clang++ -o CMakeFiles/rtfw_runtime.dir/"
                "rt/src/runtime_profile.cpp.o -c " + str(source)
            ),
        }
        database.write_text(json.dumps([entry]), encoding="utf-8")
        manifest = self.root / "sources.txt"
        manifest.write_text(
            "rtfw_runtime|rt/src/runtime_profile.cpp\n",
            encoding="utf-8",
        )
        self.assertEqual(
            portable_static.compile_entries(ROOT, database),
            portable_static.load_manifest(manifest),
        )
        database.write_text(json.dumps([entry, entry]), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            portable_static.compile_entries(ROOT, database)

        unsafe_manifest = self.root / "unsafe-sources.txt"
        unsafe_manifest.write_text("target|../escape.cpp\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "unsafe"):
            portable_static.load_manifest(unsafe_manifest)

        external_source = self.root / "external.cpp"
        external_source.write_text("int external;\n", encoding="utf-8")
        external_entry = copy.deepcopy(entry)
        external_entry["file"] = str(external_source)
        database.write_text(json.dumps([external_entry]), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "outside the repository"):
            portable_static.compile_entries(ROOT, database)

    def test_portable_assurance_dependency_inventory(self) -> None:
        result = portable_sbom.verify_dependencies(ROOT, self.dependency_path)
        self.assertTrue(result["identity_only"])
        self.assertFalse(result["vulnerability_clearance"])

        policy = json.loads(self.dependency_path.read_text(encoding="utf-8"))
        policy["dependencies"]["rapidcheck"]["commit"] = "master"
        mutated = self.root / "dependencies.json"
        mutated.write_text(json.dumps(policy), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "RapidCheck immutable pin"):
            portable_sbom.verify_dependencies(ROOT, mutated)

    def test_portable_fuzz_seed_manifests_fail_closed(self) -> None:
        valid = self.root / "valid-seeds.json"
        valid.write_text(
            '{"empty.bin":{"base64":""},"text.txt":{"utf8":"seed"}}\n',
            encoding="utf-8",
        )
        self.assertEqual(
            portable_fuzz.load_seeds(valid),
            [("empty.bin", b""), ("text.txt", b"seed")],
        )
        duplicate = self.root / "duplicate-seeds.json"
        duplicate.write_text(
            '{"seed":{"utf8":"a"},"seed":{"utf8":"b"}}\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            portable_fuzz.load_seeds(duplicate)
        unsafe = self.root / "unsafe-seeds.json"
        unsafe.write_text(
            '{"../escape":{"utf8":"a"}}\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "unsafe seed"):
            portable_fuzz.load_seeds(unsafe)


class QualificationToolTests(unittest.TestCase):
    SCOPES = ("nvidia", "xdma", "combined", "rt1", "rt2")

    def fixture_paths(self, scope: str) -> tuple[pathlib.Path, ...]:
        prefix = "" if scope == "nvidia" else f"{scope}-"
        return (
            ROOT / "tests/qualification_fixtures" / f"valid-{prefix}campaign-plan.json",
            ROOT / "tests/qualification_fixtures" / f"valid-{prefix}qualification-record.json",
            ROOT / "tests/qualification_fixtures" / f"valid-{prefix}promotion-review.json",
            ROOT / "tests/qualification_fixtures/artifacts",
        )

    @staticmethod
    def errors_from(callback) -> list[str]:
        errors = qualification.ErrorCollector()
        callback(errors)
        return errors.errors

    def test_four_independent_closed_draft_2020_12_schemas(self) -> None:
        names = (
            "campaign-plan",
            "qualification-record",
            "promotion-review",
            "promotion-proposal",
        )
        expected_types = {
            "campaign-plan": "campaign_plan",
            "qualification-record": "qualification_record",
            "promotion-review": "promotion_review",
            "promotion-proposal": "promotion_proposal",
        }
        for name in names:
            with self.subTest(schema=name):
                path = ROOT / "qualification/schemas" / f"{name}.schema.json"
                raw = path.read_bytes()
                schema = json.loads(raw)
                self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
                self.assertTrue(schema["$id"].endswith(f"/{name}.schema.json"))
                self.assertFalse(schema["additionalProperties"])
                self.assertEqual(schema["properties"]["schema_version"], {"const": 1})
                self.assertEqual(
                    schema["properties"]["document_type"],
                    {"const": expected_types[name]},
                )
                self.assertTrue(raw.endswith(b"\n"))
                self.assertFalse(raw.endswith(b"\n\n"))

    def test_all_scope_fixtures_validate_and_proposals_are_deterministic(self) -> None:
        for scope in self.SCOPES:
            with self.subTest(scope=scope), tempfile.TemporaryDirectory() as temporary:
                plan, record, review, artifacts = self.fixture_paths(scope)
                validated = qualification.validate_set(plan, record, review, artifacts)
                proposal = qualification.build_proposal(validated)
                first = pathlib.Path(temporary) / "first.json"
                second = pathlib.Path(temporary) / "second.json"
                data = qualification._canonical_bytes(proposal)
                qualification.write_new_atomic(first, data, (plan, record, review, artifacts))
                qualification.write_new_atomic(second, data, (plan, record, review, artifacts))
                self.assertEqual(first.read_bytes(), second.read_bytes())
                self.assertTrue(data.endswith(b"\n"))
                self.assertFalse(data.endswith(b"\n\n"))
                self.assertEqual(proposal["claim_scope"], scope)
                self.assertEqual(proposal["evidence_class"], "synthetic_fixture")
                self.assertFalse(proposal["support_matrix_eligible"])
                self.assertEqual(proposal["proposal_label"], "proposal_only")

    def test_plan_required_groups_scope_facts_and_bounds_fail_closed(self) -> None:
        plan_path, _, _, _ = self.fixture_paths("nvidia")
        base = json.loads(plan_path.read_text(encoding="utf-8"))
        for field in (
            "campaign_id", "tuple_id", "source", "host", "topology", "policy",
            "workload", "measurement", "accelerator", "trials", "thresholds",
        ):
            with self.subTest(field=field):
                mutated = copy.deepcopy(base)
                del mutated[field]
                messages = self.errors_from(lambda errors: qualification.validate_plan(mutated, errors))
                self.assertTrue(any(f"missing required property {field}" in message for message in messages))

        for scope, missing in (("nvidia", "nvidia"), ("xdma", "xdma")):
            path, _, _, _ = self.fixture_paths(scope)
            mutated = json.loads(path.read_text(encoding="utf-8"))
            del mutated["accelerator"][missing]
            messages = self.errors_from(lambda errors: qualification.validate_plan(mutated, errors))
            self.assertTrue(any("accelerator identities" in message for message in messages))

        combined_path, _, _, _ = self.fixture_paths("combined")
        combined = json.loads(combined_path.read_text(encoding="utf-8"))
        combined["accelerator"]["host_staging"]["direct_peer_dma"] = True
        messages = self.errors_from(lambda errors: qualification.validate_plan(combined, errors))
        self.assertTrue(any("direct_peer_dma" in message for message in messages))

        rt2_path, _, _, _ = self.fixture_paths("rt2")
        rt2 = json.loads(rt2_path.read_text(encoding="utf-8"))
        rt2["policy"]["rt2"]["preempt_rt"] = False
        messages = self.errors_from(lambda errors: qualification.validate_plan(rt2, errors))
        self.assertTrue(any("PREEMPT_RT" in message for message in messages))

        for field, value in (
            ("warmup_samples", qualification.MAX_SAMPLE_COUNT + 1),
            ("duration_seconds", qualification.MAX_DURATION_SECONDS + 1),
            ("sample_count", 0),
        ):
            mutated = copy.deepcopy(base)
            mutated["measurement"][field] = value
            messages = self.errors_from(lambda errors: qualification.validate_plan(mutated, errors))
            self.assertTrue(any(field in message for message in messages))

    def test_trial_threshold_identity_and_recomputed_result_rejection(self) -> None:
        plan_path, record_path, _, _ = self.fixture_paths("nvidia")
        plan_doc = qualification.load_document(plan_path, "campaign_plan")
        base = json.loads(record_path.read_text(encoding="utf-8"))
        mutations = []
        missing_trial = copy.deepcopy(base)
        missing_trial["trials"].pop()
        mutations.append(missing_trial)
        duplicate_trial = copy.deepcopy(base)
        duplicate_trial["trials"][1]["id"] = duplicate_trial["trials"][0]["id"]
        mutations.append(duplicate_trial)
        failed_trial = copy.deepcopy(base)
        failed_trial["trials"][0]["status"] = "fail"
        mutations.append(failed_trial)
        malformed_recovery = copy.deepcopy(base)
        malformed_recovery["trials"][5]["recovery"]["success"] = False
        mutations.append(malformed_recovery)
        drift = copy.deepcopy(base)
        drift["thresholds"][0]["definition"]["bound"] = 100
        mutations.append(drift)
        forged = copy.deepcopy(base)
        forged["thresholds"][0]["observed"] = 11
        forged["thresholds"][0]["passed"] = True
        mutations.append(forged)
        allowance = copy.deepcopy(base)
        allowance["thresholds"][0]["misses"] = 1
        mutations.append(allowance)
        unstable = copy.deepcopy(base)
        unstable["resource_trends"][0]["stable"] = False
        mutations.append(unstable)
        unhealthy = copy.deepcopy(base)
        unhealthy["device_health"]["healthy"] = False
        mutations.append(unhealthy)
        lost = copy.deepcopy(base)
        lost["device_health"]["losses"] = 1
        mutations.append(lost)
        for index, mutated in enumerate(mutations):
            with self.subTest(mutation=index):
                messages = self.errors_from(
                    lambda errors: qualification.validate_record(
                        mutated, plan_doc.value, plan_doc.sha256, errors
                    )
                )
                self.assertTrue(messages)

    def test_trend_values_reject_growth_inconsistent_extrema_and_thermal_range(self) -> None:
        plan_path, record_path, _, _ = self.fixture_paths("nvidia")
        plan = qualification.load_document(plan_path, "campaign_plan")
        base = json.loads(record_path.read_text(encoding="utf-8"))

        growth = copy.deepcopy(base)
        growth["resource_trends"][0].update(
            {"start": 1, "end": 2, "maximum": 2, "stable": True}
        )
        inconsistent = copy.deepcopy(base)
        inconsistent["resource_trends"][0].update(
            {"start": 2, "end": 1, "maximum": 0, "stable": True}
        )
        out_of_range = copy.deepcopy(base)
        out_of_range["thermal_trends"][0].update(
            {"start": 301, "end": 301, "maximum": 301, "stable": True}
        )

        for name, mutated, expected in (
            ("growth", growth, "must not grow"),
            ("maximum", inconsistent, "must cover start and end"),
            ("thermal", out_of_range, "thermal value out of range"),
        ):
            with self.subTest(case=name):
                messages = self.errors_from(
                    lambda errors: qualification.validate_record(
                        mutated, plan.value, plan.sha256, errors
                    )
                )
                self.assertTrue(any(expected in message for message in messages))

    def test_cross_document_digest_identity_and_review_failures(self) -> None:
        plan_path, record_path, review_path, _ = self.fixture_paths("nvidia")
        plan = qualification.load_document(plan_path, "campaign_plan")
        record = qualification.load_document(record_path, "qualification_record")
        review = json.loads(review_path.read_text(encoding="utf-8"))
        for field, value in (
            ("plan_sha256", "0" * 64),
            ("record_sha256", "0" * 64),
            ("evidence_manifest_sha256", "0" * 64),
            ("scope", "xdma"),
            ("tuple_id", "substituted-tuple"),
            ("decision", "reject"),
            ("pre_run_provenance_verified", False),
            ("reviewer_authentication", "signed"),
        ):
            with self.subTest(field=field):
                mutated = copy.deepcopy(review)
                mutated[field] = value
                messages = self.errors_from(
                    lambda errors: qualification.validate_review(
                        mutated, plan.value, record.value, plan.sha256,
                        record.sha256, errors,
                    )
                )
                self.assertTrue(messages)
        mutated = copy.deepcopy(review)
        mutated["exceptions"] = [
            {"id": "waiver", "resolved": False, "rationale": "synthetic"}
        ]
        messages = self.errors_from(
            lambda errors: qualification.validate_review(
                mutated, plan.value, record.value, plan.sha256,
                record.sha256, errors,
            )
        )
        self.assertTrue(any("exceptions" in message for message in messages))

    def test_strict_json_parser_rejects_ambiguous_and_bounded_input(self) -> None:
        cases = (
            b'{"schema_version":1,"schema_version":1,"document_type":"campaign_plan"}',
            b'{"schema_version":1,"document_type":"campaign_plan","x":NaN}',
            b'{"schema_version":1,"document_type":"campaign_plan"} trailing',
            b'\xef\xbb\xbf{"schema_version":1,"document_type":"campaign_plan"}',
            b'\xff',
            (b'[' * (qualification.MAX_NESTING_DEPTH + 1)) + (b']' * (qualification.MAX_NESTING_DEPTH + 1)),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for index, raw in enumerate(cases):
                with self.subTest(case=index):
                    path = root / f"case-{index}.json"
                    path.write_bytes(raw)
                    with self.assertRaises(qualification.ValidationFailure):
                        qualification.load_document(path, "campaign_plan")
            oversized = root / "oversized.json"
            oversized.write_bytes(b" " * (qualification.MAX_JSON_BYTES + 1))
            with self.assertRaises(qualification.ValidationFailure):
                qualification.load_document(oversized, "campaign_plan")

    def test_artifact_manifest_paths_digests_symlinks_and_inventory(self) -> None:
        self.assertIsNone(qualification._safe_relative_path("../escape"))
        self.assertIsNone(qualification._safe_relative_path("/absolute"))
        self.assertIsNone(qualification._safe_relative_path("a//b"))
        self.assertIsNone(qualification._safe_relative_path(r"a\b"))
        self.assertIsNone(qualification._safe_relative_path("C:/drive"))
        self.assertIsNone(qualification._safe_relative_path("a/./b"))
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            artifact = root / "raw.bin"
            artifact.write_bytes(b"payload")
            entry = {
                "path": "raw.bin",
                "size_bytes": 7,
                "sha256": hashlib.sha256(b"payload").hexdigest(),
            }
            errors = qualification.ErrorCollector()
            qualification.verify_artifacts(root, [entry], errors)
            self.assertEqual(errors.errors, [])

            (root / "unlisted.bin").write_bytes(b"secret sentinel")
            errors = qualification.ErrorCollector()
            qualification.verify_artifacts(root, [entry], errors)
            self.assertTrue(any("unlisted" in message for message in errors.errors))
            self.assertFalse(any("secret sentinel" in message for message in errors.errors))
            (root / "unlisted.bin").unlink()

            link = root / "link.bin"
            try:
                link.symlink_to(artifact)
            except (OSError, NotImplementedError):
                pass
            else:
                linked = copy.deepcopy(entry)
                linked["path"] = "link.bin"
                errors = qualification.ErrorCollector()
                qualification.verify_artifacts(root, [linked], errors)
                self.assertTrue(any("symlink" in message for message in errors.errors))

            bad = copy.deepcopy(entry)
            bad["size_bytes"] = 8
            bad["sha256"] = "0" * 64
            errors = qualification.ErrorCollector()
            qualification.verify_artifacts(root, [bad], errors)
            self.assertTrue(any("size mismatch" in message for message in errors.errors))
            self.assertTrue(any("digest mismatch" in message for message in errors.errors))

        too_many = [
            {"path": f"a-{index:03d}", "size_bytes": 0, "sha256": "0" * 64}
            for index in range(qualification.MAX_ARTIFACTS + 1)
        ]
        messages = self.errors_from(
            lambda errors: qualification._validate_manifest_shape(
                too_many, "manifest", errors
            )
        )
        self.assertTrue(any("artifact count" in message for message in messages))

    def test_artifact_tree_scan_rejects_excess_entries_before_unbounded_walk(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for name in ("a", "b", "c"):
                (root / name).mkdir()
            errors = qualification.ErrorCollector()
            with mock.patch.object(qualification, "MAX_ARTIFACT_TREE_ENTRIES", 2):
                qualification.verify_artifacts(root, [], errors)
            self.assertTrue(
                any("tree entry count exceeds 2" in message for message in errors.errors)
            )

    def test_artifact_hashing_stops_at_actual_aggregate_byte_limit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = []
            for name in ("a.bin", "b.bin", "c.bin"):
                payload = b"xx"
                (root / name).write_bytes(payload)
                manifest.append(
                    {
                        "path": name,
                        "size_bytes": len(payload),
                        "sha256": hashlib.sha256(payload).hexdigest(),
                    }
                )
            errors = qualification.ErrorCollector()
            original = qualification._sha256_file
            with (
                mock.patch.object(qualification, "MAX_ARTIFACT_BYTES", 10),
                mock.patch.object(qualification, "MAX_ARTIFACT_TOTAL_BYTES", 3),
                mock.patch.object(
                    qualification,
                    "_sha256_file",
                    wraps=original,
                ) as hasher,
            ):
                qualification.verify_artifacts(root, manifest, errors)
            self.assertTrue(
                any("aggregate bytes exceed 3" in message for message in errors.errors)
            )
            self.assertEqual(hasher.call_count, 2)

    def test_proposal_nonoverwrite_atomic_cleanup_and_nonmutation(self) -> None:
        plan, record, review, artifacts = self.fixture_paths("nvidia")
        validated = qualification.validate_set(plan, record, review, artifacts)
        data = qualification._canonical_bytes(qualification.build_proposal(validated))
        protected = [
            ROOT / "docs/portable_support_matrix.json",
            ROOT / "docs/cuda_support_matrix.json",
            ROOT / "docs/xdma_support_matrix.json",
            ROOT / "release/rtfw-release-contract.json",
            plan,
            record,
            review,
            artifacts / "raw.json",
        ]
        before = {path: path.read_bytes() for path in protected}
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output = root / "proposal.json"
            output.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                qualification.write_new_atomic(output, data, protected)
            self.assertEqual(output.read_bytes(), b"keep")

            fresh = root / "fresh.json"
            with mock.patch.object(qualification.os, "link", side_effect=KeyboardInterrupt):
                with self.assertRaises(KeyboardInterrupt):
                    qualification.write_new_atomic(fresh, data, protected)
            self.assertFalse(fresh.exists())
            self.assertEqual(list(root.glob(".*.tmp")), [])

            with self.assertRaises(ValueError):
                qualification.write_new_atomic(plan, data, protected)
        self.assertEqual(before, {path: path.read_bytes() for path in protected})

    def test_cli_rejects_output_in_artifacts_and_noncanonical_proposal(self) -> None:
        plan, record, review, artifacts = self.fixture_paths("nvidia")
        output = artifacts / "attempted-proposal.json"
        command = [
            sys.executable,
            str(ROOT / "tools/qualification.py"),
            "propose",
            "--plan", str(plan),
            "--record", str(record),
            "--review", str(review),
            "--artifact-dir", str(artifacts),
            "--output", str(output),
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

        with tempfile.TemporaryDirectory() as temporary:
            validated = qualification.validate_set(plan, record, review, artifacts)
            canonical = qualification._canonical_bytes(
                qualification.build_proposal(validated)
            )
            noncanonical = pathlib.Path(temporary) / "proposal.json"
            noncanonical.write_text(
                json.dumps(json.loads(canonical), indent=2) + "\n",
                encoding="utf-8",
            )
            loaded = qualification.load_document(noncanonical, "promotion_proposal")
            messages = self.errors_from(
                lambda errors: errors.require(
                    loaded.raw == canonical,
                    "proposal bytes are noncanonical",
                )
            )
            self.assertTrue(messages)

    def test_real_combined_and_raw_m12_document_are_not_promotable(self) -> None:
        plan, record, review, artifacts = self.fixture_paths("combined")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            copies = []
            for source in (plan, record, review):
                value = json.loads(source.read_text(encoding="utf-8"))
                value["evidence_class"] = "qualification_campaign"
                destination = root / source.name
                destination.write_text(json.dumps(value) + "\n", encoding="utf-8")
                copies.append(destination)
            with self.assertRaises(qualification.ValidationFailure) as context:
                qualification.validate_set(*copies, artifacts)
            self.assertTrue(any("M17-05" in message for message in context.exception.errors))

            raw = root / "m12-evidence.json"
            raw.write_text(
                json.dumps(HardwareEvidenceTests.evidence("cuda")) + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(qualification.ValidationFailure):
                qualification.load_document(raw, "qualification_record")

    def test_validation_isolation_and_recovery_after_failure(self) -> None:
        plan, record, review, artifacts = self.fixture_paths("nvidia")
        validated = qualification.validate_set(plan, record, review, artifacts)
        self.assertEqual(validated.plan.value["tuple_id"], "fixture-nvidia-tuple")
        with tempfile.TemporaryDirectory() as temporary:
            bad_artifacts = pathlib.Path(temporary) / "artifacts"
            shutil.copytree(artifacts, bad_artifacts)
            (bad_artifacts / "raw.json").write_bytes(b"corrupt")
            with self.assertRaises(qualification.ValidationFailure):
                qualification.validate_set(plan, record, review, bad_artifacts)
        recovered = qualification.validate_set(plan, record, review, artifacts)
        self.assertEqual(recovered.plan.sha256, validated.plan.sha256)


class HardwareEvidenceTests(unittest.TestCase):
    @staticmethod
    def evidence(backend: str) -> dict:
        if backend == "cuda":
            labels = ("host_to_device", "kernel", "device_to_host")
            label_key = "stage"
            identity = {
                "backend_id": "rtfw.cuda.driver.v1",
                "os": "linux",
                "device_name": "test gpu",
                "pci_bus_id": "0000:01:00.0",
                "compute_capability": "8.0",
                "cuda_toolkit_version": 12000,
                "cuda_driver_version": 12000,
            }
        else:
            labels = ("h2c", "c2h")
            label_key = "direction"
            identity = {
                "backend_id": "rtfw.xdma.xilinx_linux_aximm.v1",
                "pci_bdf": "0000:01:00.0",
                "driver_id": "test-driver",
                "bitstream_id": "test-bitstream",
                "h2c_path": "/dev/xdma0_h2c_0",
                "c2h_path": "/dev/xdma0_c2h_0",
                "device_offset": 0,
                "transfer_bytes": 4096,
            }
        warmup = 1
        measurement = 2
        samples = []
        for iteration in range(measurement):
            for label in labels:
                samples.append(
                    {
                        "iteration": iteration,
                        label_key: label,
                        "submit_call_ns": 1,
                        "completion_wait_ns": 2,
                        "poll_call_ns": 1,
                        "poll_count": 1,
                    }
                )
        total = (warmup + measurement) * len(labels)
        return {
            "schema_version": 1,
            "runtime_version": "1.0.0",
            "result": "pass",
            "qualification_claim": "evidence_only",
            **identity,
            "warmup_iterations": warmup,
            "measurement_iterations": measurement,
            "health": {
                "submissions": total,
                "completions": total,
                "timeouts": 0,
                "errors": 0,
                "losses": 0,
            },
            "samples": samples,
        }

    def test_cuda_and_xdma_evidence_only_schemas(self) -> None:
        for backend in ("cuda", "xdma"):
            with self.subTest(backend=backend):
                evidence = self.evidence(backend)
                self.assertEqual(
                    hardware_evidence.validate_evidence(
                        evidence,
                        backend,
                        "1.0.0",
                    ),
                    [],
                )

                promoted = copy.deepcopy(evidence)
                promoted["qualification_claim"] = "qualified"
                errors = hardware_evidence.validate_evidence(
                    promoted,
                    backend,
                    "1.0.0",
                )
                self.assertTrue(
                    any("evidence_only" in error for error in errors)
                )

                duplicated = copy.deepcopy(evidence)
                duplicated["samples"][1] = copy.deepcopy(
                    duplicated["samples"][0]
                )
                errors = hardware_evidence.validate_evidence(
                    duplicated,
                    backend,
                    "1.0.0",
                )
                self.assertTrue(any("duplicates" in error for error in errors))

                unhealthy = copy.deepcopy(evidence)
                unhealthy["health"]["errors"] = 1
                errors = hardware_evidence.validate_evidence(
                    unhealthy,
                    backend,
                    "1.0.0",
                )
                self.assertTrue(
                    any("must be zero" in error for error in errors)
                )


if __name__ == "__main__":
    unittest.main()
