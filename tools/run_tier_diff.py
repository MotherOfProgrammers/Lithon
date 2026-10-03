#!/usr/bin/env python3
"""
Tier differential test.

For every program, run it twice through build/tier_runner:
  --interp   the interpreter (the correctness oracle)
  --auto     native when the print guard proves it safe, else interpreter

The two stdout streams must be byte-identical. The tier that --auto
actually used is read from stderr ([tier1] native / [tier0] interpreter)
and reported, so a green run cannot hide "everything silently fell back".

Inputs:
  * tests/programs/*.py and tests/typed_regression/*.py (via the frontend)
  * ADVERSARIAL: hand-written IR aimed at print-format divergence

Adversarial cases also declare which tier they EXPECT, so a guard that
becomes too weak (native where it must refuse) or too strict (interpreter
where native is provably safe) both fail the run.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNNER = ROOT / "build" / "tier_runner"
FRONTEND = ROOT / "src" / "frontend" / "frontend.py"
SUITES = [ROOT / "tests" / "programs", ROOT / "tests" / "typed_regression"]

# name -> (ir_text, expected_tier_for_auto) ; tier is "tier0" or "tier1"
# Bool-only prints run natively (True/False). Float and mixed numeric prints
# run natively too, now that the JIT has XMM arithmetic, float comparison and
# CPython-compatible float rendering. Only genuinely unknown prints must fall
# back -- a tier0 expectation here means "the guard is not yet strong enough",
# so each one is a standing reminder of what is still unsupported.
ADVERSARIAL = {
    "bool_from_compare": ("""
function main():
block0:
    %0 = const_i64 1
    %1 = const_i64 2
    %2 = lt %0, %1
    call print, %2
    return
""", "tier1"),
    "not_of_int": ("""
function main():
block0:
    %0 = const_i64 5
    %1 = not %0
    call print, %1
    %2 = const_i64 0
    %3 = not %2
    call print, %3
    return
""", "tier1"),
    "bool_via_variable": ("""
function main():
block0:
    %0 = const_i64 3
    %1 = const_i64 4
    %2 = gt %0, %1
    store flag, %2
    %3 = load flag
    call print, %3
    return
""", "tier1"),
    "bool_via_return": ("""
function less(a, b):
block0:
    %0 = load a
    %1 = load b
    %2 = lt %0, %1
    return %2
    return

function main():
block0:
    %0 = const_i64 1
    %1 = const_i64 2
    %2 = call less, %0, %1
    call print, %2
    return
""", "tier1"),
    "float_print": ("""
function main():
block0:
    %0 = const_f64 3.5
    %1 = const_f64 2.0
    %2 = add %0, %1
    call print, %2
    return
""", "tier1"),
    # Float modulo, including the trunc-vs-floor cases that CPython's own %
    # gets differently and so cannot live in tests/programs/float.py, which is
    # compared against CPython. The two tiers must agree byte for byte.
    #
    # 1.0 % inf is the case that was actually wrong: n = trunc(1.0/inf) is 0,
    # so the answer is the dividend, but the guard meant to skip the n*b
    # multiply jumped to the addsd that normalizes signed zero, one
    # instruction early, leaving 0 * inf to produce a NaN.
    "float_mod_trunc_and_inf": ("""
function main():
block0:
    %0 = const_f64 1e308
    store big, %0
    %1 = load big
    %2 = load big
    %3 = mul %1, %2
    store inf, %3
    %4 = load inf
    %5 = load inf
    %6 = sub %4, %5
    store nan, %6
    %7 = const_f64 7.5
    %8 = const_f64 2.0
    %9 = const_f64 -7.5
    %10 = const_f64 -2.0
    %11 = const_f64 -4.0
    %12 = const_f64 1.0
    %13 = const_f64 0.0
    %14 = mod %7, %8
    call print, %14
    %15 = mod %9, %8
    call print, %15
    %16 = mod %7, %10
    call print, %16
    %17 = mod %9, %10
    call print, %17
    %18 = mod %11, %8
    call print, %18
    %19 = mod %12, %8
    call print, %19
    %20 = load inf
    %21 = mod %12, %20
    call print, %21
    %22 = mod %13, %20
    call print, %22
    %23 = mod %20, %12
    call print, %23
    %24 = mod %20, %20
    call print, %24
    %25 = load nan
    %26 = mod %12, %25
    call print, %26
    %27 = mod %25, %12
    call print, %27
    return
""", "tier1"),
    # The allocator may hand a dynamic shift's result RCX (it is a member of
    # kTempPool on POSIX), and `shl rcx, cl` would shift a register by its own
    # low bits. So the result is computed elsewhere and moved into RCX -- which
    # has to happen AFTER the pop that restores the shift count, or the pop
    # overwrites the result with the count and the store commits the count.
    # The first shl below is allocated to RCX, so this is that exact case.
    "dynamic_shift_rcx_dst": ("""
function main():
block0:
    %0 = const_i64 5
    store v, %0 : int[64]
    %1 = const_i64 3
    store k, %1 : int[64]
    %2 = load v
    %3 = load k
    %4 = shl %2, %3
    store r1, %4 : int[64]
    %5 = const_i64 255
    %6 = load k
    %7 = shr %5, %6
    store r2, %7 : int[64]
    %8 = load r1
    call print, %8
    %9 = load r2
    call print, %9
    return
""", "tier1"),
    "int_and_or_value_semantics": ("""
function main():
block0:
    %0 = const_i64 5
    %1 = const_i64 0
    %2 = and %0, %1
    call print, %2
    %3 = or %0, %1
    call print, %3
    %4 = and %1, %0
    call print, %4
    %5 = or %1, %0
    call print, %5
    return
""", "tier1"),
    "int_arithmetic_chain": ("""
function main():
block0:
    %0 = const_i64 7
    %1 = const_i64 6
    %2 = mul %0, %1
    %3 = const_i64 2
    %4 = sub %2, %3
    %5 = add %4, %0
    call print, %5
    return
""", "tier1"),
    "compare_used_only_for_branch": ("""
function main():
block0:
    %0 = const_i64 1
    %1 = const_i64 2
    %2 = lt %0, %1
    branch %2, block1, block2
block1:
    %3 = const_i64 111
    call print, %3
    return
block2:
    %4 = const_i64 222
    call print, %4
    return
""", "tier1"),
    "int_recursion": ("""
function fact(n):
block0:
    %0 = load n
    %1 = const_i64 2
    %2 = lt %0, %1
    branch %2, block1, block2
block1:
    %3 = const_i64 1
    return %3
    jump block2
block2:
    %4 = load n
    %5 = const_i64 1
    %6 = sub %4, %5
    %7 = call fact, %6
    %8 = load n
    %9 = mul %8, %7
    return %9
    return

function main():
block0:
    %0 = const_i64 10
    %1 = call fact, %0
    call print, %1
    return
""", "tier1"),
}


def run(args):
    return subprocess.run([str(RUNNER)] + args, capture_output=True, text=True)


def tier_of(stderr):
    if "[tier1]" in stderr:
        return "tier1"
    if "[tier0]" in stderr:
        return "tier0"
    return "unknown"


def compile_to_ir(py_file, tmpdir):
    r = subprocess.run(["python3", str(FRONTEND), str(py_file)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"frontend failed on {py_file.name}: {r.stderr}")
    ir = pathlib.Path(tmpdir) / (py_file.stem + ".ir")
    ir.write_text(r.stdout)
    return ir


def check(label, ir_path, expected_tier=None):
    ref = run([str(ir_path), "--interp"])
    auto = run([str(ir_path), "--auto"])
    tier = tier_of(auto.stderr)

    problems = []
    if ref.returncode != 0:
        problems.append(f"interpreter failed: {ref.stderr.strip()[:200]}")
    if auto.returncode != 0:
        problems.append(f"auto failed: {auto.stderr.strip()[:200]}")
    if ref.stdout != auto.stdout:
        problems.append("STDOUT MISMATCH\n"
                        f"      interp: {ref.stdout!r}\n"
                        f"      auto  : {auto.stdout!r}")
    if expected_tier and tier != expected_tier:
        problems.append(f"expected {expected_tier}, ran {tier}")

    status = "PASS" if not problems else "FAIL"
    print(f"[{status}] {label:<40} ran on {tier}")
    for p in problems:
        print(f"      {p}")
    return not problems, tier


def main():
    if not RUNNER.exists():
        print(f"missing {RUNNER}; build it first (see src/jit/tier_runner.cpp)")
        return 2

    results = []
    tiers = {"tier0": 0, "tier1": 0, "unknown": 0}

    with tempfile.TemporaryDirectory() as tmp:
        for suite in SUITES:
            for py in sorted(suite.glob("*.py")):
                ir = compile_to_ir(py, tmp)
                ok, tier = check(f"{suite.name}/{py.stem}", ir)
                results.append(ok)
                tiers[tier] += 1

        print("--- adversarial ---")
        for name, (ir_text, want) in ADVERSARIAL.items():
            ir = pathlib.Path(tmp) / f"{name}.ir"
            ir.write_text(ir_text)
            ok, tier = check(f"adversarial/{name}", ir, want)
            results.append(ok)
            tiers[tier] += 1

    passed = sum(results)
    print(f"\n{passed}/{len(results)} passed  "
          f"(native: {tiers['tier1']}, interpreter fallback: {tiers['tier0']})")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
