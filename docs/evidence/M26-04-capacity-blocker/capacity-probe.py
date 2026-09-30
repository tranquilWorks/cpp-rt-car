from pathlib import Path
import subprocess,shutil,json,hashlib
repo=Path.cwd();root=Path('/tmp/cpp-m26-04-capacity-probe');root.mkdir(exist_ok=True)
for sibling in ['golden_system','golden_cuda']:
 p=root/sibling
 if not p.exists():p.symlink_to(repo/'samples'/sibling,target_is_directory=True)
kit=root/'golden_xdma';shutil.copytree(repo/'samples/golden_xdma',kit,dirs_exist_ok=True)
source=(repo/'samples/golden_cuda/physics.hpp').read_text().replace('#include "simulated_driver.hpp"','#include "../golden_cuda/simulated_driver.hpp"').replace('CudaPhysics','CapacityProbePhysics')
owner=(kit/'owner.hpp').read_text().replace('#include "../golden_cuda/physics.hpp"','#include "capacity_probe_physics.hpp"').replace('cuda::CudaPhysics','cuda::CapacityProbePhysics')
(kit/'owner.hpp').write_text(owner)
main=root/'main.cpp';main.write_text('''#include "golden_xdma/owner.hpp"
#include <cstdio>
int main(){golden::Options o;auto p=std::make_unique<golden::xdma::Owner>(o,golden::xdma::Dispatch::graph);auto s=p->prepare();std::printf("prepare=%d error=%.*s xdma_initializes=%llu xdma_events=%llu\\n",int(s),int(p->session->runtime->last_error().size()),p->session->runtime->last_error().data(),(unsigned long long)p->driver->initializes.load(),(unsigned long long)p->driver->events.load()); if(p->close()!=rt::Status::ok)return 2;return s==rt::Status::ok?0:1;}
''')
original_io=(repo/'samples/golden_xdma/io.hpp').read_text();rows=[]
for capacity in [1,2,3]:
 (kit/'capacity_probe_physics.hpp').write_text(source.replace('c.queue_capacity = 1;',f'c.queue_capacity = {capacity};'))
 io=original_io.replace('c.device_outstanding_capacity = combined ? 3 : 2;',f'c.device_outstanding_capacity = {capacity};').replace('c.device_completion_batch = combined ? 3 : 2;',f'c.device_completion_batch = {capacity};')
 if capacity==3:io=io.replace('c.queue_capacity = 2;','c.queue_capacity = 3;')
 (kit/'io.hpp').write_text(io)
 binary=root/f'capacity-{capacity}';cmd=['g++','-std=c++20','-O2','-pthread','-Irt/include','-Icore/include','-Iinclude',str(main),'build/agent-quick/librtfw_xdma_backend.a','build/agent-quick/librtfw_cuda_backend.a','build/agent-quick/librtfw_runtime.a','-o',str(binary)]
 built=subprocess.run(cmd,capture_output=True,text=True);(root/f'build-{capacity}.log').write_text(built.stdout+built.stderr);assert built.returncode==0,built.stderr
 run=subprocess.run([str(binary)],capture_output=True,text=True);row={'runtime_capacity':capacity,'cuda_native_slots':capacity,'xdma_native_slots':3 if capacity==3 else 2,'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr,'diagnostic_only':True};rows.append(row);print(row,flush=True)
 snapshot=root/f'source-{capacity}';shutil.copytree(kit,snapshot,dirs_exist_ok=True)
(root/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
