"""Verify complete M29 mapping without changing the immutable M28 source ledger."""
from pathlib import Path
import copy,hashlib,json,sys
root=Path(sys.argv[1]);raw=(root/'docs/closeout/ledger.json').read_bytes();old=json.loads(raw)
current=json.loads((root/'docs/closeout/m29-current.json').read_text())
def validate(value):
 assert value['historical_ledger_sha256']==hashlib.sha256(raw).hexdigest()
 assert value['claims']=={'software_complete':False,'CAP-M20_complete':False,'physical_or_release_qualification':False}
 assert value['existing_cards']==old['existing_cards'] and len(value['existing_cards'])==13
 assert len(value['findings'])==30 and len(value['requirements'])==112
 assert {x['id'] for x in value['findings']}=={x['id'] for x in old['findings']}
 assert {x['id'] for x in value['requirements']}=={x['id'] for x in old['requirements']}
 index={x['id']:x for x in value['findings']}
 for finding in old['findings']:
  new=index[finding['id']]
  assert new['requirements']==finding['requirements'] and new['cards']==finding['cards']
  assert new['historical_status']==finding['status']
  if finding['status']=='external_acceptance':assert new['current_disposition']=='external_acceptance'
 for requirement in value['requirements']:
  ids=[x['id'] for x in value['findings'] if requirement['id'] in x['requirements']]
  assert requirement['findings']==ids
  assert requirement['dispositions']=={i:index[i]['current_disposition'] for i in ids}
 for identity in ['excluded-generated-campaigns','excluded-signature-fixtures']:
  assert index[identity]['current_disposition']=='planned_M29-03'
validate(current)
for mode in range(4):
 bad=copy.deepcopy(current)
 if mode==0:bad['findings'].pop()
 elif mode==1:bad['requirements'][0]['findings']=[]
 elif mode==2:bad['claims']['software_complete']=True
 else:bad['existing_cards'].pop()
 try:validate(bad)
 except AssertionError:pass
 else:raise AssertionError('incomplete or overclaimed overlay accepted')
print('PASS all112 requirements,30 findings,13 unchanged original cards and omission/overclaim controls')
