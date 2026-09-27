"""Exercise the installed conformance entry point, syntax, and process stack."""
import argparse
import subprocess
import sys
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--cli", required=True)
args, remaining = parser.parse_known_args()


class Cli(unittest.TestCase):
    def test_help_and_invalid(self):
        for arguments, code in [(["--help"], 0), (["--invalid"], 2), (["--help", "extra"], 2)]:
            result = subprocess.run([args.cli, *arguments], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, code, result.stderr)
            self.assertNotIn("conformance PASS", result.stdout)

    @unittest.skipUnless(sys.platform == "linux", "Linux CLI process-stack contract")
    def test_bounded_stack(self):
        import resource

        def bounded():
            _, hard = resource.getrlimit(resource.RLIMIT_STACK)
            resource.setrlimit(resource.RLIMIT_STACK, (512 * 1024, hard))

        result = subprocess.run([args.cli], capture_output=True, text=True, timeout=60, preexec_fn=bounded)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CUDA lifetime conformance PASS", result.stdout)
        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
