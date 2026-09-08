#!/usr/bin/env python3
"""Offline schema/publication/adversarial-artifact and CLI integration tests."""
from __future__ import annotations
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("benchmark_validator", ROOT / "tools/check_benchmark_artifact.py")
validator = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(validator)
parser = argparse.ArgumentParser()
parser.add_argument("--cli", type=Path, required=True)
parser.add_argument("--consumer", type=Path, required=True)
args, remaining = parser.parse_known_args()
CLI, CONSUMER = args.cli.resolve(), args.consumer.resolve()

class Artifacts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="rtfw-m23-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.good = self.root / "original"
        subprocess.run([str(CONSUMER), str(self.good)], check=True, timeout=30)
        self.baseline = {p.name: p.read_bytes() for p in self.good.iterdir()}

    def fixture(self, changes=None):
        path = self.root / "candidate"
        if path.exists():
            shutil.rmtree(path)
        path.mkdir()
        for name, data in {**self.baseline, **(changes or {})}.items():
            (path / name).write_bytes(data)
        return path

    def signed(self, d=None, raw=None, result=None):
        # Recompute every integrity field to test semantics, not just hashes.
        d = copy.deepcopy(d if d is not None else json.loads(self.baseline["descriptor.json"]))
        raw = copy.deepcopy(raw if raw is not None else json.loads(self.baseline["raw.json"]))
        result = copy.deepcopy(result if result is not None else json.loads(self.baseline["result.json"]))
        db = validator.canonical(d)
        dh = validator.digest(db)
        result["descriptor_sha256"] = raw["descriptor_sha256"] = dh
        context = {"descriptor_sha256": dh, "identity": result["identity"], "start_utc": result["start_utc"]}
        result["run_context_sha256"] = raw["run_context_sha256"] = validator.digest(validator.canonical(context)[:-1])
        rb = validator.canonical(raw)
        result["raw_sha256"] = validator.digest(rb)
        result["result_sha256"] = validator.digest(validator.canonical({k:v for k,v in result.items() if k!="result_sha256"}))
        return self.fixture({"descriptor.json":db, "raw.json":rb, "result.json":validator.canonical(result)})

    def test_exact_deterministic_external_provider(self):
        result = validator.validate_bundle(self.good)
        self.assertEqual(result["statistics"]["total_ns"], 50)
        self.assertEqual(result["statistics"]["p99_ns"], 10)
        second = self.root / "second"
        subprocess.run([str(CONSUMER), str(second)], check=True, timeout=30)
        self.assertEqual(self.baseline, {p.name:p.read_bytes() for p in second.iterdir()})

    def test_raw_lexical_and_canonical_rejections(self):
        data=self.baseline["raw.json"]
        cases=[data[:-2], data+b" ", data+b"{}", b"{\"a\":0,\"a\":1}\n", b"[NaN]\n",
               b"[Infinity]\n", b"[1.0]\n", b"[9223372036854775808]\n", b"[true]\n",
               b"["*33+b"0"+b"]"*33+b"\n", b"\xff", b"{}\x00\n", b"{}\r\n",
               json.dumps(json.loads(data),indent=2).encode()+b"\n", b" "*(validator.MAX_BYTES+1)]
        for value in cases:
            with self.subTest(value=value[:20]):
                with self.assertRaises((ValueError,UnicodeError,RecursionError)):
                    validator.validate_bundle(self.fixture({"raw.json":value}))

    def test_missing_unknown_and_duplicate_fields(self):
        d=json.loads(self.baseline["descriptor.json"])
        for name in d:
            candidate=copy.deepcopy(d); del candidate[name]
            with self.subTest(name=name), self.assertRaises(ValueError):
                validator.validate_bundle(self.fixture({"descriptor.json":validator.canonical(candidate)}))
        result=json.loads(self.baseline["result.json"])
        for name in result:
            candidate=copy.deepcopy(result); del candidate[name]
            with self.subTest(name=name), self.assertRaises(ValueError):
                validator.validate_bundle(self.fixture({"result.json":validator.canonical(candidate)}))
        candidate=copy.deepcopy(d); candidate["unknown"]=1
        with self.assertRaises(ValueError): validator.validate_bundle(self.signed(d=candidate))
        candidate=copy.deepcopy(d); candidate["counters"]*=2
        with self.assertRaises(ValueError): validator.validate_bundle(self.signed(d=candidate))

    def test_rebound_semantic_mutations_fail(self):
        d=json.loads(self.baseline["descriptor.json"])
        for name, value in [("schema_version",2),("provider_id","../bad"),("case_id","bad\\id"),
                            ("warmup",1001),("repetitions",0),("repetitions",10001),("warmup",True),
                            ("clock","wall"),("raw_policy","none"),("comparison_policy","qualified")]:
            changed=copy.deepcopy(d); changed[name]=value
            with self.subTest(name=name,value=value), self.assertRaises(ValueError):
                validator.validate_bundle(self.signed(d=changed))
        raw=json.loads(self.baseline["raw.json"])
        for name,value in [("index",3),("start_ns",99),("end_ns",0),("duration_ns",999),
                           ("invariants_passed",False),("counters",{"operations":2})]:
            changed=copy.deepcopy(raw); changed["samples"][0][name]=value
            with self.subTest(name=name), self.assertRaises(ValueError):
                validator.validate_bundle(self.signed(raw=changed))
        result=json.loads(self.baseline["result.json"])
        for name,value in [("measured_completed",4),("warmup_completed",0),("clock","steady"),
                           ("qualification","RT1"),("statistics",None),("raw_file","../raw.json"),
                           ("start_utc","1970-02-31T00:00:00Z"),("diagnostic","secret"),
                           ("evidence_class","controlled_performance")]:
            changed=copy.deepcopy(result); changed[name]=value
            with self.subTest(name=name), self.assertRaises(ValueError):
                validator.validate_bundle(self.signed(result=changed))
        for key in ("hostname","username","environment","serial_number","machine_id","mac_address"):
            changed=copy.deepcopy(result); changed["identity"][key]="private"
            with self.subTest(key=key), self.assertRaises(ValueError):
                validator.validate_bundle(self.signed(result=changed))
        for value in ("/home/private", "api_key=private", "user@host", "C:\\Users\\private"):
            changed=copy.deepcopy(result); changed["identity"]["cpu_model"]=value
            with self.subTest(value=value), self.assertRaises(ValueError):
                validator.validate_bundle(self.signed(result=changed))

    def test_cross_run_and_digest_mutations_fail(self):
        for name,data in self.baseline.items():
            value=bytearray(data); value[len(value)//2]^=1
            with self.subTest(name=name), self.assertRaises((ValueError,UnicodeError)):
                validator.validate_bundle(self.fixture({name:bytes(value)}))
        result=json.loads(self.baseline["result.json"])
        result["identity"]["backend"]="another"
        # Even a rehashed summary cannot silently bind the old raw samples to a new context.
        result["result_sha256"]=validator.digest(validator.canonical({k:v for k,v in result.items() if k!="result_sha256"}))
        with self.assertRaises(ValueError):
            validator.validate_bundle(self.fixture({"result.json":validator.canonical(result)}))

    def test_missing_unlisted_and_symlink_files(self):
        for name in self.baseline:
            path=self.fixture(); (path/name).unlink()
            with self.subTest(name=name), self.assertRaises(ValueError): validator.validate_bundle(path)
        path=self.fixture(); (path/"extra").write_text("unlisted")
        with self.assertRaises(ValueError): validator.validate_bundle(path)
        path=self.fixture(); (path/"raw.json").unlink()
        try:
            (path/"raw.json").symlink_to(self.good/"raw.json")
        except OSError:
            self.skipTest("symlink privilege unavailable")
        with self.assertRaises((ValueError,OSError)): validator.validate_bundle(path)
        link=self.root/"link"; link.symlink_to(self.good,target_is_directory=True)
        with self.assertRaises((ValueError,OSError)): validator.validate_bundle(link)

    def test_all_schema_keyword_constraints(self):
        schema=validator.decode((ROOT/"bench/schemas/descriptor.schema.json").read_bytes(),canonical_required=False)
        value=json.loads(self.baseline["descriptor.json"])
        validator.validate_schema(value,schema)
        with self.assertRaises(ValueError): validator.validate_schema(value,{**schema,"unsupported":True})
        for v,s in [(False,{"type":"integer"}),(1.0,{"type":"integer"}),(-1,{"type":"integer","minimum":0}),
                    ("x",{"type":"string","pattern":"^[a-z]{2}$"}),([],{"type":"array","minItems":1}),
                    ({"bad":1},{"type":"object","additionalProperties":False})]:
            with self.subTest(v=v), self.assertRaises(ValueError): validator.validate_schema(v,s)

    def test_failed_and_not_run_artifacts_never_summarize_success(self):
        raw=json.loads(self.baseline["raw.json"]); raw["samples"]=[]
        result=json.loads(self.baseline["result.json"])
        result.update(status="not_run",warmup_completed=0,measured_completed=0,
                      statistics=None,diagnostic="prerequisite unavailable")
        path=self.signed(raw=raw,result=result)
        self.assertEqual(validator.validate_bundle(path)["status"],"not_run")
        result.update(status="provider_error",diagnostic="provider callback threw")
        self.assertEqual(validator.validate_bundle(self.signed(raw=raw,result=result))["statistics"],None)
        result["diagnostic"]="private data"
        with self.assertRaises(ValueError): validator.validate_bundle(self.signed(raw=raw,result=result))

    def test_cli_selection_parse_and_no_overwrite(self):
        base=[str(CLI),"run","--provider","rtfw.self","--case","structural"]
        for extra in [[],["--output",str(self.good)],["--output",str(self.root/"unused"),"--clock","wall"],
                      ["--output",str(self.root/"unused"),"--case","duplicate"],["--unknown","x"]]:
            proc=subprocess.run(base+extra,capture_output=True,timeout=30)
            self.assertNotEqual(proc.returncode,0)
            self.assertFalse((self.root/"unused").exists())
            self.assertEqual(self.baseline,{p.name:p.read_bytes() for p in self.good.iterdir()})
        proc=subprocess.run([str(CLI),"list"],capture_output=True,text=True,check=True,timeout=30)
        catalog=json.loads((ROOT/"bench/fixtures/cpu_cases.json").read_text(encoding="utf-8"))
        runtime_catalog=json.loads((ROOT/"bench/fixtures/runtime_cases.json").read_text(encoding="utf-8"))
        expected=["rtfw.self:structural"]+["rtfw.cpu:"+case["id"] for case in catalog["cases"]]+["rtfw.runtime:"+case["id"] for case in runtime_catalog["cases"]]
        self.assertEqual(proc.stdout,"".join(name+"\n" for name in sorted(expected)))
        for clock in ("fake","steady"):
            out=self.root/clock
            subprocess.run(base+["--clock",clock,"--output",str(out)],check=True,capture_output=True,timeout=30)
            result=validator.validate_bundle(out)
            self.assertEqual(result["status"],"ok")
            self.assertEqual(result["evidence_class"],"structural_fixture" if clock=="fake" else "portable_characterization")

if __name__=="__main__":
    unittest.main(argv=[sys.argv[0]]+remaining,verbosity=2)
