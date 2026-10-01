"""`python -m main` entry point, kept for the flat py-modules layout.

The import is absolute because pyproject installs cli/init/main as flat
top-level modules; see the same note in cli.py.
"""
import sys

from cli import main

if __name__ == "__main__":
    sys.exit(main())
