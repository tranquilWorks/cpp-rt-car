#!/usr/bin/env python3
"""Exercise the real portable CLI, including malformed numeric inputs."""
import argparse
import subprocess
import sys
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--cli", required=True)
args, remaining = parser.parse_known_args()


class Cli(unittest.TestCase):
    def invoke(self, *arguments):
        return subprocess.run([args.cli, *arguments], capture_output=True, text=True, timeout=30)

    def test_help(self):
        result = self.invoke("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("usage:", result.stdout)
        self.assertNotIn("validation=pass", result.stdout)

    def test_invalid(self):
        for arguments in [["--wat", "1"], ["--count"], ["--count", "0"],
                          ["--count", "4097"], ["--steps", "0"], ["--steps", "1025"],
                          ["--seed", "4294967296"], ["--seed", "-1"], ["--seed", "1x"],
                          ["--seed", ""], ["--workers", "3"], ["--count", "1", "--count", "2"]]:
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("validation=pass", result.stdout)

    @unittest.skipUnless(sys.platform == "linux", "Linux process-stack contract, matching M23")
    def test_bounded_caller_stack(self):
        import resource

        def bounded():
            _, hard = resource.getrlimit(resource.RLIMIT_STACK)
            resource.setrlimit(resource.RLIMIT_STACK, (512*1024, hard))

        result = subprocess.run([args.cli, "--count", "17", "--steps", "7", "--seed", "0"],
                                capture_output=True, text=True, timeout=30, preexec_fn=bounded)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("completed=7 validation=pass", result.stdout)

    def test_repeat_and_maximum(self):
        options = ["--count", "4096", "--steps", "1024", "--seed", "4294967295"]
        first = self.invoke(*options)
        second = self.invoke(*options)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.stdout, second.stdout)
        self.assertIn("completed=1024 validation=pass evidence=simulated-driver-protocol", first.stdout)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
