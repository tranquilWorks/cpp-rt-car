"""Combined proposal parser controls using fabricated test data, never bench evidence."""
from pathlib import Path
import sys,json,hashlib,tempfile,shutil,subprocess
root=Path(sys.argv[1]);sys.path.insert(0,str(root/'tools'));import qualification as q
with tempfile.TemporaryDirectory(prefix='retained proposal ') as temporary:
 w=Path(temporary);artifacts=w/'artifacts';shutil.copytree(root/'tests/qualification_fixtures/artifacts',artifacts)
 values=[json.loads((root/('tests/qualification_fixtures/valid-combined-'+n+'.json')).read_text()) for n in ['campaign-plan','qualification-record','promotion-review']]
 paths=[w/(n+'.json') for n in ['plan','record','review']]
 for value in values:value['evidence_class']='qualification_campaign'
 def write(i):
  paths[i].write_bytes(q._canonical_bytes(values[i]));return hashlib.sha256(paths[i].read_bytes()).hexdigest()
 digest=write(0);values[1]['plan_sha256']=digest;record=write(1);values[2]['plan_sha256']=digest;values[2]['record_sha256']=record;write(2)
 before={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'qualification').rglob('*') if p.is_file()}
 bound=q.validate_set(*paths,artifacts);proposal=q.build_proposal(bound)
 assert proposal['proposal_label']=='proposal_only' and proposal['promotion_action']=='human_matrix_change_required'
 assert proposal['reviewer_authentication']=='attribution_only' and proposal['chronology_proof']=='external_human_verification_only'
 args=['--plan',str(paths[0]),'--record',str(paths[1]),'--review',str(paths[2]),'--artifact-dir',str(artifacts)]
 for mode,tail in [('validate',[]),('propose',['--output',str(w/'proposal.json')]),('verify-proposal',['--proposal',str(w/'proposal.json')])]:
  p=subprocess.run([sys.executable,str(root/'tools/qualification.py'),mode,*args,*tail],capture_output=True,text=True,timeout=30)
  print(p.stdout,end='');assert p.returncode==0,p.stderr
 def refused(token):
  try:q.validate_set(*paths,artifacts)
  except q.ValidationFailure as e:assert any(token in x for x in e.errors),(token,e.errors)
  else:raise AssertionError('invalid evidence accepted: '+token)
 for key,value,token in [('decision','reject','decision'),('pre_run_provenance_verified',False,'pre_run_provenance_verified'),('record_sha256','0'*64,'record_sha256')]:
  prior=values[2][key];values[2][key]=value;write(2);refused(token);values[2][key]=prior;write(2)
 raw=paths[0].read_bytes();paths[0].write_bytes(raw+b' ');refused('plan_sha256');paths[0].write_bytes(raw)
 file=next(p for p in artifacts.rglob('*') if p.is_file());raw=file.read_bytes();file.write_bytes(raw+b'tamper');refused('mismatch');file.write_bytes(raw)
 p=subprocess.run([sys.executable,str(root/'tools/qualification.py'),'propose',*args,'--output',str(w/'proposal.json')],capture_output=True,text=True,timeout=30);assert p.returncode==1
 assert before=={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'qualification').rglob('*') if p.is_file()}
 print('PASS rebound test fixture, CLI, digest/artifact/review refusals, nonoverwrite and unchanged qualification tree; NOT physical evidence')
