"""Fresh owned CTest output; retain all bytes when a run fails."""
import argparse
from pathlib import Path
import tempfile
import run
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--provenance',type=Path,required=True);p.add_argument('--config');args=p.parse_args()
directory=Path(tempfile.mkdtemp(prefix='golden showcase evidence ',dir=args.build))/'run'
print('evidence:',directory,flush=True)
run.execute(args.build.resolve(),directory,args.provenance.resolve(),'fake',args.config)
