from pathlib import Path
import os
import subprocess
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
VERIFY = REPO_ROOT / "tools" / "verify.sh"


class VerifyCliTests(unittest.TestCase):
    def run_verify(self, *arguments: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        command_env = os.environ.copy()
        if env is not None:
            command_env.update(env)
        return subprocess.run(
            [str(VERIFY), *arguments],
            cwd=REPO_ROOT,
            env=command_env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_help_is_successful(self) -> None:
        result = self.run_verify("--help")
        self.assertEqual(0, result.returncode)
        self.assertIn("fast|full", result.stdout)

    def test_missing_mode_is_rejected(self) -> None:
        result = self.run_verify()
        self.assertEqual(2, result.returncode)
        self.assertIn("Usage:", result.stderr)

    def test_unknown_mode_is_rejected(self) -> None:
        result = self.run_verify("quick")
        self.assertEqual(2, result.returncode)
        self.assertIn("Unknown verification mode", result.stderr)

    def test_source_directory_override_is_rejected(self) -> None:
        result = self.run_verify("fast", env={"OS1_BUILD_DIR": str(REPO_ROOT / "src")})
        self.assertEqual(2, result.returncode)
        self.assertIn("outside the repository", result.stderr)

    def test_repository_metadata_override_is_rejected(self) -> None:
        result = self.run_verify("fast", env={"OS1_BUILD_DIR": str(REPO_ROOT / ".git/cache")})
        self.assertEqual(2, result.returncode)
        self.assertIn("outside the repository", result.stderr)

    def test_shared_host_and_cross_directory_is_rejected(self) -> None:
        shared = str(REPO_ROOT / "build-shared")
        result = self.run_verify(
            "fast",
            env={"OS1_BUILD_DIR": shared, "OS1_HOST_BUILD_DIR": shared},
        )
        self.assertEqual(2, result.returncode)
        self.assertIn("must be different directories", result.stderr)


if __name__ == "__main__":
    unittest.main()
