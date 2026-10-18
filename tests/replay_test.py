#!/usr/bin/env python3
"""Byte-for-byte replay, committed golden output, stdin, and malformed framing checks."""
import argparse
import json
import subprocess
from pathlib import Path


def invoke(executable, arguments=(), input_text=None):
    result = subprocess.run([str(executable), *map(str, arguments)], input=input_text,
                            text=True, capture_output=True, check=True, timeout=30)
    # Standard output is the deterministic protocol; diagnostics belong on stderr.
    for index, line in enumerate(result.stdout.splitlines(), 1):
        value = json.loads(line)
        assert isinstance(value, dict), f"output line {index} must be a JSON object"
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cli", type=Path)
    parser.add_argument("examples", type=Path)
    args = parser.parse_args()
    sample = args.examples / "sample.txt"
    first = invoke(args.cli, (sample,))
    second = invoke(args.cli, (sample,))
    expected = (args.examples / "sample.expected.jsonl").read_text(encoding="utf-8")
    assert first == second, "replaying the same file changed output"
    assert first == expected, "sample replay differs from committed golden output"
    assert invoke(args.cli, input_text=sample.read_text(encoding="utf-8")) == first, "stdin replay differs"
    assert invoke(args.cli, ("--pool", "--check", sample)) == first, "pool or invariant checking changed protocol output"

    commands = "\n".join(("# comment", "ADD 7 BUY 100 2", "ADD 8 SELL 100 1", "BOOK", "STATS"))
    assert invoke(args.cli, input_text=commands) == invoke(args.cli, input_text=commands + "\n"), "last line without newline was lost"
    # A rejected oversized record must not consume the following valid record.
    output = invoke(args.cli, input_text="x" * 4097 + "\nADD 1 BUY 100 2\nSTATS\n")
    records = [json.loads(line) for line in output.splitlines()]
    assert any(record.get("type") == "rejected" for record in records), "oversized line was not rejected"
    assert any(record.get("type") == "accepted" and record.get("order_id") == 1 for record in records), "oversized line corrupted next record"
    print("Replay golden file, repeatability, stdin, EOF, and oversized-record tests passed.")


if __name__ == "__main__":
    main()
