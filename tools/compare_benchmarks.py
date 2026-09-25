#!/usr/bin/env python3
"""M23 offline baseline capture, comparison, review proposals and reports.

The v1 runner, bundle validator and evidence classes remain unchanged.
No subprocess, profiler, device, baseline update or threshold approval runs here.
"""
from __future__ import annotations

import argparse
import datetime as dt
from fractions import Fraction
import math
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Any

import check_benchmark_artifact as artifact

ROOT = Path(__file__).resolve().parents[1]
SCHEMAS = ROOT / "bench/analysis/contracts.schema.json"
PPM = 1_000_000
MAX_TOTAL_BYTES = 512 * 1024 * 1024
require = artifact.require
canonical = artifact.canonical
digest = artifact.digest


def read_document(path: Path) -> tuple[dict[str, Any], str]:
    with path.open("rb") as stream:
        raw = stream.read(artifact.MAX_BYTES + 1)
    value = artifact.decode(raw, canonical_required=False)
    require(type(value) is dict, "document must be an object")
    return value, digest(raw)


def validate(value: dict[str, Any], kind: str) -> None:
    schemas, _ = read_document(SCHEMAS)
    artifact.validate_schema(value, schemas["$defs"][kind])


def timestamp(value: str) -> dt.datetime:
    return dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def key(case: dict[str, Any]) -> str:
    return case["provider_id"] + ":" + case["case_id"]


def policy_file(path: Path) -> tuple[dict[str, Any], str]:
    policy, sha = read_document(path)
    validate(policy, "policy")
    ids = [key(c) for c in policy["cases"]]
    require(len(ids) == len(set(ids)), "duplicate policy case")
    for case in policy["cases"]:
        names = case["invariant_counters"]
        require(len(names) == len(set(names)), "duplicate invariant counter")
        require((case["regression_limit_ppm"] is None) == (policy["mode"] == "portable"),
                "portable policy cannot set thresholds; controlled policy must set them")
    return policy, sha


def relative_bundle(root: Path, name: str) -> Path:
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,255}", name) is not None,
            "invalid relative bundle")
    parts = name.split("/")
    require(all(p not in ("", ".", "..") for p in parts), "invalid bundle component")
    return root.joinpath(*parts)


def load_bundle(root: Path, entry: dict[str, Any], budget: list[int]) -> dict[str, Any]:
    path = relative_bundle(root, entry["bundle"])
    files = artifact.read_bundle(path)
    budget[0] += sum(map(len, files.values()))
    require(budget[0] <= MAX_TOTAL_BYTES, "analysis input budget exceeded")
    hashes = {name: digest(raw) for name, raw in files.items()}
    if "files" in entry:
        require(hashes == entry["files"], "manifest artifact digest mismatch")
    result = artifact.validate_bundle(path)
    require(files == artifact.read_bundle(path), "bundle changed during validation")
    descriptor = artifact.decode(files["descriptor.json"])
    raw = artifact.decode(files["raw.json"])
    return {"entry": entry, "files": hashes, "result": result, "descriptor": descriptor,
            "samples": raw["samples"]}


def capture(plan_path: Path, policy_path: Path, root: Path, role: str) -> dict[str, Any]:
    policy, policy_sha = policy_file(policy_path)
    plan, _ = read_document(plan_path)
    validate(plan, "plan")
    require(len({r["trial_id"] for r in plan["runs"]}) == len(plan["runs"]), "duplicate trial ID")
    require(len({r["bundle"] for r in plan["runs"]}) == len(plan["runs"]), "duplicate bundle")
    allowed = {key(c) for c in policy["cases"]}
    budget = [0]
    runs = []
    for entry in plan["runs"]:
        bundle = load_bundle(root, entry, budget)
        require(key(bundle["descriptor"]) in allowed, "undeclared policy case")
        runs.append({**entry, "files": bundle["files"]})
    manifest = {"schema_version": 1, "kind": role, "created_utc": now(),
                "policy_sha256": policy_sha, "collection": plan["collection"], "runs": runs}
    validate(manifest, "manifest")
    return manifest


def median(values: list[int | Fraction]) -> Fraction:
    ordered = sorted(values)
    n = len(ordered)
    require(n > 0, "empty population")
    return (Fraction(ordered[(n - 1) // 2]) + Fraction(ordered[n // 2])) / 2


def interval(values: list[Fraction], confidence_ppm: int, family: int) -> tuple[Fraction, Fraction] | None:
    """Exact non-interpolated order-statistic interval, alpha/(2*cases).

    Each side's two-tailed coverage error is bounded by alpha/(2*cases).
    Union bounds cover both populations and every declared case, without
    independence across cases. Runs within each population must be IID.
    """
    ordered = sorted(values)
    n = len(ordered)
    alpha = Fraction(PPM - confidence_ppm, PPM * 2 * family)
    chosen = None
    tail = 0
    for k in range(1, n // 2 + 1):
        tail += math.comb(n, k - 1)
        if Fraction(2 * tail, 2 ** n) <= alpha:
            chosen = (ordered[k - 1], ordered[n - k])
        else:
            break
    return chosen


def rational(value: Fraction | int) -> dict[str, int]:
    value = Fraction(value)
    return {"numerator": value.numerator, "denominator": value.denominator}


def noisy(values: list[int | Fraction], policy: dict[str, Any]) -> dict[str, Any]:
    center = median(values)
    if center <= 0:
        return {"median_ns": rational(center), "mad_ppm": None,
                "outliers": len(values), "accepted": False}
    deviations = [abs(Fraction(x) - center) for x in values]
    mad = median(deviations) * PPM / center
    outliers = sum(d * PPM > policy["outlier_relative_ppm"] * center for d in deviations)
    return {"median_ns": rational(center), "mad_ppm": rational(mad), "outliers": outliers,
            "accepted": mad <= policy["max_mad_ppm"] and outliers == 0}


def manifest_file(path: Path, role: str, policy_sha: str, root: Path,
                  budget: list[int]) -> tuple[dict[str, Any], str, list[dict[str, Any]]]:
    value, sha = read_document(path)
    validate(value, "manifest")
    timestamp(value["created_utc"])
    require(value["kind"] == role and value["policy_sha256"] == policy_sha, "manifest policy/role mismatch")
    entries = value["runs"]
    require(len({e["trial_id"] for e in entries}) == len(entries), "duplicate trial ID")
    require(len({e["bundle"] for e in entries}) == len(entries), "duplicate bundle")
    runs = [load_bundle(root, entry, budget) for entry in entries]
    require(all(r["descriptor"]["clock"] != "steady" or
                r["result"]["identity"]["host_label"] == value["collection"]["host_label"] for r in runs),
            "collection host differs from measured identity")
    require(all(r["result"]["end_utc"] in ("not_available", "1970-01-01T00:00:00Z") or
                r["result"]["end_utc"] <= value["created_utc"] for r in runs), "capture predates run")
    return value, sha, runs


def review_proposal(baseline: Path, policy_path: Path) -> dict[str, Any]:
    policy, policy_sha = policy_file(policy_path)
    value, sha = read_document(baseline)
    validate(value, "manifest")
    require(policy["mode"] == "controlled" and value["kind"] == "baseline" and
            value["policy_sha256"] == policy_sha, "review requires controlled baseline")
    return {"schema_version": 1, "kind": "threshold_review", "decision": "proposal_only",
            "baseline_sha256": sha, "policy_sha256": policy_sha,
            "collection_sha256": digest(canonical(value["collection"])),
            "reviewer": "not_available", "approved_utc": "not_available"}


def check_review(path: Path | None, baseline: dict[str, Any], baseline_sha: str,
                 policy_sha: str, runs: list[dict[str, Any]]) -> tuple[bool, str | None]:
    if path is None:
        return False, None
    review, sha = read_document(path)
    validate(review, "review")
    require(review["baseline_sha256"] == baseline_sha and review["policy_sha256"] == policy_sha and
            review["collection_sha256"] == digest(canonical(baseline["collection"])), "review binding mismatch")
    if review["decision"] != "approved":
        return False, sha
    require(review["reviewer"] != "not_available", "missing reviewer declaration")
    approved = timestamp(review["approved_utc"])
    require(timestamp(baseline["created_utc"]) <= approved, "review predates baseline")
    require(all(r["result"]["start_utc"] != "not_available" and
                timestamp(r["result"]["start_utc"]) > approved for r in runs), "candidate not after approval")
    return True, sha


def profiler_file(path: Path | None, runs: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], str | None]:
    if path is None:
        return [], None
    value, sha = read_document(path)
    validate(value, "profiler")
    by_hash = {r["files"]["result.json"]: r for r in runs}
    seen = set()
    for link in value["links"]:
        require(link["result_file_sha256"] in by_hash, "profiler references unknown result")
        run = by_hash[link["result_file_sha256"]]
        require(key(link) == key(run["descriptor"]) and
                link["run_context_sha256"] == run["result"]["run_context_sha256"], "profiler context mismatch")
        require(link["first_sample"] <= link["last_sample"] < len(run["samples"]), "profiler sample range")
        identity = (link["result_file_sha256"], link["trace_sha256"], link["first_sample"], link["last_sample"])
        require(identity not in seen, "duplicate profiler link")
        seen.add(identity)
        require((link["relationship"] == "uncorrelated") == (link["correlation_record_sha256"] is None),
                "clock relationship needs explicit correlation record digest")
    return value["links"], sha


def identity_reason(runs: list[dict[str, Any]], steady: bool) -> str | None:
    identities = [r["result"]["identity"] for r in runs]
    ignored = {"source_commit", "source_tree"}
    fingerprint = lambda v: {k: x for k, x in v.items() if k not in ignored}
    if any(fingerprint(i) != fingerprint(identities[0]) for i in identities[1:]):
        return "identity_mismatch"
    if steady:
        optional = {"temperature", "frequency", "policy_origin"}
        if any(i["source_dirty"] != "false" or any(x in ("unknown", "not_available")
               for k, x in i.items() if k not in optional) for i in identities):
            return "identity_incomplete"
    return None


def run_summary(run: dict[str, Any]) -> dict[str, Any]:
    return {"trial_id": run["entry"]["trial_id"], "files": run["files"],
            "command": run["entry"]["command"], "identity": run["result"]["identity"],
            "start_utc": run["result"]["start_utc"], "end_utc": run["result"]["end_utc"],
            "run_context_sha256": run["result"]["run_context_sha256"],
            "status": run["result"]["status"], "sample_count": len(run["samples"]),
            "statistics": run["result"]["statistics"]}


def compare_case(case: dict[str, Any], left: list[dict[str, Any]], right: list[dict[str, Any]],
                 policy: dict[str, Any], independent: bool, reviewed: bool) -> dict[str, Any]:
    row = {"case": key(case), "status": "inconclusive", "reason": "", "performance": None,
           "noise": None, "baseline": [run_summary(r) for r in left],
           "candidate": [run_summary(r) for r in right]}
    def reject(reason: str, status: str = "inconclusive") -> dict[str, Any]:
        row.update(reason=reason, status=status)
        return row
    if not left or not right:
        return reject("missing_case", "incompatible")
    runs = left + right
    if any(r["result"]["status"] not in ("ok", "not_run") for r in runs):
        return reject("failed_run", "failed")
    if any(r["result"]["status"] == "not_run" for r in runs):
        return reject("prerequisite_unavailable", "not_run")
    descriptor = runs[0]["descriptor"]
    if any(r["descriptor"] != descriptor for r in runs[1:]):
        return reject("descriptor_mismatch", "incompatible")
    for group in (left, right):
        if len({(r["result"]["identity"]["source_commit"], r["result"]["identity"]["source_tree"])
                for r in group}) != 1:
            return reject("mixed_source_population", "incompatible")
    steady = descriptor["clock"] == "steady"
    reason = identity_reason(runs, steady)
    if reason:
        return reject(reason, "incompatible")
    names = {c["name"] for c in descriptor["counters"]}
    if not set(case["invariant_counters"]) <= names:
        return reject("missing_counter", "incompatible")
    reference = runs[0]["samples"]
    if any([s["checksum"] for s in r["samples"]] != [s["checksum"] for s in reference] or
           any([s["counters"][n] for s in r["samples"]] != [s["counters"][n] for s in reference]
               for n in case["invariant_counters"]) for r in runs[1:]):
        return reject("correctness_or_counter_mismatch", "incompatible")
    if not steady:
        return reject("fake_clock_no_performance_inference",
                      "structural_match" if policy["mode"] == "portable" else "inconclusive")
    if not independent:
        return reject("independent_runs_not_declared")
    if min(len(left), len(right)) < policy["minimum_runs"] or any(
            len(r["samples"]) < policy["minimum_samples"] for r in runs):
        return reject("insufficient_runs_or_samples")
    if len({r["files"]["result.json"] for r in runs}) != len(runs):
        return reject("reused_run_evidence", "incompatible")
    populations = [[median([s["duration_ns"] for s in r["samples"]]) for r in group]
                   for group in (left, right)]
    noise = {"within_runs": [noisy([s["duration_ns"] for s in r["samples"]], policy) for r in runs],
             "between_runs": [noisy(p, policy) for p in populations]}
    row["noise"] = noise
    if not all(n["accepted"] for values in noise.values() for n in values):
        return reject("noise_or_outliers")
    bounds = [interval(p, policy["confidence_ppm"], len(policy["cases"])) for p in populations]
    if any(b is None for b in bounds):
        return reject("insufficient_confidence")
    base, candidate = bounds
    assert base is not None and candidate is not None
    if base[0] <= 0:
        return reject("zero_baseline")
    low, high = candidate[0] / base[1], candidate[1] / base[0]
    ratio = median(populations[1]) / median(populations[0])
    row["performance"] = {"baseline_median_ns": rational(median(populations[0])),
                          "candidate_median_ns": rational(median(populations[1])),
                          "difference_ns": rational(median(populations[1]) - median(populations[0])),
                          "ratio": rational(ratio), "ratio_interval": [rational(low), rational(high)],
                          "confidence_ppm": policy["confidence_ppm"],
                          "family_cases": len(policy["cases"]),
                          "direction": "slower" if low > 1 else "faster" if high < 1 else "unresolved",
                          "regression_limit_ppm": case["regression_limit_ppm"]}
    if policy["mode"] == "portable":
        return reject("portable_descriptive_only", "characterization")
    if not reviewed:
        return reject("threshold_review_required")
    limit = 1 + Fraction(case["regression_limit_ppm"], PPM)
    if low > limit:
        return reject("reviewed_threshold_exceeded", "regression")
    if high <= limit:
        return reject("reviewed_threshold_satisfied", "pass")
    return reject("threshold_overlaps_confidence_interval")


def compare(args: argparse.Namespace) -> dict[str, Any]:
    policy, policy_sha = policy_file(args.policy)
    budget = [0]
    baseline, baseline_sha, left = manifest_file(args.baseline, "baseline", policy_sha, args.baseline_root, budget)
    candidate, candidate_sha, right = manifest_file(args.candidate, "candidate", policy_sha, args.candidate_root, budget)
    allowed = {key(c) for c in policy["cases"]}
    require(all(key(r["descriptor"]) in allowed for r in left + right), "undeclared case in manifest")
    require(all(sum(key(r["descriptor"]) == k for r in group) <= 64 for k in allowed for group in (left, right)),
            "too many runs per case")
    collection_match = baseline["collection"] == candidate["collection"]
    independent = collection_match and baseline["collection"]["independence"] == "independent_process_runs"
    require(policy["mode"] == "controlled" or args.review is None, "portable comparison cannot consume threshold review")
    reviewed, review_sha = check_review(args.review, baseline, baseline_sha, policy_sha, right)
    if reviewed:
        require(collection_match and all(v != "not_available" for v in baseline["collection"].values()),
                "reviewed collection is incomplete or changed")
    links, profiler_sha = profiler_file(args.profiler, left + right)
    rows = []
    for case in sorted(policy["cases"], key=key):
        l = [r for r in left if key(r["descriptor"]) == key(case)]
        r = [r for r in right if key(r["descriptor"]) == key(case)]
        row = compare_case(case, l, r, policy, independent, reviewed)
        if not collection_match:
            row.update(status="incompatible", reason="collection_controls_mismatch", performance=None)
        rows.append(row)
    priority = ["failed", "incompatible", "regression", "not_run", "inconclusive",
                "characterization", "structural_match", "pass"]
    status = next(s for s in priority if any(r["status"] == s for r in rows))
    report = {"schema_version": 1, "kind": "benchmark_comparison", "tool_version": "1",
            "mode": policy["mode"], "status": status, "qualification": "none",
            "baseline_sha256": baseline_sha, "candidate_sha256": candidate_sha,
            "policy_sha256": policy_sha, "review_sha256": review_sha,
            "review_origin": "external_unverified_declaration" if reviewed else "none",
            "profiler_sha256": profiler_sha, "profiler_links": links, "cases": rows}
    validate(report, "report")
    return report


def markdown(report: dict[str, Any]) -> str:
    lines = ["# Benchmark comparison", "", f"Status: **{report['status']}**. Mode: {report['mode']}.",
             "Qualification: none. Review identity/chronology are external declarations, not authentication.",
             "", "| Case | Disposition | Reason | Candidate / baseline median |", "| --- | --- | --- | --- |"]
    for row in report["cases"]:
        p = row["performance"]
        ratio = "not available" if p is None else f"{p['ratio']['numerator']}/{p['ratio']['denominator']}"
        lines.append(f"| {row['case']} | {row['status']} | {row['reason']} | {ratio} |")
    lines += ["", "## Evidence", ""]
    for name in ("baseline_sha256", "candidate_sha256", "policy_sha256", "review_sha256", "profiler_sha256"):
        lines.append(f"- {name}: {report[name] or 'not_available'}")
    lines += ["", "No samples were discarded. Confidence uses independent run medians and a family-wide binomial bound.",
              "Profiler links are supplied correlations, not causal attribution. Inspect the JSON report for complete run statistics.", ""]
    return "\n".join(lines)


def publish(path: Path, data: bytes) -> None:
    """Single-file atomic no-overwrite publication; parents must exist."""
    require(len(data) <= artifact.MAX_BYTES, "report exceeds output bound")
    require(".." not in path.parts, "parent traversal forbidden")
    path = path.absolute()
    require(not path.exists() and not path.is_symlink(), "output already exists")
    fd, temporary = tempfile.mkstemp(prefix=".rtfw-analysis-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path)  # Atomic fail-if-exists, including a concurrent winner.
    finally:
        os.unlink(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="action", required=True)
    capture_parser = subs.add_parser("capture", help="Capture exact existing run bytes into a new manifest")
    capture_parser.add_argument("--role", choices=("baseline", "candidate"), required=True)
    capture_parser.add_argument("--plan", type=Path, required=True)
    capture_parser.add_argument("--run-root", type=Path, required=True)
    proposal = subs.add_parser("review-proposal", help="Emit proposal_only; cannot approve")
    proposal.add_argument("--baseline", type=Path, required=True)
    comparison = subs.add_parser("compare")
    for name in ("baseline", "candidate", "baseline-root", "candidate-root"):
        comparison.add_argument("--" + name, type=Path, required=True)
    for name in ("review", "profiler"):
        comparison.add_argument("--" + name, type=Path)
    comparison.add_argument("--format", choices=("json", "markdown"), default="json")
    for sub in (capture_parser, proposal, comparison):
        sub.add_argument("--policy", type=Path, required=True)
        sub.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    switches = [a.split("=", 1)[0] for a in sys.argv[1:] if a.startswith("--")]
    if len(switches) != len(set(switches)):
        parser.error("duplicate option")
    try:
        require(not args.output.exists(), "output already exists")
        if args.action == "capture":
            result = capture(args.plan, args.policy, args.run_root, args.role)
        elif args.action == "review-proposal":
            result = review_proposal(args.baseline, args.policy)
        else:
            result = compare(args)
        data = markdown(result).encode("utf-8") if getattr(args, "format", "json") == "markdown" else canonical(result)
        publish(args.output, data)
        if args.action != "compare":
            print("Benchmark analysis document written")
            return 0
        print("Benchmark comparison: " + result["status"])
        return {"pass": 0, "structural_match": 0, "characterization": 0, "regression": 1,
                "incompatible": 2, "failed": 2, "not_run": 3, "inconclusive": 4}[result["status"]]
    except (ValueError, OSError, KeyError, RecursionError, UnicodeError):
        print("Benchmark analysis rejected", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
