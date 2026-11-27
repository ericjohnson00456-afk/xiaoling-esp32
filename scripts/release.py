#!/usr/bin/env python3
"""Deprecated board build entry point; use scripts/build.py instead."""

import sys
import build


def main():
    print("Warning: scripts/release.py is deprecated; use scripts/build.py instead.",
          file=sys.stderr)
    return build.main()


if __name__ == "__main__":
    raise SystemExit(main())
