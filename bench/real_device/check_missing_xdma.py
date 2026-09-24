#!/usr/bin/env python3
"""Node-free native-host check: missing prerequisites must emit valid NOT RUN."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
p=argparse.ArgumentParser()
p.add_argument('--host',type=Path,required=True)
p.add_argument('--validator',type=Path,required=True)
a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='rtfw-missing-xdma-') as directory:
    root=Path(directory); output=root/'result'
    command=[str(a.host.resolve()),'real-xdma-roundtrip-64',str(output),str(root/'absent-h2c'),str(root/'absent-c2h'),'0','64']
    result=subprocess.run(command,capture_output=True,text=True,timeout=30)
    assert result.returncode==3,(result.returncode,result.stderr)
    result=subprocess.run([sys.executable,str(a.validator.resolve()),'--artifact-root',str(output)],capture_output=True,text=True,timeout=30)
    assert result.returncode==3,(result.returncode,result.stderr)
    before={f.name:f.read_bytes() for f in output.iterdir()}
    assert subprocess.run(command,capture_output=True,timeout=30).returncode==2
    assert before=={f.name:f.read_bytes() for f in output.iterdir()}
print('Native XDMA missing-endpoint and destination-preservation checks passed')
