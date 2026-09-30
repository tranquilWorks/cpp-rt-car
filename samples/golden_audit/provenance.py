"""Configure-time identity of the source-only audit runner."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def generate(root, output):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True,
                                       stderr=subprocess.DEVNULL).strip()
    try:
        if Path(git('rev-parse', '--show-toplevel')).resolve() != root.resolve():
            raise ValueError('foreign parent repository')
        commit, tree, dirty = git('rev-parse', 'HEAD'), git('rev-parse', 'HEAD^{tree}'), bool(git('status', '--porcelain'))
    except (OSError, ValueError, subprocess.CalledProcessError):
        commit, tree, dirty = None, None, True
    folder = Path(__file__).resolve().parent
    result = dict(schema=1, source_commit=commit, source_tree=tree, dirty=dirty,
                  files={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(folder.iterdir()) if p.is_file() and p.name != 'provenance.json'})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    generate(args.root, args.output)
