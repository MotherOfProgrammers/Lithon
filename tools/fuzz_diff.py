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

    tools/fuzz_diff.py --floats          # float programs: arithmetic, compares, printing
    tools/fuzz_diff.py --mod             # integer modulo: powers of two, idiv, zero divisor
    tools/fuzz_diff.py --bitwise         # & | ^ << >>: both encodings, count bounds, RCX hazard

--floats is a separate mode rather than a tweak to the int generators because
the int generators annotate every variable int[64], and a float cannot be
declared that way. It emits flat, unannotated float programs instead. That
mode is the only thing here that covers float codegen at all, and it has
already paid for itself: it is what found a float temporary being clobbered
across a call, `%g` picking exponent notation by precision rather than by
value, std::stod rejecting every subnormal literal, and a NaN divisor taking
the division-by-zero trap.

--mod exists because modulo is the one operator where the CPython oracle
cannot referee the general case: C's `%` truncates toward zero and Python's
floors, so the two agree only for non-negative operands. --mod therefore
generates non-negative dividends and stays a meaningful CI gate, and
--mod-negatives opts into the diverging shape (interpreter vs JIT is still
enforced; the CPython comparison is expected to report the language gap, the
same way --shared-loop-vars does).
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
ACCUM_UNROLL = False
# --phi: run the JIT twice, with and without the SSA pipeline, and require both
# to match the interpreter. The plain run is the control: it proves the pipeline
# did not change the answer, rather than only proving the pipeline agrees with
# the interpreter on some other program's terms.
SSA_MODE = False
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
    # Float mode. The int generator above cannot see any of the float
    # machinery: it emits no float literals, never divides, and compares only
    # ints, so float codegen was entirely uncovered by the fuzzer until this
    # mode existed -- which is how a float temporary live across a `print`
    # survived in a caller-saved XMM, and how `%g` choosing exponent notation
    # by precision instead of by value went unnoticed.
    #
    # Deliberate choices, each of which is a case that has actually been wrong:
    #   * `/` is generated, so the zero-divisor trap and the NaN-divisor
    #     non-trap are both exercised.
    #   * float and int atoms are mixed in the same expression, so the
    #     int-to-double promotion and mixed comparison paths get hit.
    #   * literals include subnormals and huge/tiny magnitudes, because the
    #     round-trip formatter has a different code path for each and
    #     std::stod used to reject every subnormal outright.
    #   * a `print` is emitted *between* statements so values are live across
    #     a call, which is the register-allocator case above.
    FLOAT_VARS = ["p", "q", "r", "s"]

    # Small decimals print as themselves; the wide ones stress the exponent
    # threshold (CPython switches to exponent form only below 1e-4 or at/above
    # 1e16) and the subnormals stress the short-round-trip search.
    FLOAT_LITERALS = [
        "0.0", "-0.0", "1.0", "-1.0", "0.5", "2.5", "3.5", "7.0", "0.1",
        "1e16", "1e15", "1e-5", "1e-4", "1e300", "1e-300", "1.5e-8",
        "5e-324", "2.2250738585072014e-308", "1.7976931348623157e308",
        "1.0000000000000002", "0.30000000000000004", "-2.5", "1e6", "1e-6",
    ]

    def __init__(self, seed, shared_loop_vars=False, floats=False, mod_negatives=False):
        self.r = random.Random(seed)
        self.lines = []
        self.declared = set()
        self.float_declared = set()
        self.shared_loop_vars = shared_loop_vars
        self.floats = floats
        self.mod_negatives = mod_negatives
        self.pool = self.FLOAT_VARS if floats else VARS

    def emit(self, indent, s):
        self.lines.append("    " * indent + s)

    def float_atom(self):
        if self.float_declared and self.r.random() < 0.7:
            return self.r.choice(sorted(self.float_declared))
        return self.r.choice(self.FLOAT_LITERALS)

    def atom(self):
        if not self.floats:
            if self.declared and self.r.random() < 0.7:
                return self.r.choice(sorted(self.declared))
            return str(self.r.randint(0, 9))
        # Float mode mixes int and float atoms freely: an int in a float
        # expression is what forces the cvtsi2sd promotion, and a float in an
        # otherwise-int context is what forces the guard to infer a float kind.
        k = self.r.random()
        if k < 0.5 and self.float_declared:
            return self.r.choice(sorted(self.float_declared))
        if k < 0.8:
            return self.r.choice(self.FLOAT_LITERALS)
        if self.declared and self.r.random() < 0.5:
            return self.r.choice(sorted(self.declared))
        return str(self.r.randint(0, 9))

    def fresh_var(self, exclude=()):
        """A VARS-pool name that is not already declared in this scope."""
        free = [v for v in self.pool if v not in self.declared and v not in exclude]
        if not free:
            return self.r.choice(self.pool)
        return self.r.choice(free)

    def expr(self, depth=0):
        k = self.r.random()
        if depth >= 2 or k < 0.4:
            return self.atom()
        # `/` is the important one: it is the only op that can trap, and the
        # only one whose result is a float even when both operands are ints.
        op = self.r.choice(["+", "-", "*", "/"] if self.floats else ["+", "-"])
        return f"{self.expr(depth + 1)} {op} {self.expr(depth + 1)}"

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

    def float_program(self):
        """A float-only program: no annotations, no loops, no helper calls.

        Deliberately flat. Everything interesting about float support is in the
        arithmetic, the comparison and the formatting, and a flat shape keeps
        any mismatch attributable to one of those rather than to a control-flow
        transform on top of it.
        """
        # Declare every float variable up front, so a later statement can read
        # one that was written several statements earlier -- that distance is
        # what pushes a value across a `print` and out of a register.
        for v in self.FLOAT_VARS:
            self.emit(0, f"{v} = {self.r.choice(self.FLOAT_LITERALS)}")
            self.float_declared.add(v)

        for _ in range(self.r.randint(4, 10)):
            k = self.r.random()
            if k < 0.35:
                # Read a float, do arithmetic, print the result.
                self.emit(0, f"print({self.expr(1)})")
            elif k < 0.55:
                # Compare and print the bool: exercises the setcc-and-not-parity
                # path, including the NaN case.
                self.emit(0, f"print({self.cond()})")
            elif k < 0.7:
                name = self.r.choice(sorted(self.float_declared))
                self.emit(0, f"{name} = {self.expr(1)}")
            elif k < 0.85:
                # A bare int print between float statements: the call here is
                # what a float value has to survive.
                self.emit(0, f"print({self.r.randint(0, 9)})")
            else:
                self.emit(0, f"print({self.float_atom()})")
        return "\n".join(self.lines) + "\n"

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
            if self.floats:
                # Float mode has no annotated int variables, so the only
                # assignable state is the float pool.
                if self.float_declared and self.r.random() < 0.7:
                    name = self.r.choice(sorted(self.float_declared))
                    self.emit(indent, f"{name} = {self.expr(1)}")
                else:
                    self.emit(indent, f"print({self.expr(1)})")
                wrote = True
                continue
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
    def program_mod(self):
        """Modulo-focused programs.

        The whole point of a separate generator is the sign of the dividend.
        C's `%` truncates toward zero and Python's floors, so `a % b` agrees
        only when both are non-negative:

            -7 % 3   C: -1    Python: 2
            -7 % 4   C: -3    Python: 1

        This mode therefore generates NON-NEGATIVE dividends by default, which
        keeps the CPython secondary oracle meaningful and the mode usable in
        CI. --mod-negatives generates the diverging shape, where the
        interpreter/JIT agreement is still enforced but the CPython comparison
        is expected to report the known language gap (the same treatment
        --shared-loop-vars gets).

        Divisors are chosen to hit each emitted shape:
          1, 2, 4, 8, 16, 1024  a power of two -> the `and` + sign-fixup path
          3, 5, 7, 10, 100      imm32 non-power-of-two -> idiv with a constant
          0                      the runtime/modulo-by-zero trap
          a declared variable    idiv reading a register that may be RAX or RDX
        """
        self.lines = []
        self.declared = set()

        # Keep every dividend non-negative unless the caller opted into the
        # diverging shape: the CPython oracle cannot referee those.
        def nonneg_atom():
            if self.declared and self.r.random() < 0.6:
                return self.r.choice(sorted(self.declared))
            return str(self.r.randint(0, 12))

        def dividend_atom(allow_negative):
            if allow_negative and self.r.random() < 0.45:
                return "-" + str(self.r.randint(1, 12))
            return nonneg_atom()

        const_divisors = [1, 2, 3, 4, 5, 7, 8, 10, 16, 100, 1024, 0]
        allow_negative = self.mod_negatives

        # Two live variables, so RAX and RDX are genuinely occupied when the
        # idiv sequence runs -- that is the clobber hazard this mode exists
        # to hit, and it cannot show up if the divisor is a literal.
        x = self.fresh_var()
        y = self.fresh_var(exclude={x})
        self.declare(0, x, nonneg_atom())
        self.declare(0, y, nonneg_atom())

        for _ in range(self.r.randint(4, 10)):
            a = dividend_atom(allow_negative)
            shape = self.r.random()
            if shape < 0.25:
                b = str(self.r.choice(const_divisors))
            elif shape < 0.45:
                b = y                                     # divisor in a register
            elif shape < 0.55:
                b = str(self.r.choice(const_divisors))    # both sides constant
                a = str(self.r.randint(0, 40)) if not allow_negative \
                    else str(self.r.randint(-40, 40))
            else:
                b = str(self.r.choice(const_divisors))
            k = self.r.random()
            if k < 0.55:
                self.emit(0, f"print({a} % {b})")
            elif k < 0.7:
                # `%=` lowers to the same mod instruction but through a store,
                # so the result also has to survive a variable write/read.
                tgt = self.r.choice(sorted(self.declared))
                self.emit(0, f"{tgt} %= {b}")
                self.emit(0, f"print({tgt})")
            elif k < 0.85:
                tgt = self.r.choice(sorted(self.declared))
                self.emit(0, f"{tgt} = {a} % {b}")
                # a print between statements keeps values live across a call
                self.emit(0, f"print({x})")
                self.emit(0, f"print({tgt})")
            else:
                # chained, to nest a modulo inside a larger expression
                self.emit(0, f"print(({a} % {b}) + {nonneg_atom()})")
        return "\n".join(self.lines) + "\n"

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

    def program_bitwise(self):
        """Bitwise and shift-focused programs.

        A separate mode for the same reason --mod is: the interesting shapes
        are operand forms the general generator never produces, and folding
        them into expr() would make every unrelated seed dilute them.

        What this is really reaching for, in order of how badly it has gone:

          * A shift result landing in RCX. RCX is a member of kTempPool on
            POSIX, so the allocator can pick it as a shift's destination, and
            `shl rcx, cl` would shift a register by its own low bits. The
            result has to be computed elsewhere and moved into RCX *after* the
            pop that restores the count. Two count variables plus an
            intervening print is the shape that actually produced that
            allocation.
          * Values live across a call. Every print below sits between
            statements on purpose, so a shift result has to survive being
            spilled or promoted while something else is live.
          * Both encodings, selected by whether the count is a literal or a
            variable. Literal counts in {1} and {small} pick the D1 and C1
            immediate forms; variable counts pick the D3/CL form.
          * The count bounds check itself, in both directions: 0 and 63 must
            work, and anything outside must trap identically on both engines.

        Keeping the CPython oracle usable is a design constraint, not an
        afterthought. `&`, `|`, `^` and `>>` agree with CPython for every
        int64 value -- they are width-preserving, and Python's `>>` floors
        toward negative infinity exactly like `sar`. Only `<<` can disagree,
        by wrapping where Python grows. So every left shift here is bounded so
        its result fits in int64, and the deliberate disagreements are left to
        tests/programs/bitwise_wrap.py. Without that care every generated
        program containing a shift would report interp_vs_cpython and the mode
        would be useless as a CI gate.
        """
        self.lines = []
        self.declared = set()

        # Bit patterns worth hitting: single bits, byte boundaries, all-ones,
        # and the sign bit. Negatives matter as much as positives because
        # &, |, ^ are two's complement and `>>` is arithmetic.
        PATTERNS = [0, 1, 2, 3, 5, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128,
                    255, 256, 1023, 65535, -1, -2, -8, -127, -255, -256]
        # Left-shift counts: small enough that PATTERNS_MAX << 8 still fits in
        # int64, so CPython and Lithon cannot diverge on the result.
        SHIFT_COUNTS = [0, 1, 2, 3, 4, 5, 6, 7, 8]
        # Right-shift counts and bitwise counts have no such constraint.
        WIDE_COUNTS = [0, 1, 2, 7, 8, 31, 32, 61, 62, 63]
        # Counts that must trap at run time. These can only arrive through a
        # variable, never as a literal: a literal out-of-range count is a
        # compile-time RCR error, the frontend refuses the whole program, and
        # the fuzzer skips it without ever comparing the two engines. Holding
        # the bad value in a variable is what turns it into the runtime trap
        # this mode exists to fuzz.
        TRAP_COUNTS = [64, 65, 100, 1000, -1, -100]

        def operand():
            if self.declared and self.r.random() < 0.65:
                return self.r.choice(sorted(self.declared))
            return str(self.r.choice(PATTERNS))

        def small_count():
            """A count expression guaranteed to be in 0..8.

            Only k1/k2 or a literal -- NOT any declared variable. The value
            variables hold PATTERNS, which include 63 and 255; using one as a
            count is exactly the int64-overflowing left shift this mode is
            trying to keep out of the CPython comparison.

            A declared variable rather than a literal, because the variable
            form is the one that takes the D3/CL path where the RCX hazard
            lives; the literal immediate forms get their own shapes below.
            """
            return self.r.choice([k1, k2, str(self.r.choice(SHIFT_COUNTS))])

        def wide_count():
            return str(self.r.choice(WIDE_COUNTS))

        # Two count variables up front, so a body can hold two live shift
        # counts at once. That is what forces the allocator to pick a
        # destination other than the obvious scratch.
        k1 = self.fresh_var()
        k2 = self.fresh_var(exclude={k1})
        self.declare(0, k1, str(self.r.choice(SHIFT_COUNTS)))
        self.declare(0, k2, str(self.r.choice(SHIFT_COUNTS)))
        # Two value variables, so operands are not always constants and the
        # load/shift/store path is reached as well as the const one.
        x = self.fresh_var(exclude={k1, k2})
        y = self.fresh_var(exclude={k1, k2, x})
        self.declare(0, x, str(self.r.choice(PATTERNS)))
        self.declare(0, y, str(self.r.choice(PATTERNS)))

        for _ in range(self.r.randint(5, 12)):
            shape = self.r.random()
            if shape < 0.18:
                self.emit(0, f"print({operand()} & {operand()})")
            elif shape < 0.32:
                self.emit(0, f"print({operand()} | {operand()})")
            elif shape < 0.46:
                self.emit(0, f"print({operand()} ^ {operand()})")
            elif shape < 0.58:
                # Right shift, wide count: always agrees with CPython, so this
                # is the shape that can use the whole 0..63 range.
                self.emit(0, f"print({operand()} >> {self.r.choice([wide_count(), k1, k2])})")
            elif shape < 0.70:
                # Left shift, bounded count so int64 cannot overflow.
                self.emit(0, f"print({operand()} << {self.r.choice([small_count(), k1, k2])})")
            elif shape < 0.80:
                # Augmented forms: these lower through a store, so the result
                # has to survive a variable write and the following read.
                tgt = self.r.choice([v for v in (x, y) if v in self.declared])
                op = self.r.choice(["&=", "|=", "^=", ">>=", "<<="])
                rhs = operand() if op in ("&=", "|=", "^=") else \
                    (wide_count() if op == ">>=" else small_count())
                self.emit(0, f"{tgt} {op} {rhs}")
                # The print here is deliberate: it is what makes the stored
                # value live across a call.
                self.emit(0, f"print({tgt})")
            elif shape < 0.88:
                # Two shifts back to back, results stored. This is the exact
                # shape of the RCX-destination bug: the first shift's result
                # was allocated to RCX and the second shift's count check
                # clobbered it.
                self.emit(0, f"{x} = {operand()} << {small_count()}")
                self.emit(0, f"{y} = {operand()} >> {wide_count()}")
                self.emit(0, f"print({x})")
                self.emit(0, f"print({y})")
            elif shape < 0.94:
                # Nested, so a shift feeds another expression rather than
                # going straight to a print.
                self.emit(0, f"print(({operand()} >> {wide_count()}) & {operand()})")
                self.emit(0, f"print(({operand()} << {small_count()}) ^ {operand()})")
            else:
                # A counted loop, so the unroller and the loop passes see
                # shifts rather than only straight-line code. The mask at the
                # end of the body keeps acc bounded no matter what the body
                # does to it, which is what stops `<<` from overflowing int64
                # and making the CPython oracle useless.
                iv = self.r.choice([v for v in LOOPVARS if v not in self.declared])
                self.emit(0, f"{iv}: int[64] = 0")
                n = self.r.randint(1, 6)
                self.emit(0, f"for {iv} in range({n}):")
                body = self.r.random()
                if body < 0.4:
                    self.emit(1, f"{x} = ({x} + {operand()}) & 255")
                elif body < 0.7:
                    self.emit(1, f"{x} = ({x} << {small_count()}) & 255")
                else:
                    self.emit(1, f"{x} = ({x} >> {wide_count()}) & 255")
                self.emit(0, f"print({x})")

        # Optionally end with a runtime shift trap. This is deliberately last:
        # the trap aborts the process, so anything after it would never run and
        # would silently shrink the coverage of this program. Both engines must
        # agree that it trapped, with the same operator, and evaluate() treats
        # that as a result rather than a CPython language gap.
        if self.r.random() < 0.5:
            self.emit(0, f"{k1} = {self.r.choice(TRAP_COUNTS)}")
            self.emit(0, f"print({operand()} {self.r.choice(['<<', '>>'])} {k1})")
        return "\n".join(self.lines) + "\n"

    def program_phi(self):
        """Merge-focused programs: conditional expressions, run through --ssa.

        A conditional expression is the one program shape that *only* SSA can
        express well. `a if c else b` needs a value defined on both arms and
        read at the join, which is a Phi in every compiler; here it is a
        temporary variable that the SSA pipeline promotes, and Mem2Reg's
        promotion rule, the phi placement, the copy resolution, and the
        register allocator all have to be right for the printed answer to
        match the interpreter's.

        What this reaches for, in order of how badly it has gone:

          * Merges nested in merges. The outer arm's value is itself a merge,
            so the inner one has to be resolved before the outer reads it and
            the block ordering has to survive the resolution.
          * Merges inside loops, so a promoted value crosses a back edge as
            well as a join -- the loop-header phi, whose incoming operand for
            the first iteration comes from outside the loop.
          * Merges over floats. A promoted value's *type* matters, not just
            its id: a phi that is silently treated as an integer prints
            nonsense. Float literals are kept to small decimals so the CPython
            oracle stays usable (see program_bitwise on why that matters).
          * Merges as the right-hand side of an assignment, so the merge block
            has to be closed correctly before the store is emitted into it.

        Merges whose two arms are *different* types are deliberately not
        generated: V1_SPEC is statically typed and the checker rejects them
        (correctly) with "type of '__ifexprN' disagrees across branches", so
        they would only ever be skipped. The type-per-merge case that matters
        is already covered by running this generator in both int and float
        mode -- the same program shape, two different inferred kinds.

        Every value printed is also compared against CPython, so a merge that
        picks the wrong arm is a mismatch rather than a silently wrong
        program.
        """
        self.lines = []
        self.declared = set()
        self.float_declared = set()

        def int_atom():
            if self.declared and self.r.random() < 0.7:
                return self.r.choice(sorted(self.declared))
            return str(self.r.randint(0, 9))

        def float_atom():
            # Small decimals only: exact in binary, so the printed text is
            # identical in both engines and a mismatch means a real bug rather
            # than a rounding difference in the round-trip formatter.
            if self.float_declared and self.r.random() < 0.6:
                return self.r.choice(sorted(self.float_declared))
            return self.r.choice(["0.0", "0.5", "1.0", "1.5", "2.5", "3.5",
                                  "-1.5", "7.0", "10.25"])

        def arith(depth=0):
            atom = float_atom() if self.floats else int_atom()
            if depth >= 1 or self.r.random() < 0.5:
                return atom
            return f"{atom} {self.r.choice(['+', '-'])} {arith(depth + 1)}"

        def condition():
            return f"{arith(1)} {self.r.choice(['<', '>', '=='])} {arith(1)}"

        def merge(depth=0):
            if depth < 2 and self.r.random() < 0.25:
                return f"{merge(depth + 1)} if {condition()} else {merge(depth + 1)}"
            return f"{arith(1)} if {condition()} else {arith(1)}"

        if self.floats:
            a, b = "f", "g"
            self.emit(0, f"{a}: float[64] = {float_atom()}")
            self.emit(0, f"{b}: float[64] = {float_atom()}")
            self.declared = {a, b}
            self.float_declared = {a, b}
        else:
            a, b = VARS[0], VARS[1]
            self.declare(0, a, "0")
            self.declare(0, b, "1")
            self.declared = {a, b}

        self.emit(0, f"print({merge()})")

        # A merge as a call argument: the merge block has to be finished before
        # the call is emitted into it.
        if self.r.random() < 0.5:
            m = self.fresh_var(exclude={a, b})
            kind = "float[64]" if self.floats else "int[64]"
            # Built before m joins the pool: a declaration's own initialiser
            # cannot read the variable it declares, and the checker is right to
            # reject that (0.6.10).
            self.emit(0, f"{m}: {kind} = {a} + ({merge()})")
            if self.floats:
                self.float_declared.add(m)
            else:
                self.declared.add(m)

        # Merges across a back edge, in a loop whose trip count is small and
        # non-trivial (0 and 1 exercise the "never taken" and "taken once"
        # header phis).
        n = self.r.choice([1, 2, 3, 4, 5])
        acc = self.fresh_var(exclude={a, b})
        self.emit(0, f"{acc}: int[64] = 0")
        iv = LOOPVARS[0]
        self.emit(0, f"{iv}: int[64] = 0")
        self.emit(0, f"for {iv} in range({n}):")
        self.emit(1, f"{acc} = {acc} + 1")
        self.emit(1, f"{acc} = {acc} + (1 if {condition()} else -1)")
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

    # A divide/modulo-by-zero trap is a *result*, not a rejection: the
    # interpreter prints "error: interpreter: division by zero" (or "modulo by
    # zero") and exits 1, and the JIT prints the same line and exits 1. Both
    # engines are supposed to do that, so the program is worth comparing -- it
    # exercises the emitted zero test (comisd sets ZF for an unordered compare
    # as well as for an equal one, so the check has to be ZF AND !PF or it
    # traps on a NaN divisor instead). Skipping these would throw away precisely
    # the code most likely to be wrong; --floats generated 21 of them in 300
    # programs.
    #
    # Only those two messages are treated this way. Every other nonzero exit is
    # a typecheck or other rejection, which is not what this fuzzer is about.
    # The two are kept as separate strings on purpose: the interpreter spells
    # them differently and run_tier_diff.py diffs stderr byte-for-byte, so a
    # single combined substring would still catch both.
    #
    # An out-of-range shift count is the same kind of result for a different
    # reason. x86 masks the count to its low 6 bits, so `1 << 64` would
    # silently execute as a shift by 0 and `1 << -1` as a shift by 63; Lithon
    # traps instead of returning the wrong answer. The trap is part of the
    # language, so a program that hits it is a result to compare between the
    # two engines, not a rejection to skip and not a language gap to report.
    interp_stderr = interp.stderr or ""
    is_div_trap = (interp.returncode == 1
                   and ("division by zero" in interp_stderr
                        or "modulo by zero" in interp_stderr))
    is_shift_trap = (interp.returncode == 1
                     and "shift count out of range" in interp_stderr)
    if interp.returncode not in (0, TIMEOUT_RC) and not (is_div_trap or is_shift_trap):
        return Result(True, reason="skip")

    # The diamond unroller is off by default (it measured slower), so --diamond
    # has to opt in or it would fuzz a build that never runs the transform.
    jit_args = [str(TIER_RUNNER), str(tmp_ir), "--auto"]
    if UNROLL_DIAMONDS:
        jit_args.append("--unroll-diamonds")
    # Accumulator unrolling is off by default for the same reason diamond
    # unrolling is: the fast path is the one production runs, so an opt-in
    # transform needs an explicit mode or it never gets exercised. It matters
    # more than usual here -- it REORDERS blocks (the jammed main loop is emitted
    # after the remainder), which is precisely what a liveness model built on
    # textual block ranges gets wrong, silently and only for float
    # accumulators.
    if ACCUM_UNROLL:
        jit_args.append("--accum-unroll")
    jit = run(jit_args)
    if SSA_MODE:
        jit_ssa = run(jit_args + ["--ssa"])
        # A crash in the pipeline is itself the finding, so compare it here
        # rather than letting the CPython oracle explain it away.
        if jit_ssa.returncode != jit.returncode or jit_ssa.stdout != jit.stdout:
            return Result(False, interp.stdout, interp.returncode,
                          jit_ssa.stdout, jit_ssa.returncode,
                          cpy_out=None, cpy_rc=None, reason="jit_vs_interp")
    cpy = run([sys.executable, str(py_path)])
    seen = (interp.stdout, interp.returncode, jit.stdout, jit.returncode, cpy.stdout, cpy.returncode)

    # 1. PRIMARY: the two engines must agree with each other. Checked first so a CPython disagreement
    #    can never mask a JIT bug. Both hanging counts as agreement (partial stdout is not comparable).
    both_hang = interp.returncode == TIMEOUT_RC and jit.returncode == TIMEOUT_RC
    if not both_hang and (jit.stdout != interp.stdout or jit.returncode != interp.returncode):
        return Result(False, *seen, reason="jit_vs_interp")

    # Both engines reached a nonzero exit that looks like a divide trap, so
    # require that BOTH actually report it: one side trapping and the other
    # exiting 1 for some other reason is a divergence even though rc and stdout
    # already matched. The stderr lines are not compared verbatim because
    # tier_runner also writes its "[tier1] native" / "[tier0] interpreter"
    # banner there, which is not part of the program's output.
    #
    # The message text is compared too, not just its presence: a program that
    # traps on modulo in the interpreter must not appear as a division trap in
    # the JIT, and the two spellings are the only way to tell them apart. The
    # shift message is matched whole, operator included, for the same reason:
    # `<<` and `>>` trap with different text and swapping them would be a bug
    # this comparison exists to catch.
    def trap_text(stderr):
        for m in ("modulo by zero", "division by zero",
                  "shift count out of range 0..63 for `<<`",
                  "shift count out of range 0..63 for `>>`"):
            if m in (stderr or ""):
                return m
        return None

    interp_trapped = trap_text(interp.stderr)
    jit_trapped = trap_text(jit.stderr)
    if (interp.returncode != 0 or jit.returncode != 0) and not both_hang:
        if interp_trapped != jit_trapped:
            return Result(False, *seen, reason="jit_vs_interp")

    # 2. SECONDARY: what they agree on must match CPython (a language-semantics gap, not a JIT bug).
    #
    # An out-of-range shift is exempt. CPython has no such rule -- `1 << 64` is
    # a perfectly good 65-bit number there -- so CPython exits 0 with output
    # where Lithon exits 1 with none, and comparing them would report every
    # single generated trap as a "language gap" and bury the real findings.
    # The primary JIT-vs-interpreter agreement above, including that both
    # engines trap with the same operator, is still enforced in full.
    if cpy.returncode == 0 and not (is_shift_trap and interp_trapped == jit_trapped):
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
    ap.add_argument("--accum", action="store_true",
                    help="run the JIT with --accum-unroll (float accumulator splitting)")
    ap.add_argument("--diamond", action="store_true",
                    help="use the small if/else-diamond-focused generator (see Gen.program_diamond)")
    ap.add_argument("--shared-loop-vars", action="store_true",
                    help="generate loop variables that are read/assigned elsewhere (reproduces the known "
                         "for-loop divergence from CPython; see the docstring)")
    ap.add_argument("--floats", action="store_true",
                    help="generate flat float programs (arithmetic, comparisons, printing, int/float mixing)")
    ap.add_argument("--mod", action="store_true",
                    help="use the modulo-focused generator (see Gen.program_mod)")
    ap.add_argument("--bitwise", action="store_true",
                    help="use the bitwise/shift-focused generator (see Gen.program_bitwise)")
    ap.add_argument("--mod-negatives", action="store_true",
                    help="let --mod generate negative dividends, where C's truncating %% and Python's "
                         "floored %% disagree; the CPython oracle then reports the known language gap")
    ap.add_argument("--phi", action="store_true",
                    help="use the merge-focused generator (see Gen.program_phi) and compare the JIT "
                         "with and without the SSA pipeline against the interpreter")
    args = ap.parse_args()
    TIMEOUT = args.timeout
    global UNROLL_DIAMONDS, SSA_MODE, ACCUM_UNROLL
    UNROLL_DIAMONDS = args.diamond and not args.no_diamond_unroll
    ACCUM_UNROLL = args.accum
    SSA_MODE = args.phi

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
        gen = Gen(seed, args.shared_loop_vars, floats=args.floats,
                  mod_negatives=args.mod_negatives)
        if args.phi:
            src = gen.program_phi()
        elif args.floats:
            src = gen.float_program()
        elif args.mod:
            src = gen.program_mod()
        elif args.bitwise:
            src = gen.program_bitwise()
        elif args.diamond:
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
