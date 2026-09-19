#!/usr/bin/env python3
"""
Typed regression driver: runs each tests/typed_regression/*.py
through the FULL real pipeline -- frontend.py -> lithon_interp
--typecheck -- and compares against expected/*.out (reused from the
already-CPython-verified untyped originals, since these typed
sources are semantically identical but not runnable by real CPython
due to "int[64]" not being valid subscript syntax at runtime).
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROGRAMS_DIR = ROOT / "tests" / "typed_regression"
EXPECTED_DIR = PROGRAMS_DIR / "expected"
FRONTEND = ROOT / "src" / "frontend" / "frontend.py"
INTERP_BIN = ROOT / "build" / "hello"


def run_lithon_typed(py_file: pathlib.Path):
    ir_result = subprocess.run(
        ["python3", str(FRONTEND), str(py_file)],
        capture_output=True, text=True
    )
    if ir_result.returncode != 0:
        return None, f"frontend failed:\n{ir_result.stderr}"

    ir_path = ROOT / f"_tmp_typed_{py_file.stem}.ir"
    ir_path.write_text(ir_result.stdout)

    try:
        run_result = subprocess.run(
            [str(INTERP_BIN), "--typecheck", str(ir_path)],
            capture_output=True, text=True
        )
    finally:
        ir_path.unlink(missing_ok=True)

    if run_result.returncode != 0:
        return None, f"typecheck/run failed (exit {run_result.returncode}):\n{run_result.stdout}{run_result.stderr}"

    lines = run_result.stdout.splitlines(keepends=True)
    marker = "--- running interpreter ---\n"
    if marker in lines:
        idx = lines.index(marker)
        lines = lines[idx + 1:]
    return "".join(lines), None


def main():
    programs = sorted(PROGRAMS_DIR.glob("*.py"))
    if not programs:
        print("no typed test programs found")
        return 1

    failures = []
    passed = 0

    for prog in programs:
        expected_path = EXPECTED_DIR / f"{prog.stem}.out"
        if not expected_path.exists():
            failures.append((prog.name, "missing expected output file"))
            continue
        expected = expected_path.read_text()

        actual, err = run_lithon_typed(prog)
        if err:
            failures.append((prog.name, err))
            continue
        if actual != expected:
            failures.append((prog.name,
                f"MISMATCH\n--- expected ---\n{expected}--- got ---\n{actual}"))
        else:
            passed += 1

    print(f"\n{passed}/{len(programs)} passed\n")
    if failures:
        print(f"{len(failures)} FAILURE(S):\n")
        for name, detail in failures:
            print(f"[{name}] {detail}\n")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
