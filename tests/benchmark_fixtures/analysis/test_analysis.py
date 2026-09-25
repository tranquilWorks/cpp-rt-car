#!/usr/bin/env python3
"""Synthetic statistical fixtures and actual analysis/template CLI integration.

All timing/host/review values generated here are test data, never bench evidence.
"""
from __future__ import annotations
import argparse
import copy
from fractions import Fraction
import itertools
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import compare_benchmarks as analysis

parser = argparse.ArgumentParser()
parser.add_argument("--template", type=Path)
args, remaining = parser.parse_known_args()
TEMPLATE = args.template.resolve() if args.template else None


def save(path, value):
    path.write_bytes(analysis.canonical(value))


def write_bundle(path, duration=100, trial=0, candidate=False, clock="steady", status="ok", changes=None):
    path.mkdir(exist_ok=True)
    descriptor = dict(schema_version=1, provider_id="synthetic.analysis", provider_version=1,
                      case_id="integer", subsystem="test", implementation="test-v1", configuration="fixed",
                      workload_kind="integer", workload_sha256="a" * 64, parameters=[],
                      counters=[dict(name="operations", unit="count", minimum=0, maximum=100)],
                      clock=clock, unit="ns", comparison_policy="none", raw_policy="all_measured",
                      evidence_class="structural_fixture" if clock == "fake" else "portable_characterization",
                      warmup=2, repetitions=5)
    identity = dict(source_commit=("2" if candidate else "1") * 40, source_tree=("4" if candidate else "3") * 40,
                    source_dirty="false", compiler="synthetic", compiler_version="1", build_configuration="test",
                    build_flags_sha256="b" * 64, os="test", kernel="test", architecture="test", cpu_model="test",
                    host_label="synthetic-host", logical_cpus=2, total_memory_bytes=4096, page_size=4096,
                    thread_policy="synthetic", memory_policy="synthetic", backend="test", driver="test",
                    frequency="not_available", temperature="not_available", policy_origin="caller_declared_or_unavailable")
    utc = "1970-01-01T00:00:00Z" if clock == "fake" else f"2026-01-0{3 if candidate else 1}T00:{trial // 60:02}:{trial % 60:02}Z"
    result = dict(schema_version=1, runner_version="1", descriptor_file="descriptor.json", raw_file="raw.json",
                  workload_sha256=descriptor["workload_sha256"], clock=clock, evidence_class=descriptor["evidence_class"],
                  qualification="none", identity=identity, start_utc=utc, end_utc=utc, status=status,
                  warmup_completed=2 if status == "ok" else 0, measured_completed=5 if status == "ok" else 0,
                  diagnostic="" if status == "ok" else "prerequisite unavailable" if status == "not_run" else "provider invocation failed")
    durations = duration if isinstance(duration, list) else [duration] * 5
    samples = []
    tick = 0
    if status == "ok":
        for i, value in enumerate(durations):
            samples.append(dict(index=i, start_ns=tick, end_ns=tick+value, duration_ns=value,
                                counters={"operations":16}, checksum=i+10, invariants_passed=True))
            tick += value
    raw = dict(schema_version=1, samples=samples)
    if changes:
        changes(descriptor, raw, result)
    if result["status"] == "ok":
        values = sorted(s["duration_ns"] for s in samples)
        result["statistics"] = dict(counter_totals={n:sum(s["counters"][n] for s in samples) for n in samples[0]["counters"]},
                                    min_ns=min(values), max_ns=max(values), total_ns=sum(values), percentile_method="nearest_rank",
                                    **{f"p{p}_ns":values[(len(values)*p+99)//100-1] for p in (50,95,99)})
    else:
        result["statistics"] = None
    db = analysis.canonical(descriptor)
    result["descriptor_sha256"] = raw["descriptor_sha256"] = analysis.digest(db)
    context = {"descriptor_sha256":result["descriptor_sha256"],"identity":identity,"start_utc":result["start_utc"]}
    result["run_context_sha256"] = raw["run_context_sha256"] = analysis.digest(analysis.canonical(context)[:-1])
    rb = analysis.canonical(raw)
    result["raw_sha256"] = analysis.digest(rb)
    result["result_sha256"] = analysis.digest(analysis.canonical(result))
    (path/"descriptor.json").write_bytes(db)
    (path/"raw.json").write_bytes(rb)
    save(path/"result.json", result)


class Statistics(unittest.TestCase):
    def test_exact_binomial_interval_independently_enumerated(self):
        # Eleven IID Bernoulli signs: [X2,X10] misses in 24 of 2048
        # sign patterns, below the per-population 0.025 budget.
        signs = list(itertools.product((0,1), repeat=11))
        self.assertEqual(sum(sum(s)<2 or sum(s)>9 for s in signs), 24)
        self.assertEqual(analysis.interval(list(map(Fraction, range(1,12))),950000,1),(2,10))
        self.assertEqual(analysis.interval(list(map(Fraction, range(1,12))),950000,4),(1,11))
        self.assertIsNone(analysis.interval([Fraction(1)]*5,950000,1))
        self.assertIsNone(analysis.interval([Fraction(1)]*9,999900,256))

    def test_even_median_and_rational_preservation(self):
        self.assertEqual(analysis.median([1,2]), Fraction(3,2))
        self.assertEqual(analysis.rational(Fraction(101,100)),dict(numerator=101,denominator=100))


class Analysis(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.policy = dict(schema_version=1,kind="comparison_policy",mode="controlled",confidence_ppm=950000,
                           minimum_runs=9,minimum_samples=5,max_mad_ppm=100000,outlier_relative_ppm=500000,
                           cases=[dict(provider_id="synthetic.analysis",case_id="integer",invariant_counters=["operations"],regression_limit_ppm=100000)])
        self.controls = dict(host_label="synthetic-host",independence="independent_process_runs",frequency_policy="synthetic",
                             thermal_protocol="synthetic",affinity_policy="synthetic",background_load="synthetic")
        self.make()

    def make(self, left=100, right=100, n=9, clock="steady", status="ok", changes=None):
        save(self.root/"policy.json",self.policy)
        for candidate in (False,True):
            role = "candidate" if candidate else "baseline"
            entries=[]
            for i in range(n):
                name=f"{role}-{i}"
                write_bundle(self.root/name,left if not candidate else right,i,candidate,clock,
                             status if candidate else "ok",changes if candidate else None)
                entries.append(dict(trial_id=name,bundle=name,command=["synthetic-fixture",name]))
            plan=dict(schema_version=1,kind="run_plan",collection=self.controls,runs=entries)
            save(self.root/f"{role}-plan.json",plan)
            manifest=analysis.capture(self.root/f"{role}-plan.json",self.root/"policy.json",self.root,role)
            manifest["created_utc"]=f"2026-01-0{3 if candidate else 1}T01:00:00Z"
            save(self.root/f"{role}.json",manifest)
        review=analysis.review_proposal(self.root/"baseline.json",self.root/"policy.json") if self.policy["mode"]=="controlled" else None
        if review:
            review.update(decision="approved",reviewer="synthetic-test-reviewer",approved_utc="2026-01-02T00:00:00Z")
            save(self.root/"review.json",review)

    def options(self, review=True, profiler=False):
        return argparse.Namespace(policy=self.root/"policy.json",baseline=self.root/"baseline.json",candidate=self.root/"candidate.json",
                                  baseline_root=self.root,candidate_root=self.root,
                                  review=self.root/"review.json" if review else None,
                                  profiler=self.root/"profiler.json" if profiler else None)

    def result(self, **kwargs):
        return analysis.compare(self.options(**kwargs))

    def mutate(self, file, fn):
        path=self.root/file
        value=json.loads(path.read_bytes());fn(value);save(path,value)

    def test_threshold_equal_pass_strictly_above_regression(self):
        self.make(right=110)
        self.assertEqual(self.result()["status"],"pass")
        self.make(right=111)
        self.assertEqual(self.result()["status"],"regression")
        self.make(right=80)
        self.assertEqual(self.result()["cases"][0]["performance"]["direction"],"faster")

    def test_confidence_overlap_is_inconclusive(self):
        self.make()
        for i in range(9):
            write_bundle(self.root/f"candidate-{i}",100+i*2,i,True)
        m=analysis.capture(self.root/"candidate-plan.json",self.root/"policy.json",self.root,"candidate")
        save(self.root/"candidate.json",m)
        self.assertEqual(self.result()["cases"][0]["reason"],"threshold_overlaps_confidence_interval")

    def test_portable_thresholds_forbidden_and_descriptive_result(self):
        self.policy["mode"]="portable"
        save(self.root/"policy.json",self.policy)
        with self.assertRaises(ValueError):analysis.policy_file(self.root/"policy.json")
        self.policy["cases"][0]["regression_limit_ppm"]=None
        self.make(right=200)
        self.assertEqual(self.result(review=False)["status"],"characterization")
        with self.assertRaises(ValueError):self.result()

    def test_fake_clock_only_structural_even_with_review(self):
        self.make(clock="fake",right=9999)
        r=self.result(review=False)
        self.assertEqual(r["status"],"inconclusive")
        self.assertIsNone(r["cases"][0]["performance"])
        with self.assertRaises(ValueError):self.result()

    def test_not_run_and_failed_runs_have_no_success_delta(self):
        for status,expected in [("not_run","not_run"),("provider_error","failed")]:
            self.make(status=status)
            r=self.result();self.assertEqual(r["status"],expected)
            self.assertIsNone(r["cases"][0]["performance"])

    def test_all_identity_fields_checked_and_sources_allowed(self):
        self.assertEqual(self.result()["status"],"pass")
        for field,value in [("compiler","other"),("build_flags_sha256","c"*64),
                            ("driver","other"),("thread_policy","other"),("memory_policy","other"),
                            ("kernel","other"),("logical_cpus",4),("source_dirty","true")]:
            with self.subTest(field=field):
                self.make(changes=lambda d,r,s:s["identity"].update({field:value}))
                self.assertEqual(self.result()["status"],"incompatible")

    def test_unknown_required_identity_is_incompatible(self):
        self.make(changes=lambda d,r,s:s["identity"].update(source_commit="unknown"))
        self.assertEqual(self.result()["cases"][0]["reason"],"identity_incomplete")

    def test_workload_and_counter_inventory_mismatch(self):
        self.make(changes=lambda d,r,s:d.update(configuration="other"))
        self.assertEqual(self.result()["cases"][0]["reason"],"descriptor_mismatch")
        self.policy["cases"][0]["invariant_counters"]=["missing"]
        self.make();self.assertEqual(self.result()["cases"][0]["reason"],"missing_counter")

    def test_independent_counter_and_checksum_mismatch(self):
        for mutation in [lambda d,r,s:r["samples"][0]["counters"].update(operations=17),
                         lambda d,r,s:r["samples"][0].update(checksum=99)]:
            self.make(changes=mutation)
            self.assertEqual(self.result()["cases"][0]["reason"],"correctness_or_counter_mismatch")

    def test_within_run_outlier_not_trimmed(self):
        self.make(right=[100,100,100,100,1000])
        r=self.result();self.assertEqual(r["status"],"inconclusive")
        self.assertEqual(r["cases"][0]["reason"],"noise_or_outliers")
        self.assertEqual(r["cases"][0]["candidate"][0]["statistics"]["max_ns"],1000)
        self.assertEqual(r["cases"][0]["noise"]["within_runs"][-1]["outliers"],1)

    def test_mad_zero_denominator_and_insufficient_samples(self):
        self.make(right=0)
        self.assertEqual(self.result()["status"],"inconclusive")
        self.make(right=[60,80,100,120,140])
        self.assertEqual(self.result()["cases"][0]["reason"],"noise_or_outliers")
        self.make(n=8)
        self.assertEqual(self.result()["cases"][0]["reason"],"insufficient_runs_or_samples")
        self.policy["minimum_samples"]=6;self.make()
        self.assertEqual(self.result()["cases"][0]["reason"],"insufficient_runs_or_samples")

    def test_missing_case_and_family_accounting(self):
        self.policy["cases"].append(dict(provider_id="synthetic.analysis",case_id="absent",invariant_counters=[],regression_limit_ppm=0))
        self.make();r=self.result()
        self.assertEqual(r["status"],"incompatible")
        self.assertEqual(len(r["cases"]),2)
        self.assertEqual(r["cases"][1]["performance"]["family_cases"],2)

    def test_duplicate_runs_unknown_fields_and_mutation_rejected(self):
        for file,mutation in [("candidate.json",lambda v:v["runs"].append(v["runs"][0])),
                              ("candidate.json",lambda v:v.update(extra=1)),
                              ("policy.json",lambda v:v.update(minimum_runs=True)),
                              ("candidate.json",lambda v:v["runs"][0].update(bundle="../outside"))]:
            self.make();self.mutate(file,mutation)
            with self.subTest(file=file),self.assertRaises(ValueError):self.result()
        self.make();p=self.root/"candidate-0/raw.json";p.write_bytes(p.read_bytes()+b" ")
        with self.assertRaises(ValueError):self.result()

    def test_policy_change_invalidates_manifest_and_review(self):
        self.mutate("policy.json",lambda v:v.update(max_mad_ppm=123456))
        with self.assertRaises(ValueError):self.result()
        self.make();self.mutate("review.json",lambda v:v.update(policy_sha256="0"*64))
        with self.assertRaises(ValueError):self.result()

    def test_review_proposal_never_approves_and_chronology(self):
        proposal=analysis.review_proposal(self.root/"baseline.json",self.root/"policy.json")
        self.assertEqual(proposal["decision"],"proposal_only")
        save(self.root/"review.json",proposal)
        self.assertEqual(self.result()["cases"][0]["reason"],"threshold_review_required")
        for value in ["2025-12-31T00:00:00Z","2026-01-03T00:00:00Z","not_available"]:
            self.make();self.mutate("review.json",lambda v:v.update(approved_utc=value))
            with self.subTest(value=value),self.assertRaises(ValueError):self.result()
        self.make();self.assertEqual(self.result(review=False)["status"],"inconclusive")

    def test_collection_change_and_independence(self):
        self.mutate("candidate.json",lambda v:v["collection"].update(frequency_policy="changed"))
        self.assertEqual(self.result(review=False)["status"],"incompatible")
        with self.assertRaises(ValueError):self.result()
        self.controls["independence"]="not_available";self.make()
        self.assertEqual(self.result(review=False)["cases"][0]["reason"],"independent_runs_not_declared")

    def test_collection_host_binding_and_reused_evidence(self):
        self.make(changes=lambda d,r,s:s["identity"].update(host_label="other"))
        with self.assertRaises(ValueError):self.result()
        self.make()
        for p in (self.root/"candidate-0").iterdir():
            shutil.copyfile(p,self.root/"candidate-1"/p.name)
        m=analysis.capture(self.root/"candidate-plan.json",self.root/"policy.json",self.root,"candidate")
        save(self.root/"candidate.json",m)
        self.assertEqual(self.result()["cases"][0]["reason"],"reused_run_evidence")

    def test_failed_run_takes_precedence_over_not_run(self):
        self.make(status="not_run")
        write_bundle(self.root/"candidate-1",trial=1,candidate=True,status="provider_error")
        m=analysis.capture(self.root/"candidate-plan.json",self.root/"policy.json",self.root,"candidate")
        save(self.root/"candidate.json",m)
        self.assertEqual(self.result()["status"],"failed")

    def test_mixed_sources_and_future_runs_rejected(self):
        write_bundle(self.root/"candidate-1",trial=1,candidate=True,
                     changes=lambda d,r,s:s["identity"].update(source_commit="5"*40))
        m=analysis.capture(self.root/"candidate-plan.json",self.root/"policy.json",self.root,"candidate")
        save(self.root/"candidate.json",m)
        self.assertEqual(self.result()["cases"][0]["reason"],"mixed_source_population")
        self.mutate("candidate.json",lambda v:v.update(created_utc="2025-01-01T00:00:00Z"))
        with self.assertRaises(ValueError):self.result()

    def test_between_run_noise_and_input_budget(self):
        write_bundle(self.root/"candidate-1",1000,trial=1,candidate=True)
        m=analysis.capture(self.root/"candidate-plan.json",self.root/"policy.json",self.root,"candidate")
        save(self.root/"candidate.json",m)
        self.assertEqual(self.result()["cases"][0]["reason"],"noise_or_outliers")
        entry=m["runs"][0]
        with self.assertRaises(ValueError):analysis.load_bundle(self.root,entry,[analysis.MAX_TOTAL_BYTES])

    def test_profiler_exact_context_and_clock_relationship(self):
        r=json.loads((self.root/"candidate-0/result.json").read_bytes())
        link=dict(provider_id="synthetic.analysis",case_id="integer",result_file_sha256=analysis.digest((self.root/"candidate-0/result.json").read_bytes()),
                  run_context_sha256=r["run_context_sha256"],tool="synthetic-profiler",tool_version="1",trace_sha256="d"*64,
                  first_sample=0,last_sample=4,clock_domain="test",relationship="uncorrelated",correlation_record_sha256=None)
        value=dict(schema_version=1,kind="profiler_correlations",links=[link]);save(self.root/"profiler.json",value)
        self.assertEqual(self.result(profiler=True)["profiler_links"],[link])
        for name,item in [("last_sample",5),("run_context_sha256","e"*64),("relationship","same_clock"),("case_id","other")]:
            changed=copy.deepcopy(value);changed["links"][0][name]=item;save(self.root/"profiler.json",changed)
            with self.subTest(name=name),self.assertRaises(ValueError):self.result(profiler=True)

    def test_cli_report_deterministic_exit_and_destination_preservation(self):
        self.make(right=111)
        command=[sys.executable,str(ROOT/"tools/compare_benchmarks.py"),"compare"]
        for name,value in vars(self.options()).items():
            if value is not None:command += ["--"+name.replace("_","-"),str(value)]
        for name in ("a.json","b.json"):
            run=subprocess.run(command+["--output",str(self.root/name)],capture_output=True,timeout=30)
            self.assertEqual(run.returncode,1,run.stderr)
        self.assertEqual((self.root/"a.json").read_bytes(),(self.root/"b.json").read_bytes())
        before=(self.root/"a.json").read_bytes()
        self.assertEqual(subprocess.run(command+["--output",str(self.root/"a.json")],capture_output=True,timeout=30).returncode,2)
        self.assertEqual(before,(self.root/"a.json").read_bytes())
        self.assertEqual(subprocess.run(command+["--format","markdown","--output",str(self.root/"report.md")],capture_output=True,timeout=30).returncode,1)
        self.assertIn("reviewed_threshold_exceeded",(self.root/"report.md").read_text())


class TemplateCli(unittest.TestCase):
    @unittest.skipUnless(TEMPLATE,"template CLI supplied by CTest/package integration")
    def test_actual_template_capture_compare_and_unchanged_validator(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve()
            for name in ("trial-01","candidate"):
                subprocess.run([str(TEMPLATE),"--clock","fake","--output",str(root/name)],check=True,timeout=30)
                result=analysis.artifact.validate_bundle(root/name)
                self.assertEqual(result["statistics"]["counter_totals"],{"elements":320,"written_bytes":2560})
            policy=ROOT/"bench/analysis/portable-policy.json"
            for role,bundle in [("baseline","trial-01"),("candidate","candidate")]:
                plan=json.loads((ROOT/"bench/analysis/example-plan.json").read_bytes());plan["runs"][0]["bundle"]=bundle
                save(root/"plan.json",plan)
                command=[sys.executable,str(ROOT/"tools/compare_benchmarks.py"),"capture","--role",role,"--policy",str(policy),
                         "--plan",str(root/"plan.json"),"--run-root",str(root),"--output",str(root/f"{role}.json")]
                subprocess.run(command,check=True,timeout=30)
            options=argparse.Namespace(policy=policy,baseline=root/"baseline.json",candidate=root/"candidate.json",
                                       baseline_root=root,candidate_root=root,review=None,profiler=None)
            self.assertEqual(analysis.compare(options)["status"],"structural_match")
            before=(root/"candidate/result.json").read_bytes()
            self.assertEqual(subprocess.run([str(TEMPLATE),"--clock","fake","--output",str(root/"candidate")],timeout=30).returncode,2)
            self.assertEqual(before,(root/"candidate/result.json").read_bytes())
            for arguments in [["--clock","other"],["--case","missing"],["--clock","fake","--clock","steady"]]:
                self.assertEqual(subprocess.run([str(TEMPLATE),*arguments,"--output",str(root/"invalid")],timeout=30).returncode,2)
                self.assertFalse((root/"invalid").exists())
            subprocess.run([str(TEMPLATE),"--case","transform-1024","--clock","steady","--output",str(root/"steady")],check=True,timeout=30)
            self.assertEqual(analysis.artifact.validate_bundle(root/"steady")["statistics"]["counter_totals"]["elements"],5120)


if __name__=="__main__":
    unittest.main(argv=[sys.argv[0],*remaining])
