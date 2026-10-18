#!/usr/bin/env python3
"""Tests of scan reporting and history preservation using synthetic local data."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "tools" / "release_audit.py"
SPEC = importlib.util.spec_from_file_location("release_audit", SOURCE)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class ReleaseAuditTests(unittest.TestCase):
    def test_secret_patterns_and_no_values_in_report(self):
        token = ("gh" + "p_" + "a" * 36).encode()
        self.assertIn("github_token", AUDIT.inspect(token))
        assignment = ("pass" + "word = 'synthetic-value'").encode()
        self.assertIn("literal_credential_assignment", AUDIT.inspect(assignment))
        self.assertEqual(AUDIT.inspect(b"src/engine.cpp"), [])

    def test_personal_and_relative_paths(self):
        personal = ("C:" + "/" + "Users/" + "example/project").encode()
        self.assertIn("personal_absolute_path", AUDIT.inspect(personal))
        self.assertEqual(AUDIT.inspect(b"build/release/apex_cli; <repo>/tests"), [])

    def test_url_credential(self):
        url = ("https" + "://person:" + "value@example.invalid/repository").encode()
        self.assertIn("credential_in_url", AUDIT.inspect(url))

    def test_history_is_scanned_without_rewrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.run(["git", "-C", str(root), *args], capture_output=True, check=True).stdout
            git("init")
            token = "gh" + "p_" + "b" * 36
            (root / "sample.txt").write_text(token)
            git("add", "sample.txt")
            git("-c", "user.name=Audit test", "-c", "user.email=test@example.invalid",
                "commit", "-m", "synthetic fixture")
            revision = git("rev-parse", "HEAD")
            (root / "sample.txt").write_text("sanitized fixture\n")
            report = AUDIT.scan(root, True)
            self.assertEqual(report["current_findings"], [])
            self.assertTrue(any("github_token" in row["categories"] for row in report["history_findings"]))
            self.assertNotIn(token, str(report))
            self.assertEqual(git("rev-parse", "HEAD"), revision)

    def test_build_artifact_and_large_file_are_flagged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "-C", str(root), "init"], capture_output=True, check=True)
            (root / "program.exe").write_bytes(b"fixture")
            (root / "large.txt").write_bytes(b"x" * (1024 * 1024 + 1))
            (root / "credentials.json").write_text("{}")
            rows = {row["path"]: row["categories"] for row in AUDIT.scan(root, False)["current_findings"]}
            self.assertIn("build_or_archive_artifact", rows["program.exe"])
            self.assertIn("large_file_over_1_MiB", rows["large.txt"])
            self.assertIn("sensitive_filename", rows["credentials.json"])


if __name__ == "__main__":
    unittest.main()
