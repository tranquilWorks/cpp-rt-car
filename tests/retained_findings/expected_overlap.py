"""Require the original overlap assertion to fail under a controlled host delay."""
import subprocess,sys
p=subprocess.run([sys.argv[1],'--gtest_filter=GPUStub.Overlap'],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
print(p.stdout,end='')
assert p.returncode==1,p.returncode
assert 'overlap.count()' in p.stdout and '[  FAILED  ] GPUStub.Overlap' in p.stdout
print('PASS controlled coordinator delay fails the unchanged overlap oracle; no clock repair claim')
