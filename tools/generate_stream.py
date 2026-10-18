#!/usr/bin/env python3
"""Generate replay commands; independent of the core-only benchmark generator."""
import argparse
from pathlib import Path
import random


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--events", type=int, default=100000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.events <= 10000000:
        parser.error("--events must be 1..10000000")
    random_source = random.Random(args.seed)
    with args.output.open("w", encoding="ascii", newline="\n") as output:
        output.write(f"# generated events={args.events} seed={args.seed}\n")
        for index in range(1, args.events + 1):
            if index > 1 and random_source.randrange(5) == 0:
                # Some targets are inactive: rejection is part of replay behavior.
                output.write(f"CANCEL {random_source.randrange(1, index)}\n")
            else:
                side = "BUY" if random_source.randrange(2) == 0 else "SELL"
                price = 10000 + random_source.randrange(-20, 21)
                quantity = random_source.randrange(1, 101)
                output.write(f"ADD {index} {side} {price} {quantity}\n")
        output.write("BOOK\nSTATS\n")


if __name__ == "__main__":
    main()
