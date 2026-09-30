#!/usr/bin/env python3
"""
Differential fuzzer: interpreter vs JIT (tier_runner --auto).

Generates random, annotation-complete Lithon programs, and for each one
that the real typechecker accepts (hello's own exit code is the gate --
we do not reimplement V1_SPEC's rules here), runs it on both execution
engines and diffs stdout + exit code. On the first mismatch, the failing
program is minimized (repeated single-statement-removal passes, each
re-checked against the SAME two gates: still typechecks, still mismatches)
and the minimal repro is written to fuzz_failures/ as both .py and .ir.

CPython is used as a secondary oracle: if the interpreter itself disagrees
with CPython, that's a real bug too, just a different one (frontend/
interpreter vs V1_SPEC semantics, not JIT vs interpreter) -- reported
separately so it doesn't get confused with the primary JIT-vs-interpreter
comparison this tool exists to make reliable.

    tools/fuzz_diff.py                  # 500 programs, exit 1 on any bug
    tools/fuzz_diff.py --count 5000
    tools/fuzz_diff.py --seed 12345 --count 200

KNOWN LANGUAGE DIVERGENCE (found by this tool): Lithon lowers `for v in range(n)` to a counter that
IS `v` (src/frontend/frontend.py build_for), so unlike CPython:
  * after the loop, v == n (CPython: n-1);
  * a zero-iteration loop overwrites v with 0 (CPython leaves it untouched);
  * assigning to v inside the body changes the iteration count (can loop forever).
`for/else` and `while/else` are also silently dropped by the frontend. Until the language semantics are
decided, the generator uses dedicated loop variables that are never read or assigned outside their own
loop (so the CPython oracle stays meaningful); --shared-loop-vars generates the diverging shape.

AOT is not compared here: no AOT binary emitter exists in this repo yet
(see the src/ tree -- there is no ELF/PE writer anywhere). This tool
compares the two engines that actually exist. Extend CONFIGS/engines here
once a third one does.

Bitwise operators (&, |, ^, <<, >>) are NOT generated: the frontend has no
support for them at all yet (only `and`/`or`/`not`, which are boolean, not
bitwise) -- see src/frontend/frontend.py. That is a real language gap, not
a fuzzer limitation; this tool can start covering them the day they exist.
"""
import argparse
import ast
import random
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UNROLL_DIAMONDS = False
FRONTEND = ROOT / "src" / "frontend" / "frontend.py"
HELLO = ROOT / "build" / "hello"
TIER_RUNNER = ROOT / "build" / "tier_runner"

VARS = ["a", "b", "c", "d", "e", "f", "g", "h"]
LOOPVARS = ["i", "j", "k", "l"]      # never read or assigned outside their own loop (see docstring)

TIMEOUT = 15
TIMEOUT_RC = 124                     # returncode reported for a run that exceeded TIMEOUT


def run(cmd, **kw):
    """Like subprocess.run, but a timeout is returned as rc=TIMEOUT_RC instead of raised, so an
    infinite loop in one engine is a finding to report, not a crash of the fuzzer."""
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT, **kw)
    except subprocess.TimeoutExpired as e:
        def txt(x):
            return x.decode(errors="replace") if isinstance(x, bytes) else (x or "")
        return subprocess.CompletedProcess(cmd, TIMEOUT_RC, txt(e.stdout), txt(e.stderr))


# ---------------------------------------------------------------------
# Program generation. Every binding is explicitly typed (V1_SPEC 0.6.1),
# every function fully annotated (0.6.8), for-loop variables predeclared
# with a value before the loop (0.6.6), and only already-declared
# variables are mutated inside if/else bodies (so nothing trips the
# definite-assignment check at 0.6.10 for reasons unrelated to what
# we're testing). Magnitudes stay small so int[64] overflow -- a real,
# deliberate divergence from Python's arbitrary-precision ints -- never
# fires and muddies the diff.
# ---------------------------------------------------------------------
class Gen:
    def __init__(self, seed, shared_loop_vars=False):
        self.r = random.Random(seed)
        self.lines = []
        self.declared = set()
        self.shared_loop_vars = shared_loop_vars

    def emit(self, indent, s):
        self.lines.append("    " * indent + s)

    def atom(self):
        if self.declared and self.r.random() < 0.7:
            return self.r.choice(sorted(self.declared))
        return str(self.r.randint(0, 9))

    def fresh_var(self, exclude=()):
        """A VARS-pool name that is not already declared in this scope."""
        free = [v for v in VARS if v not in self.declared and v not in exclude]
        if not free:
            return self.r.choice(VARS)
        return self.r.choice(free)

    def expr(self, depth=0):
        k = self.r.random()
        if depth >= 2 or k < 0.4:
            return self.atom()
        if k < 0.7:
            return f"{self.expr(depth + 1)} + {self.expr(depth + 1)}"
        return f"{self.expr(depth + 1)} - {self.expr(depth + 1)}"

    def cond(self):
        ops = ["<", ">", "=="]
        c = f"{self.expr(1)} {self.r.choice(ops)} {self.expr(1)}"
        k = self.r.random()
        if k < 0.15:
            return f"not ({c})"
        return c

    def declare(self, indent, name, value_expr):
        self.emit(indent, f"{name}: int[64] = {value_expr}")
        self.declared.add(name)

    def stmt(self, indent, depth):
        k = self.r.random()
        if not self.declared or k < 0.3:
            name = self.r.choice(VARS)
            self.declare(indent, name, self.expr())
        elif k < 0.55:
            name = self.r.choice(sorted(self.declared))
            self.emit(indent, f"{name} = {self.expr()}")
        elif k < 0.7:
            self.emit(indent, f"print({self.expr()})")
        elif k < 0.85 and depth < 2:
            self.emit(indent, f"if {self.cond()}:")
            self.if_block(indent + 1, depth + 1)
            if self.r.random() < 0.5:
                self.emit(indent, "else:")
                self.if_block(indent + 1, depth + 1)
        elif depth < 2:
            n = self.r.randint(1, 6)
            if self.shared_loop_vars:
                var = self.r.choice(VARS)
                self.declare(indent, var, "0")          # later code may read/assign it: diverges from CPython
            else:
                var = self.r.choice(LOOPVARS)
                self.emit(indent, f"{var}: int[64] = 0")  # deliberately NOT added to self.declared
            # acc += m * var, with m a variable the loop never writes. This is
            # the shape strength_reduce_multiplies() rewrites, so the fuzzer
            # reaches that pass instead of only ever seeing `print(var)`.
            # Both `m` and `acc` have to be declared *before* the `for`, or the
            # declaration lands in the loop body and the frontend rejects it.
            acc = None
            if self.declared and self.r.random() < 0.5:
                m = self.r.choice(sorted(set(self.declared) - {var}))
                acc = self.fresh_var(exclude={m, var})
                self.declare(indent, acc, "0")
            self.emit(indent, f"for {var} in range({n}):")
            if acc is not None:
                self.emit(indent + 1, f"{acc} = {acc} + {m} * {var}")
            else:
                self.emit(indent + 1, f"print({var})")
        else:
            self.emit(indent, f"print({self.expr()})")

    def if_block(self, indent, depth):
        # Only mutate variables that already exist before the if/else, so
        # nothing declared here needs to survive past the branch.
        wrote = False
        for _ in range(self.r.randint(1, 3)):
            if self.declared and self.r.random() < 0.7:
                name = self.r.choice(sorted(self.declared))
                self.emit(indent, f"{name} = {self.expr()}")
                wrote = True
            else:
                self.emit(indent, f"print({self.expr()})")
                wrote = True
        if not wrote:
            self.emit(indent, "print(0)")

    # Two recursive shapes: a tail-recursive accumulator (like
    # tests/jit_only/tail_deep.py, just far shallower here -- that file
    # already covers the extreme-depth/TCO case) and a tree-recursive,
    # non-tail form (two arithmetic operations around the recursive call,
    # so it genuinely exercises call/return plumbing instead of being
    # rewritten into a loop). Depths stay well under CPython's default
    # 1000-frame recursion limit.
    def emit_recursive_helper(self):
        if self.r.random() < 0.5:
            self.emit(0, "def rec(n: int[64], acc: int[64]) -> int[64]:")
            self.emit(1, "if n < 1:")
            self.emit(2, "return acc")
            self.emit(1, "return rec(n - 1, acc + n)")
            depth = self.r.randint(5, 200)
            return f"rec({depth}, 0)"
        else:
            self.emit(0, "def rec(n: int[64]) -> int[64]:")
            self.emit(1, "if n < 1:")
            self.emit(2, "return 0")
            self.emit(1, "return n + rec(n - 1)")
            depth = self.r.randint(3, 15)
            return f"rec({depth})"

    # A deliberately small generator for strength reduction. The general
    # generator declares 3 variables up front and keeps declaring more, but
    # strength_reduce_multiplies() only fires when the function stores few
    # enough variables for the accumulator to still win a promotion register
    # (see the guard in optimize.h). Without this mode the fuzzer almost never
    # reaches the pass at all -- 300 general seeds fired it zero times.
    #
    # So: at most 2 program variables plus 1 accumulator, and the only
    # statement shapes are ones that keep the multiply and its induction
    # variable in the canonical two-block counted loop.
    def program_lsr(self):
        self.lines = []
        self.declared = set()
        outer = self.r.choice(VARS[:4])
        acc = self.fresh_var(exclude={outer})
        self.declare(0, outer, str(self.r.randint(1, 4)))
        self.declare(0, acc, "0")
        iv = self.r.choice(LOOPVARS)
        self.emit(0, f"{iv}: int[64] = 0")
        n = self.r.randint(1, 5)
        self.emit(0, f"for {iv} in range({n}):")
        shape = self.r.random()
        if shape < 0.45:
            # acc += m * iv  -- the exact match, varying operand order
            if self.r.random() < 0.5:
                self.emit(1, f"{acc} = {acc} + {outer} * {iv}")
            else:
                self.emit(1, f"{acc} = {acc} + {iv} * {outer}")
        elif shape < 0.7:
            # two accumulations in one body
            self.emit(1, f"{acc} = {acc} + {outer} * {iv}")
            self.emit(1, f"{acc} = {acc} - {outer} * {iv}")
        else:
            # product used through a local
            self.emit(1, f"p: int[64] = {outer} * {iv}")
            self.emit(1, f"{acc} = {acc} + p")
        self.emit(0, f"print({acc})")
        return "\n".join(self.lines) + "\n"

    def program_diamond(self):
        """Focused generator for the aggressive diamond unroller.

        Emits a counted loop whose body is exactly the shape
        match_diamond_unroll() claims to handle:

            H:  iv < n   -> D, exit
            D:  iv < k   -> A, E
            A:  acc = acc +- x;  jump B
            E:  acc = acc +- y;  jump B
            B:  iv = iv + 1;    jump H

        Trip counts deliberately straddle the unroll factor (0, 1, U-1, U,
        U+1, primes) because the unroller duplicates the exit test into every
        copy and closes the loop with a back edge to the entry test, so an
        off-by-one there only shows up on counts that are not a multiple of U.

        The arms are kept to pure arithmetic: a print() or a call in an arm
        would make the body fail is_unrollable_body() and the shape would
        never reach the unroller at all.
        """
        self.lines = []
        self.declared = set()
        acc = VARS[0]
        self.declare(0, acc, "0")
        n = self.r.choice([0, 1, 2, 3, 4, 5, 7, 8, 9, 11, 16, 17])
        # Mix the degenerate splits (always-then, always-else, never-taken)
        # with k in the middle of the range, so both arms and the transition
        # between them get executed.
        k = self.r.choice([0, n, n // 2, self.r.randint(0, n + 1)])
        then_delta = self.r.randint(1, 4)
        else_delta = self.r.randint(1, 4)
        op = self.r.choice(["+", "-"])
        iv = LOOPVARS[0]
        self.emit(0, f"{iv}: int[64] = 0")
        self.emit(0, f"for {iv} in range({n}):")
        self.emit(1, f"if {iv} < {k}:")
        self.emit(2, f"{acc} = {acc} {op} {then_delta}")
        self.emit(1, "else:")
        self.emit(2, f"{acc} = {acc} {op} {else_delta}")
        self.emit(0, f"print({acc})")
        return "\n".join(self.lines) + "\n"

    def program(self):
        call_expr = self.emit_recursive_helper() if self.r.random() < 0.5 else None
        for v in VARS[:3]:
            self.declare(0, v, str(self.r.randint(0, 5)))
        for _ in range(self.r.randint(4, 10)):
            self.stmt(0, 0)
        if call_expr:
            self.emit(0, f"print({call_expr})")
        self.emit(0, f"print({self.expr()})")
        return "\n".join(self.lines) + "\n"


# ---------------------------------------------------------------------
# Running one candidate through both engines.
# ---------------------------------------------------------------------
class Result:
    def __init__(self, ok, interp_out=None, interp_rc=None, jit_out=None,
                 jit_rc=None, cpy_out=None, cpy_rc=None, reason=None):
        self.ok = ok            # False => a genuine mismatch was found
        self.interp_out, self.interp_rc = interp_out, interp_rc
        self.jit_out, self.jit_rc = jit_out, jit_rc
        self.cpy_out, self.cpy_rc = cpy_out, cpy_rc
        self.reason = reason    # None | "skip" | "interp_vs_cpython" | "jit_vs_interp"


def evaluate(py_path: Path, tmp_ir: Path):
    fe = run([sys.executable, str(FRONTEND), str(py_path)])
    if fe.returncode != 0:
        return Result(True, reason="skip")   # not even valid Lithon syntax
    tmp_ir.write_text(fe.stdout)

    interp = run([str(HELLO), str(tmp_ir)])
    if interp.returncode not in (0, TIMEOUT_RC):
        return Result(True, reason="skip")   # typecheck (or other) rejection -- not our concern here

    # The diamond unroller is off by default (it measured slower), so --diamond
    # has to opt in or it would fuzz a build that never runs the transform.
    jit_args = [str(TIER_RUNNER), str(tmp_ir), "--auto"]
    if UNROLL_DIAMONDS:
        jit_args.append("--unroll-diamonds")
    jit = run(jit_args)
    cpy = run([sys.executable, str(py_path)])
    seen = (interp.stdout, interp.returncode, jit.stdout, jit.returncode, cpy.stdout, cpy.returncode)

    # 1. PRIMARY: the two engines must agree with each other. Checked first so a CPython disagreement
    #    can never mask a JIT bug. Both hanging counts as agreement (partial stdout is not comparable).
    both_hang = interp.returncode == TIMEOUT_RC and jit.returncode == TIMEOUT_RC
    if not both_hang and (jit.stdout != interp.stdout or jit.returncode != interp.returncode):
        return Result(False, *seen, reason="jit_vs_interp")

    # 2. SECONDARY: what they agree on must match CPython (a language-semantics gap, not a JIT bug).
    if cpy.returncode == 0:
        if interp.returncode == TIMEOUT_RC:
            return Result(False, *seen, reason="hang_vs_cpython")   # Lithon never finishes; CPython does
        if cpy.stdout != interp.stdout:
            return Result(False, *seen, reason="interp_vs_cpython")

    return Result(True)


# ---------------------------------------------------------------------
# Minimization: repeated whole-STATEMENT removal on the AST (a block is removed together with its
# body). The old line-based version could delete an `if` header and leave its body -- or an `else:` --
# behind, silently turning the program into a different program (a loop-body assignment, a for/else).
# A removal is kept only if the program still reproduces the SAME class of mismatch.
# ---------------------------------------------------------------------
def _statement_lists(tree):
    lists = [tree.body]
    for node in ast.walk(tree):
        if node is tree:
            continue
        for field in ("body", "orelse"):
            lst = getattr(node, field, None)
            if isinstance(lst, list) and lst and isinstance(lst[0], ast.stmt):
                lists.append(lst)
    return lists


def minimize(src: str, target_reason: str, tmp_dir: Path) -> str:
    tree = ast.parse(src)

    def reproduces():
        py = tmp_dir / "_min.py"
        py.write_text(ast.unparse(tree) + "\n")
        r = evaluate(py, tmp_dir / "_min.ir")
        return (not r.ok) and r.reason == target_reason

    def remove_one():
        for lst in _statement_lists(tree):
            for i in range(len(lst)):
                if len(lst) == 1:
                    break                      # keep every block non-empty
                removed = lst.pop(i)
                if reproduces():
                    return True
                lst.insert(i, removed)
        return False

    while remove_one():
        pass
    return ast.unparse(tree) + "\n"


def main():
    global TIMEOUT
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=500)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=TIMEOUT, help="seconds before a run counts as a hang")
    ap.add_argument("--lsr", action="store_true",
                    help="use the small strength-reduction-focused generator (see Gen.program_lsr)")
    ap.add_argument("--no-diamond-unroll", action="store_true",
                    help="do not pass --unroll-diamonds to tier_runner (used by --diamond)")
    ap.add_argument("--diamond", action="store_true",
                    help="use the small if/else-diamond-focused generator (see Gen.program_diamond)")
    ap.add_argument("--shared-loop-vars", action="store_true",
                    help="generate loop variables that are read/assigned elsewhere (reproduces the known "
                         "for-loop divergence from CPython; see the docstring)")
    args = ap.parse_args()
    TIMEOUT = args.timeout
    global UNROLL_DIAMONDS
    UNROLL_DIAMONDS = args.diamond and not args.no_diamond_unroll

    for exe, name in ((HELLO, "hello"), (TIER_RUNNER, "tier_runner")):
        if not exe.exists():
            print(f"error: {name} not built -- run: cmake --build build", file=sys.stderr)
            return 2

    tmp_dir = Path("/tmp/lithon_fuzz_diff")
    tmp_dir.mkdir(exist_ok=True)
    fail_dir = ROOT / "fuzz_failures"

    n_ok = n_skip = n_fail = 0
    by_reason = {}
    for seed in range(args.seed, args.seed + args.count):
        gen = Gen(seed, args.shared_loop_vars)
        if args.diamond:
            src = gen.program_diamond()
        elif args.lsr:
            src = gen.program_lsr()
        else:
            src = gen.program()
        py = tmp_dir / "candidate.py"
        py.write_text(src)
        result = evaluate(py, tmp_dir / "candidate.ir")

        if result.ok:
            if result.reason == "skip":
                n_skip += 1
            else:
                n_ok += 1
            continue

        n_fail += 1
        print(f"\n=== MISMATCH (seed={seed}, {result.reason}) ===")
        minimal = minimize(src, result.reason, tmp_dir)

        fail_dir.mkdir(exist_ok=True)
        stem = f"seed{seed}_{result.reason}"
        (fail_dir / f"{stem}.py").write_text(minimal)
        fe = run([sys.executable, str(FRONTEND), str(fail_dir / f"{stem}.py")])
        if fe.returncode == 0:
            (fail_dir / f"{stem}.ir").write_text(fe.stdout)

        print(f"minimal repro written to {fail_dir / (stem + '.py')} "
              f"and {fail_dir / (stem + '.ir')}")
        print("--- minimal source ---")
        print(minimal)
        reval = evaluate(fail_dir / f"{stem}.py", tmp_dir / "reval.ir")
        print(f"interpreter: rc={reval.interp_rc} stdout={reval.interp_out!r}")
        print(f"jit        : rc={reval.jit_rc} stdout={reval.jit_out!r}")
        print(f"cpython    : rc={reval.cpy_rc} stdout={reval.cpy_out!r}")
        print("(rc=%d means the run hit --timeout and was killed)" % TIMEOUT_RC)
        by_reason[result.reason] = by_reason.get(result.reason, 0) + 1

    shutil.rmtree(tmp_dir, ignore_errors=True)
    print(f"\n{n_ok} passed, {n_skip} skipped (rejected before reaching the "
          f"interp/JIT comparison), {n_fail} mismatches, out of {args.count}")
    for reason, n in sorted(by_reason.items()):
        meaning = {"jit_vs_interp": "JIT BUG: native output differs from the interpreter",
                   "interp_vs_cpython": "language gap: Lithon output differs from CPython",
                   "hang_vs_cpython": "language gap: Lithon does not terminate, CPython does"}.get(reason, "")
        print(f"  {reason}: {n}   {meaning}")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
