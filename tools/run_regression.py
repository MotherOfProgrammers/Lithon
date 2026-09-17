#!/usr/bin/env python3
"""
M2 regression driver.

Runs every tests/programs/*.py through:
  1. CPython directly (recorded once as tests/programs/expected/*.out)
  2. The Lithon pipeline: src/frontend/frontend.py -> build/hello

Fails loudly, and shows a diff, on any mismatch. Run this after any
change to the frontend or interpreter -- it should stay green.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROGRAMS_DIR = ROOT / "tests" / "programs"
EXPECTED_DIR = PROGRAMS_DIR / "expected"
FRONTEND = ROOT / "src" / "frontend" / "frontend.py"
INTERP_BIN = ROOT / "build" / "hello"


def run_lithon(py_file: pathlib.Path) -> str:
    ir_result = subprocess.run(
        ["python3", str(FRONTEND), str(py_file)],
        capture_output=True, text=True
    )
    if ir_result.returncode != 0:
        raise RuntimeError(f"frontend failed on {py_file.name}:\n{ir_result.stderr}")

    ir_text = ir_result.stdout
    ir_path = ROOT / f"_tmp_{py_file.stem}.ir"
    ir_path.write_text(ir_text)

    try:
        run_result = subprocess.run(
            [str(INTERP_BIN), str(ir_path)],
            capture_output=True, text=True
        )
    finally:
        ir_path.unlink(missing_ok=True)

    if run_result.returncode != 0:
        raise RuntimeError(f"interpreter failed on {py_file.name}:\n{run_result.stderr}")

    # hello.cpp currently prints debug lines ("functions parsed: ...",
    # "--- running interpreter ---") before the real program output.
    # Strip everything up to and including that marker line.
    lines = run_result.stdout.splitlines(keepends=True)
    marker = "--- running interpreter ---\n"
    if marker in lines:
        idx = lines.index(marker)
        lines = lines[idx + 1:]
    return "".join(lines)


def main():
    programs = sorted(PROGRAMS_DIR.glob("*.py"))
    if not programs:
        print("no test programs found under tests/programs/")
        return 1

    failures = []
    passed = 0

    for prog in programs:
        expected_path = EXPECTED_DIR / f"{prog.stem}.out"
        if not expected_path.exists():
            failures.append((prog.name, "missing expected output file"))
            continue

        expected = expected_path.read_text()

        try:
            actual = run_lithon(prog)
        except RuntimeError as e:
            failures.append((prog.name, str(e)))
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
