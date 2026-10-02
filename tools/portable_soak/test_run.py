import json
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
import run

class JournalTests(unittest.TestCase):
    def execute(self, source, cycles=2, seconds=3):
        folder=Path(self.temp.name)
        fixture=folder/'fixture.py'; fixture.write_text(source, encoding='utf-8')
        output=folder/'result'
        value=run.run([sys.executable, fixture], cycles, seconds, output)
        records=[json.loads(line) for line in (output/'journal.jsonl').read_text().splitlines()]
        self.assertEqual(records[0]['status'], 'started')
        self.assertEqual(records[-1], value)
        self.assertEqual(json.loads((output/'report.json').read_text()), value)
        return value, output
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
    def test_actual_success_and_existing_output_refusal(self):
        source='import json\n'+''.join('print('+repr(json.dumps(dict(run.FIELDS,cycle=i)))+')\n' for i in (1,2))
        value, output=self.execute(source)
        self.assertEqual(value['status'],'passed')
        self.assertEqual(value['totals']['frames'],386)
        self.assertEqual(value['totals']['acquired'],value['totals']['released'])
        with self.assertRaises(FileExistsError): run.run([sys.executable],2,1,output)
    def test_nonzero_retains_complete_diagnostic(self):
        value,output=self.execute('import sys\nprint("ordinary fixture refusal",file=sys.stderr)\nsys.exit(7)\n')
        self.assertEqual(value['status'],'failed');self.assertEqual(value['returncode'],7)
        self.assertIn('ordinary fixture refusal',(output/'stderr.log').read_text())
    def test_zero_exit_missing_rows_is_not_success(self):
        value,_=self.execute('pass\n');self.assertEqual(value['status'],'invalid_output')
    def test_unbalanced_resource_record_is_not_success(self):
        value,_=self.execute('print('+repr(json.dumps(dict(run.FIELDS,cycle=1,released=5)))+')\n',cycles=1)
        self.assertEqual(value['status'],'invalid_output')
    def test_timeout_reaps_child(self):
        value,_=self.execute('import time\ntime.sleep(30)\n',seconds=.1)
        self.assertEqual(value['status'],'timeout');self.assertIsNotNone(value['returncode'])
        self.assertLess(value['elapsed_wall_seconds'],10)
    @unittest.skipIf(sys.platform=='win32','POSIX signal delivery fixture')
    def test_interrupt_has_terminal_journal(self):
        root=Path(self.temp.name); fixture=root/'interrupted.py'; output=root/'result'
        fixture.write_text('import sys\nsys.path.insert(0,'+repr(str(Path(run.__file__).parent))+')\nimport run\nrun.run([sys.executable,"-c","import time; time.sleep(30)"],2,20,'+repr(str(output))+')\n')
        child=subprocess.Popen([sys.executable,str(fixture)])
        try:
            for _ in range(200):
                if (output/'stdout.log').exists():break
                time.sleep(.01)
            else:self.fail('fixture did not start')
            time.sleep(.05);child.send_signal(signal.SIGINT);child.wait(timeout=10)
            value=json.loads((output/'report.json').read_text())
            self.assertEqual(value['status'],'interrupted');self.assertIsNotNone(value['returncode'])
        finally:
            if child.poll() is None:child.kill();child.wait()
if __name__=='__main__':unittest.main()
