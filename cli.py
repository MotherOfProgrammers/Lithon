"""`lithon` command line:  lithon script.py [--strict | --interp] [--ir] [--verbose]"""
import argparse
import sys

# Absolute, not relative: pyproject installs cli/init/main as flat top-level
# py-modules, so `cli` is not part of a package and has no parent to be
# relative to. The implementation lives in init.py, which is a duplicate of
# __init__.py with _ROOT resolving to the repo root rather than its parent.
from init import LithonError, __version__, compile as compile_file, run


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="lithon", description="Run a script on the Lithon engine.")
    ap.add_argument("script", help="Python-syntax source file")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--strict", action="store_true",
                   help="native only: refuse (exit 3) instead of falling back to the interpreter")
    g.add_argument("--interp", action="store_true",
                   help="interpreter only (the correctness oracle)")
    ap.add_argument("--ir", action="store_true", help="print the IR and exit")
    ap.add_argument("-v", "--verbose", action="store_true", help="show which tier ran and why")
    ap.add_argument("--version", action="version", version=f"lithon {__version__}")
    args = ap.parse_args(argv)

    try:
        if args.ir:
            sys.stdout.write(compile_file(args.script))
            return 0
        mode = "strict" if args.strict else "interp" if args.interp else "auto"
        result = run(args.script, mode=mode)
        if args.verbose:
            for line in result.stderr.splitlines():
                print(line, file=sys.stderr)
        return 0
    except LithonError as e:
        print(f"lithon: {e}", file=sys.stderr)
        return e.returncode or 1
    except FileNotFoundError:
        print(f"lithon: cannot open {args.script}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
