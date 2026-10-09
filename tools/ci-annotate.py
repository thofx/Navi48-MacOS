#!/usr/bin/env python3
"""ci-annotate.py - turn a failed step's output into GitHub Actions error annotations.

Annotations (unlike the job log) are readable through the public API without logging in, so a failure can be read from
outside. Usage: ci-annotate.py <title> [<log file>]  (stdin without a file). With a tools/check.sh log, one annotation per
"FAIL  <step>" block (the FAIL line plus the indented log tail that follows it); otherwise one annotation with the last 40
lines. At most 10 annotations, 3900 characters each (GitHub's limits).
"""
import sys


def encode(lines):
    return "%0A".join(l.rstrip("\n").replace("%", "%25") for l in lines)[:3900]


def main():
    title = sys.argv[1]
    lines = (open(sys.argv[2]) if len(sys.argv) > 2 else sys.stdin).readlines()
    blocks, block = [], []
    for line in lines:
        if line.startswith("FAIL "):
            if block:
                blocks.append(block)
            block = [line]
        elif block and line.startswith("      "):
            block.append(line[6:])
        elif block:
            blocks.append(block)
            block = []
    if block:
        blocks.append(block)
    if not blocks:
        blocks = [lines[-40:]]
    for b in blocks[:10]:
        print(f"::error title={title}::{encode(b)}")


if __name__ == "__main__":
    main()
