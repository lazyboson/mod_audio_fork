#!/usr/bin/env python3
"""Cumulative failed-call count from a SIPp -trace_stat CSV.

Prints a sentinel far above any tolerance when the file is missing or has no
data rows, so a SIPp that never started fails the run instead of reporting 0.
"""
import csv
import sys

MISSING = 999999999


def main() -> int:
    try:
        with open(sys.argv[1], newline="", encoding="utf-8", errors="replace") as handle:
            rows = list(csv.reader(handle, delimiter=";"))
    except OSError as error:
        print(MISSING)
        print(f"no SIPp stat file: {error}", file=sys.stderr)
        return 0
    if len(rows) < 2:
        print(MISSING)
        print("SIPp stat file has no data rows", file=sys.stderr)
        return 0
    header = rows[0]
    try:
        column = header.index("FailedCall(C)")
    except ValueError:
        print(MISSING)
        print(f"no FailedCall(C) column in {header}", file=sys.stderr)
        return 0
    last = [row for row in rows[1:] if len(row) > column][-1]
    print(int(last[column]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
