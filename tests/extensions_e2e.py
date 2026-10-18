#!/usr/bin/env python3
"""Verify delivered pipeline/TWAP programs and committed examples."""
import argparse
import json
from pathlib import Path
import subprocess


def run(command, data=None):
    result = subprocess.run(command, input=data, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, (command, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pipeline", required=True)
    parser.add_argument("--twap", required=True)
    parser.add_argument("--examples", required=True)
    args = parser.parse_args()
    examples = Path(args.examples)
    expected = (examples / "sample.expected.jsonl").read_text()
    result = run([args.pipeline, "--capacity", "1", str(examples / "sample.txt")])
    assert result.stdout == expected, "pipeline output differs from committed sequential replay"
    assert "queue_high_water=1 capacity=1" in result.stderr
    stdin = run([args.pipeline, "--capacity", "2"], (examples / "sample.txt").read_text())
    assert stdin.stdout == expected, "stdin pipeline differs from file pipeline"
    oversized = run([args.pipeline, "--capacity", "1"], "X" * 300000 + "\nBOOK")
    decoded = [json.loads(line) for line in oversized.stdout.splitlines()]
    assert len(decoded) == 2 and decoded[0]["reason"] == "line_too_long"
    assert decoded[1]["type"] == "book"
    empty = run([args.pipeline, "--capacity", "1"], "")
    assert not empty.stdout and "lines=0" in empty.stderr
    invalid = subprocess.run([args.pipeline, "--capacity", "0"], capture_output=True, text=True, timeout=5)
    assert invalid.returncode != 0

    command = [args.twap, "--quantity", "23", "--slices", "5", "--start", "0", "--end", "100",
               "--side", "BUY", "--price", "101", "--first-id", "1000", "--market", str(examples / "twap_market.txt")]
    first = run(command).stdout
    assert first == run(command).stdout, "TWAP output is not deterministic"
    events = [json.loads(line) for line in first.splitlines()]
    assert events[-1] == json.loads((examples / "twap.expected-report.json").read_text())
    times = [event["time"] for event in events[:-1]]
    assert times == sorted(times) and times[0] == 0 and times[-1] == 100
    assert run([args.twap]).stdout == first, "default TWAP example differs from committed market input"
    invalid = subprocess.run([args.twap, "--quantity", "0"], capture_output=True, text=True, timeout=5)
    assert invalid.returncode != 0
    print("pipeline and TWAP executable examples passed")


if __name__ == "__main__":
    main()
