"""Generate marked scheduling controls from unchanged original test sources."""
from pathlib import Path
import sys,json,hashlib,difflib
root,out=map(Path,sys.argv[1:]);out.mkdir(parents=True,exist_ok=True)
records=[]
def generated(path,name,edits,prefix=''):
 before=(root/path).read_text();after=before
 for old,new in edits:
  assert after.count(old)==1,(path,old,after.count(old))
  after=after.replace(old,new)
 after=prefix+after
 (out/name).write_text(after)
 (out/(name+'.patch')).write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile=path,tofile='TEST-ONLY/'+name)))
 records.append(dict(source=path,sha256=hashlib.sha256((root/path).read_bytes()).hexdigest(),generated=name,generated_sha256=hashlib.sha256(after.encode()).hexdigest()))
generated('tests/package_consumer/sampled_io_simulation_consumer.cpp','sampled.hpp',[
 ('      if (s.missing_ack)','      schedule_control::native_tail();\n      if (s.missing_ack)'),
 ('      const auto limit =\n          std::chrono::steady_clock::now() + std::chrono::seconds(2);','      schedule_control::gate_entry();\n      const auto limit =\n          std::chrono::steady_clock::now() + std::chrono::seconds(2);'),
 ('      status = f.runtime.start();','      status = f.runtime.start();\n      schedule_control::after_start(status);'),
], '#include "schedule.hpp"\n#define RTFW_SAMPLED_SIMULATION_NO_MAIN\n')
# Keep the original GTest overlap oracle. Parent checks its expected failure.
generated('tests/test_gpu_stub.cpp','overlap.cpp',[
 ('    auto start = simcore::hal::now();\n    pool.drain();','    auto start = simcore::hal::now();\n    while (!fence.is_signaled()) std::this_thread::yield();\n    std::this_thread::sleep_for(150ms); // controlled coordinator descheduling\n    pool.drain();')
])
(out/'sources.json').write_text(json.dumps(records,indent=2)+'\n')
