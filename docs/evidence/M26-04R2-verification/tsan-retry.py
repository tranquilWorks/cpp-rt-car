from pathlib import Path
import subprocess,json
out=Path('/tmp/cpp-m26-04r2-tsan-attempts');out.mkdir(exist_ok=True)
records=[]
for i in range(30):
 p=subprocess.run(['/tmp/cpp-m26-04r2-tsan-tests'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 data=p.stdout;(out/f'attempt-{i+1:02}.log').write_bytes(data);records.append({'attempt':i+1,'exit':p.returncode,'mapping_only':data.startswith(b'FATAL: ThreadSanitizer: unexpected memory mapping')})
 (out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
 if p.returncode==0:
  Path('/tmp/cpp-m26-04r2-tsan-passed.log').write_bytes(data);print('PASS attempt',i+1);break
 if not records[-1]['mapping_only']:raise SystemExit(f'Functional/non-mapping failure, exit {p.returncode}: {data[-1500:]!r}')
else:raise SystemExit('All attempts failed before tests with mapping error')
