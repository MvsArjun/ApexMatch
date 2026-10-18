#!/usr/bin/env python3
"""Read-only release scan. Reports locations/categories, never credential values."""
from __future__ import annotations
import argparse
import io
import json
from pathlib import Path
import re
import subprocess

SECRET_PATTERNS = {
    "private_key": re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "github_token": re.compile(rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})"),
    "aws_access_key": re.compile(rb"(?:AKIA|ASIA)[A-Z0-9]{16}"),
    "openai_key": re.compile(rb"sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}"),
    "credential_in_url": re.compile(rb"https?://[^\s/:@]+:[^\s/@]+@"),
}
LOCAL_PATH = re.compile(rb"(?:[A-Za-z]:[\\/]Users[\\/][^\s\\/\"']+|/(?:Users|home)/[^\s/\"']+)")
BUILD_SUFFIXES = {".exe", ".dll", ".obj", ".o", ".a", ".lib", ".pdb", ".etl", ".zip"}


def inspect(data: bytes) -> list[str]:
    categories = [name for name, pattern in SECRET_PATTERNS.items() if pattern.search(data)]
    if LOCAL_PATH.search(data):
        categories.append("personal_absolute_path")
    return categories


def git(root: Path, *arguments: str, data: bytes | None = None) -> bytes:
    return subprocess.run(["git", "-C", str(root), *arguments], input=data,
                          capture_output=True, check=True).stdout


def history_blobs(root: Path):
    objects = git(root, "rev-list", "--objects", "--all").splitlines()
    paths = {}
    for entry in objects:
        parts = entry.split(b" ", 1)
        if len(parts) == 2:
            paths[parts[0]] = parts[1].decode("utf-8", "replace")
    if not paths:
        return
    response = io.BytesIO(git(root, "cat-file", "--batch", data=b"\n".join(paths) + b"\n"))
    for object_id, path in paths.items():
        header = response.readline().split()
        if len(header) != 3:
            raise RuntimeError("unexpected Git object response")
        size = int(header[2])
        content = response.read(size)
        if len(content) != size or response.read(1) != b"\n":
            raise RuntimeError("truncated Git object response")
        if header[1] == b"blob":
            yield object_id.decode(), path, content


def scan(root: Path, include_history: bool) -> dict:
    current = []
    filenames = git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split(b"\0")
    checked = 0
    for filename in sorted(set(filter(None, filenames))):
        relative = filename.decode("utf-8", "replace")
        path = root / relative
        if not path.is_file():
            continue
        checked += 1
        content = path.read_bytes()
        categories = inspect(content)
        if len(content) > 1024 * 1024:
            categories.append("large_file_over_1_MiB")
        if path.suffix.lower() in BUILD_SUFFIXES:
            categories.append("build_or_archive_artifact")
        if categories:
            current.append({"path": relative, "categories": categories})
    historical = []
    blobs = 0
    if include_history:
        for object_id, path, content in history_blobs(root):
            blobs += 1
            categories = inspect(content)
            if categories:
                historical.append({"object": object_id[:12], "path": path, "categories": categories})
    return {"schema_version": 1, "checked_current_files": checked, "checked_history_blobs": blobs,
            "current_findings": current, "history_findings": historical,
            "limitations": "Pattern-based scan, not proof of absence of every secret. History is read-only; no rewriting or publication performed."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    report = scan(root, args.history)
    encoded = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    current_problem = bool(report["current_findings"])
    historical_secret = any(any(category in SECRET_PATTERNS for category in row["categories"])
                            for row in report["history_findings"])
    return 1 if current_problem or historical_secret else 0


if __name__ == "__main__":
    raise SystemExit(main())
