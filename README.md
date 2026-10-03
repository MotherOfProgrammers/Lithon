<div align="center">

<img src="assets/boa.png" alt="Lithon Logo" width="380" />

# Lithon

### Give Python wings. Bare-metal speed with zero external dependencies.

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen?style=flat-square&logo=github-actions)](https://github.com/Project-Lithon/lithon/actions)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Architecture](https://img.shields.io/badge/arch-x86--64-red?style=flat-square)](https://en.wikipedia.org/wiki/X86-64)
[![Dependencies](https://img.shields.io/badge/third--party_deps-zero-success?style=flat-square)](#-quickstart)
[![Typing](https://img.shields.io/badge/typing-mandatory%20static-green?style=flat-square)](#-what-is-lithon)
[![License](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)

</div>

---

<p align="center">
  <img src="assets/web-banner.png" alt="Lithon docs landing page" width="900" />
</p>

---

## 🚀 What is Lithon?

Lithon is a **zero-dependency, dual-tier execution engine** for a statically verified, Python-flavored language built for bare-metal performance.

It looks like Python. It reads like Python. But under the hood, Lithon makes a strict promise: **every variable has a proven, fixed type before execution begins.** No dynamic dispatch, no boxing integers on the heap, and no interpreter overhead on critical paths.

Rather than linking heavy compiler frameworks like LLVM or Cranelift, Lithon’s custom C++ engine compiles typed AST blocks directly into **raw, guard-free x86-64 machine instructions** and executes them in memory via native OS page allocation (`mmap` / `VirtualAlloc`). 

> **The Lithon Promise:** If a type flow cannot be mathematically proven safe and static, Lithon will refuse to compile it—ensuring slow execution paths never exist in your binary.

---

## ⚡ Quickstart

Lithon has **no third-party dependencies** — no LLVM, no runtime library, nothing
to `pip install` to make the engine work. It does need a toolchain and a
language, though, so the "zero dependencies" claim is about the *engine's*
inputs, not its build:

- CMake ≥ 3.20 and a **C++20** compiler (tested with GCC 12 and Clang 17)
- Python ≥ 3.10 — only for the compile step, which turns `.py` into IR. Once IR
  exists, the native program never touches CPython: no objects, no refcounting,
  no GC.

```bash
# 1. Clone
git clone git@github.com:Project-Lithon/lithon.git
cd lithon

# 2. Build the C++ engine. There is no Makefile; CMake drives everything.
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 3. Compile a .py program to IR, then run it on the native tier.
#    The frontend is the only step that involves Python.
python3 src/frontend/frontend.py tests/programs/float.py > /tmp/float.ir
./build/tier_runner /tmp/float.ir --strict
```

```text
[tier1] native
0.3333333333333333
0.30000000000000004
1.0
-0.0
1.0000000000000002
...
```

The `[tier1] native` line goes to **stderr**; the program's own output goes to
stdout. `--strict` means "native only, refuse rather than fall back", so a
successful exit status is proof the code was really emitted and executed —
that is the flag to use when testing, because `--auto` can hide a total
fallback to the interpreter behind correct output.

### The `lithon` command

`pip install -e .` adds a `lithon` wrapper that does the two-step dance for you.
It is optional — the engine works without it — and it installs into whichever
interpreter you point `pip` at, so check `lithon --version` agrees with the
`python3` you expect.

```bash
pip install -e .
lithon tests/programs/float.py            # auto: native when provably safe
lithon tests/programs/float.py --strict   # native only, rc=3 if refused
lithon tests/programs/float.py --ir       # print the IR and stop
lithon tests/programs/float.py -v         # show which tier ran, and why
```

`init.py` finds the frontend and `build/tier_runner` relative to the repo, and
honours `LITHON_HOME` and `LITHON_RUNNER` if you need to point it elsewhere.

> **Two layouts, one of them dead.** The tree carries a package layout
> (`__init__.py`, `__main__.py`, where `_ROOT` is the repo's *parent*) alongside
> the flat layout actually installed (`init.py`, `main.py`, where `_ROOT` is the
> repo root). `pyproject.toml` wires the flat one, so the package pair is dead
> code and `import lithon` raises `ModuleNotFoundError`. The `lithon` *command*
> works; the `lithon` *module* does not yet. Worth collapsing to a real
> `lithon/` package.

---

## 🏗️ Architecture: The 4-Language Integration Matrix

Lithon operates across four precise architectural layers, using each language exclusively where it excels:

| Layer | Language | Primary Responsibility |
| :--- | :--- | :--- |
| **Frontend** | Python | Expressive syntax, user logic, AST generation target |
| **Engine** | C++ (C++20) | AST parser, static flow verifier, type checker, memory manager |
| **Bridge** | C | SysV & Win64 ABI calling convention alignment |
| **Backend** | x64 Assembly | Bare-metal machine code generation (hand-encoded opcodes) |

```mermaid
graph TD
    A[Python Source Code] --> B[Lithon Static Flow Verifier]
    B -->|Verified Static Types| C[Tier-1: Hand-Rolled x64 JIT Emitter]
    B -->|Dynamic Operations / Unhandled I/O| D[Tier-0: C++ Fallback Interpreter]
    C --> E[mmap PROT_EXEC Executable Memory Buffer]
    E --> F[Direct CPU Execution]
    D --> G[C++ Native Execution]
```

---

## 🆚 How is Lithon Different?

| Feature | CPython | Cython / mypyc | PyPy | **Lithon** |
| :--- | :--- | :--- | :--- | :--- |
| **Primary Target** | Dynamic Bytecode | C Extension Source | Tracing JIT | **Bare-Metal x64 Machine Code** |
| **Typing System** | Dynamic | Optional / Annotated | Dynamic Tracing | **Mandatory Static** |
| **External Dependencies** | CPython Runtime | C/C++ Compiler | Heavy JIT Runtime | **Zero (Self-Contained Engine)** |
| **Startup Overhead** | High | Medium | Very High (Warmup) | **Instant (< 1ms)** |
| **Memory Allocation** | Boxed Heap Objects | Partial Unboxing | Traced Heap | **Raw Stack Registers / Unboxed** |
| **Memory Page Control** | None | OS Standard | Runtime Managed | **Direct `mmap` / `VirtualAlloc`** |

---

## 🎯 Primary Use Cases

Lithon is purpose-built for low-latency tasks where Python traditionally relies on external C/C++ wrappers:

- 🔢 **Numeric & Algorithmic Loops:** High-throughput mathematical sequences, signal processing, and matrix transformations.
- ⚡ **Quantitative Systems & Trading Logic:** Microsecond-level strategy execution without garbage collector pauses or JIT warmup latency.
- 🎮 **Performance-Sensitive Engine Tooling:** High-frequency game logic, spatial partitioning, and real-time data pipelines.
- 🛡️ **Predictable Micro-Utilities:** Deterministic, fixed-schema processing where memory and execution bounds must be guaranteed prior to runtime.

---

## 🧭 Roadmap to AOT Binary Synthesis

```text
  Phase I (Current)            Phase II (Next)             Phase III (Future)
┌──────────────────────┐    ┌──────────────────────┐    ┌──────────────────────┐
│ Dual-Tier JIT Engine │───►│ AOT Binary Synthesis │───►│ Zero-Copy C-FFI      │
│ In-memory mmap x64   │    │ Standalone <10KB ELF │    │ Direct Syscall Ops   │
└──────────────────────┘    └──────────────────────┘    └──────────────────────┘
```

- [x] **Phase I: Dual-Tier JIT Architecture (V1)**
  - Hand-rolled x86-64 machine code emitter in pure C++20.
  - Direct execution via `mmap` (`PROT_READ | PROT_EXEC`) and `VirtualAlloc`.
  - Static type flow analysis with deterministic Tier-0 interpreter fallback.
  - Full SysV and Windows x64 ABI compliance for C-level call compatibility.

- [ ] **Phase II: Ahead-Of-Time (AOT) Binary Compiler (V2)**
  - Direct ELF64 (Linux) and PE32+ (Windows) header synthesis.
  - Compile Python scripts into **standalone native executables (`.bin` / `.exe`) under 10 KB**.
  - Complete decoupling from the Lithon interpreter engine for standalone deployment.

- [ ] **Phase III: Systems & Hardware Integration (V3)**
  - Direct `syscall` (`0x0F 0x05`) instruction emission from Python syntax.
  - Zero-copy C pointer exposure and raw memory array mutation.
  - AVX2 / SIMD vectorization for parallel list processing.

---

## 📍 Roadmap: Where Lithon Stands Today

*Current status, Phase I (Dual-Tier JIT). "Shipped" means enforced by a test that
fails when it regresses — not merely present.*

### Shipped and enforced

| Area | Status | Evidence |
| :--- | :--- | :--- |
| Hand-rolled x86-64 encoder | Shipped | `encoder_test`, `branch_test`, `stack_test`, `tools/check_encoder_vs_as.py` |
| SysV x64 ABI | Shipped | `src/jit/jit_abi.h`; `check_stack_alignment.py` (with `--self-test`) disassembles every function and proves callee-saved + 16-byte stack alignment at every call/ret |
| Windows x64 ABI | Implemented, **not yet verified** | `#if defined(_WIN32)` in `jit_abi.h`; the audit only exercises the host ABI, so the Win64 path has no test evidence yet |
| CPU target-feature detection | Shipped | `src/jit/cpu_features.h`; `cpu_features_test` (30 assertions) + `check_cpu_features.py` require CPUID to agree with the kernel (`/proc/cpuinfo`), so the AVX/AVX-512 gate cannot silently mis-detect |
| VEX/SSE transition discipline | Shipped (guardrail) | `tools/check_vex_transitions.py` disassembles each function and flags legacy SSE after VEX with no `vzeroupper`; no AVX is emitted yet, and `--self-test` proves the scanner fires |
| Tier-0 interpreter fallback | Shipped | `tier_runner --auto` falls back on unprovable output (`print_guard.h`) |
| Static type flow verifier | Shipped | `tools/typecheck.py`, `run_typed_regression.py` (13/13) |
| Conditional expressions | Shipped | `a if c else b`, nested; lowers to an SSA merge — `tests/programs/if_expr.py`, `tests/typed_regression/if_expr.py`, `fuzz_diff.py --phi` |
| Liveness + register allocation | Shipped | `liveness_test`, `regalloc_test` |
| IR text format | Shipped | `src/ir/text_parser.cpp` — no Python dependency in the engine |
| Function calls, recursion, TCO | Shipped | `compile_module_call_test`, `fib_test`; self-tail-calls become loops, O(1) stack |
| Bitwise ops and shifts | Shipped | `& \| ^ << >>`, int-only; `check_encoder_vs_as.py` proves all 5 encodings against GNU as, `typecheck_test` pins the static rules, `tests/programs/bitwise.py` is a pure CPython-agreement test |

### Optimization pipeline (this is the active work)

Every optimization is independently switchable, so its effect can be measured
rather than assumed.

| Change | Lives in | Isolated measurement vs. HEAD |
| :--- | :--- | :--- |
| `strength_reduce_multiplies` — invariant × induction-variable → repeated add | `optimize.h` | **nested 1.062×** |
| `accumulator_unroll` — split a counted reduction's accumulator into N partials | `optimize.h` | **opt-in** (`--accum-unroll`); **float reduction 1.58×** (95.8 → 60.5 ms), integer `sum 20M` roughly neutral (within noise) — for a float accumulator it reassociates the adds, so the value is not bit-identical to the interpreter's |
| Callee-saved borrowing — temps live across a call borrow a callee-saved register instead of the stack | `register_alloc.h` | **fib 1.063×** |
| Shared virtual-temp liveness — one `VirtualTemps` set, excluded *before* live ranges are computed | `liveness.h` | correctness, not speed |
| `fold_constants`, `eliminate_dead_code`, `convert_self_tail_calls` | `optimize.h` | bundled, not isolated |
| `mem2reg` + SSA copy resolution (`--ssa`) | `ssa.h` | correctness, not speed — removes 29 loads / 20 stores on `tests/typed_regression/if_expr.py` |

Measured on an Intel i3-3110M (Sandy Bridge), 12 interleaved rounds, CPU-time
clock, one pinned core. See [Testing](#-testing) to reproduce them yourself.

### The SSA pipeline, phase by phase

The memory-based JIT is being converted to SSA one phase at a time, each phase
independently switchable so its effect can be measured rather than assumed. The
IR keeps mutable variables in memory — a variable is written by `Store` and read
by `Load`, exactly like a spill slot — so "promotion" here means turning those
into values that live in registers.

| Phase | What it adds | Lives in | Evidence |
| :--- | :--- | :--- | :--- |
| 2.1 | CFG, Cooper-Harvey-Kennedy dominators, natural loops, preheaders, latches, exits | `loop_info.h` | `loop_info_test`, `loop_canon_test` |
| 2.2 | Phi placement — iterated dominance frontier over each variable's def blocks (Cytron et al.) | `ssa.h` | `ssa_test` |
| 2.3 | Mem2Reg — promote promotable variables, place their phis, rewrite loads/stores away | `ssa.h` | `mem2reg_test` |
| 2.4 | SSA copy propagation + dead-value elimination, then `resolve_phis()` | `optimize.h` | `optimize_ssa_test` |
| 2.5 | Phi copies as register moves — each merge's edge stores become `mov reg, reg` instead of a memory round-trip | `register_alloc.h` | `phi_register_test` |
| 2.6 | Dedicated loop-exit-block synthesis — a loop exit that lands on a block something else can also reach gets its own block | `loop_info.h` | `loop_canon_test` |
| 2.7 | CFG liveness + interference-graph allocation, replacing the flat instruction-span model | `liveness.h` | `liveness_test`, `regalloc_test` |
| 2.8 | Register coalescing (IRC) — let a merge's destination reuse its source's register | both halves are done: `forward_trivial_phis` deletes same-operand merges, and `coalesce_phi_registers` gives a merge a dead source's register (5 across the corpus) |

Two decisions in that sequence are load-bearing, and both are the kind that
look like details until they produce wrong answers:

**Promotion is gated on a must-analysis, and the analysis iterates downward.**
A variable is promoted only if it is *definitely assigned* at every load and at
every planned Phi's incoming edge. The dataflow meets over predecessors by
intersection, and it iterates **down from "everything"** rather than up from
nothing, which converges on the greatest fixpoint. That is not a stylistic
choice: iterating upward, a variable assigned in a preheader and read inside the
loop collapses at the header to the meet of `{preheader}` and `{back edge}`, the
back-edge set never picks it up, and ordinary loop accumulators would be
silently un-promotable forever. Anything the analysis cannot prove stays in
memory, and `vars_declined` reports it rather than letting it pass unnoticed.

**`resolve_phis()` is a lowerer, not an emitter.** Because no codegen path emits
`Op::Phi`, resolution stores each incoming value on its predecessor *edge* and
turns the Phi itself into a load. This is what makes conditional expressions
work today, and it is correct — `--ssa` is differential-fuzzed against the
interpreter — but the merged value still round-trips through memory.

`find_loops()`/`LoopSpan` and the flat-span view they backed are **gone**. They
approximated a loop by the contiguous flat instruction span `[header, latch]`,
which is a superset when the body happens to be laid out contiguously and an
*underset* when it is not — and the underset was the dangerous direction, since
`extend_across_loops()` then failed to carry a value across the back edge past a
body block the span missed, making the value look dead where it was still live.
Phase 2.7 moved the allocator onto real CFG dataflow at the same time as those
consumers moved to `compute_loop_info()`, so the imprecise view no longer has
any callers to be wrong for.

One prerequisite of the emitter is landed, and one phase is half written:

| Phase | Would add | Why it is not already done |
| :--- | :--- | :--- |
| emitter | A direct `Op::Phi` case, so a merge is an opcode instead of a memory round-trip | **its liveness prerequisite is done**: `compute_edge_uses()` gives a Phi's operands to the predecessor they arrive on, instead of filing them as uses in the join. Still owed: value-kind inference for a float merge, an allocator slot for the result, and codegen for the join's parallel copies — and that last one is the same interference problem as 2.8, so the emitter relocates that work rather than shrinking it |
| 2.8 | Register coalescing (IRC) — let a merge's destination reuse its source's register | one half is done: `forward_trivial_phis()` deletes a merge whose operands all agree, so that copy set never exists (`ssa.h`, `ssa_test`). The other half is done too, and it had to be moved: `coalesce_phi_registers()` runs *after* the temporaries are coloured, since the register it claims is a temporary's and only allocation knows it. On the typed regression corpus it deletes 5 copies. Most merges still cannot benefit, and the write-up below says exactly why, by category. |

### Known gaps — the honest list

- **`Phi` is still lowered, not emitted.** The encoder has no `Op::Phi` case, so
  `resolve_phis()` turns a merge into a store on each *predecessor edge* plus a
  load at the join. Phase 2.5 then gives that variable a dedicated register, so
  the lowering disappears: the store and the load become `mov reg, reg`. What
  remains is the shape, not the cost — the copies are still sequenced, and a
  destination pinned to its own register can never reuse a dead source's. That is
  2.8's job, and it is done — see below.
- **The one kind of merge that costs nothing is already handled, and mostly by
  something older.** `forward_trivial_phis()` deletes a Phi whose operands all
  name the same value, which is what `c ? 1 : 1` becomes after propagation — a
  copy set that never had to exist, and a promotion-pool register never spent on
  it. Worth being precise about the payoff: on every shape the frontend currently
  produces, `copy_propagate()` gets there first, so this pass fires on none of
  them. It is a guard for a case the existing passes do not currently create, not
  a speedup — the unit tests construct that case by hand, since `phi` is an
  internal opcode the text parser cannot express. The guard is the interesting
  part: forwarding is refused unless the operand's definition dominates the join,
  and `ssa_test` pins that by building a Phi whose operand is *not* in scope on
  one of its own incoming edges.
- **A merge's operands are live on an edge, which is not a place block-granular
  liveness has a term for.** The operand of a Phi in the join is read at the
  instant the predecessor's branch is taken — later than every ordinary use in
  that block. Filing it as a use *of the join* is wrong in both directions at
  once: it inflates the value's range backwards across the whole join, and it
  never records the thing that matters, that the value must stay alive to the
  end of the predecessor. A value whose last real use precedes the branch can
  then look dead, have its register reused by the very next instruction, and be
read clobbered by the copy — a silent wrong answer. `compute_edge_uses()`
   carries these per-edge instead, and `liveness_test` pins both halves: two
   operands sharing a predecessor *do* interfere, because both are needed at its
   end, while operands of *different* edges do not — the non-interference a
   coalescer will need.
- **A merge's destination can take over its source's dead register, which deletes
   that edge's copy outright.** `RegisterAllocator::coalesce_phi_registers()` runs
   *after* the temporaries are coloured, because the register being claimed is a
   temporary's and only allocation knows it — so every condition is a question
   about the interference graph, and the graph exists by then. For each merge it
   asks whether some incoming edge's source is a real register that nothing wants
   after that edge's store retires; if so the destination is given that register,
   and `materialize_into` elides the `mov` because source and destination are now
   the same register. The safety argument is one comparison: `last_use == the
   store`'s index means the source is dead the instant the copy retires, so the
   only thing left wanting the register is the merge itself. Four other conditions
   back it up — the source must be a register at all (a constant is virtual, and
   the copy that remains is an immediate, which is the cheapest form there is),
   the merge must not outlive a call unless the register is callee-saved, no other
   value's range may touch the merge's extent, and two merges may not claim one
   register. `phi_register_test` pins all three shapes: a dead arithmetic source
   coalesces, a source read again after the join does not, and constant sources do
   not. Mutating the `last_use` guard breaks the negative test, so the guard is
   load-bearing; mutating the clash guard breaks nothing, because with three
   temporary registers the corpus never puts two values in one register across a
   merge — that check is defence, and no test exercises it yet.
   **Honest size of the win: 5 merges across the typed regression corpus.** Most
   merges cannot benefit. `mem2reg()` runs first, so a merge's arms are already
   SSA values, and the ones that arrive are constants, another merge's value, or an
   arithmetic result still live somewhere else — the first has no register to
   adopt, the second means relocating a whole merge, and the third is refused.
- **The register budget bounds it.** A Phi copy gets a callee-saved register, of
  which there are five, and real variables rank for them first by loop-depth
  weight. So a merge-heavy program gets every copy in a register, while a
  function with more merges than registers keeps the memory path for the
  remainder — still correct, just not yet free. `--stats` reports the split as
  `phi copies: N of M in registers`; a shortfall is the budget, not a defect.
- **A merge carrying a double is never promoted.** A general-purpose register
  cannot hold a double, so those copies stay in memory by the same rule that
  governs every other float value. Closing that needs an XMM pool, which is
  2.7's.
- Net: **25 of the 26 IR opcodes are emitted.** The one missing one is `Phi`,
  still lowered by `resolve_phis()`.
- **Conditional expressions work, via that round-trip.** `a if c else b` has no
  stack-based lowering of its own: the frontend gives the result a compiler-
  generated temporary (`__ifexprN`), both arms store it, the join loads it, and
  Mem2Reg promotes exactly that pattern into a real Phi. The temporary has no
  annotation because it has no source variable, so the type checker infers its
  type from the arms — and rejects the program when the two arms disagree, since
  `1.5 if c else 0` is not "a float or an int", it is two different types.
  Nothing is left on the floor here; the merge is genuinely what SSA is for.
- **`--accum-unroll` reassociates float sums.** Splitting a float reduction's
  accumulator changes the rounding, so with that opt-in flag the native value
  can differ from the interpreter's (and from CPython's). The default pipeline
  never does this, and the tier-diff gate runs with the pass off, so bit-exact
  parity holds everywhere except when the flag is explicitly passed.

- **A loop exit that only the loop reaches is not given its own block.** Phase
  2.6 synthesizes a dedicated exit block only where it can do work: when the
  exit's target block has another predecessor, so the edge is genuinely shared.
  A target the loop alone reaches already *is* the exit block, and splitting it
  would add a block that jumps to a block that jumps onward — pure block-count
  inflation, and `loop_canon_test` asserts the pass declines to do it.

### `%` is C's `%`, not Python's

This is the one place where Lithon deliberately does **not** follow Python, so
it is worth stating plainly rather than leaving to a comment in `ir.h`.

```python
print(-7 % 3)   # Lithon: -1     CPython: 2
print(7 % -3)   # Lithon:  1     CPython: -2
```

`%` truncates toward zero and takes the sign of the **dividend**, which is what
C, Rust, Java and every other compiled language do. CPython floors instead, so
its remainder has the sign of the **divisor**. Both engines here implement the
truncating rule, which is what makes the tier diff a meaningful check rather
than two engines agreeing on a shared mistake.

The reason is the one C gives: a remainder never leaves the domain of its
operands, so `Mod` is typed like `Mul` (int iff both operands are int) rather
than like `Div`, which must widen to float because a quotient generally is not
an integer. Typing it as `Div` would make `7 % 3` a `float` and lose the point.

Consequences worth knowing, all covered by tests:

- A zero divisor **traps** on both engines, like `Div`, with its own message
  (`modulo by zero`) so the two are distinguishable in a diff.
- `INT64_MIN % -1` is `0`. It is the one input that makes hardware `idiv` raise
  `#DE`, since the quotient would be 2⁶³, so the divisor is tested up front and
  the whole division collapses to a zero.
- Float `Mod` has **no SSE2 instruction** — `fmod` is a libm call, and calling
  one per modulo would be far more expensive than the interpreter this is
  meant to be replacing. It is computed as `a - n*b` for `n = trunc(a/b)`, the
  definition C uses. The interesting case is `n == 0`, which happens exactly
  when `|a| < |b|`, and it is the *only* case where the multiply can go wrong:
  IEEE makes `0 * inf` a NaN, but `n*b` is 0 for every `b` when `n` is 0, so
  C's `fmod(1.0, inf)` is `1.0`. The guard that skips the multiply has to
  distinguish a real zero quotient from a NaN one using the same `ZF AND !PF`
  shape as the zero-divisor check, because `comisd` sets ZF for an unordered
  compare too.
- A zero remainder is normalized to `+0.0` to match CPython, which does not
  preserve the dividend's sign for a zero remainder. C's `fmod(-4.0, 2.0)` is
  `-0.0`; Lithon prints `0.0`. This is a conscious divergence, chosen so the
  float and integer paths agree with each other.

Integer `Mod` is also strength-reduced where it is exact: a constant divisor
that is a power of two becomes a mask plus a sign fixup, since `-7 & 3` is `1`
and not the `-3` that `-7 % 4` has to return.

### Shifts are 64-bit, and their counts are checked

`&`, `|`, `^`, `<<` and `>>` are integer-only, and each is a plain 64-bit
machine operation. `>>` is arithmetic (`sar`), so `-8 >> 1` is `-4`.

Three rules follow from "the machine word is 64 bits wide", and all three
diverge from CPython, whose ints are unbounded:

```python
print(5 << 63)     # Lithon: -9223372036854775808   CPython: 46116860184273879040
print(5 << 62)     # Lithon:  4611686018427387904   CPython: 4611686018427387904
print(1 << 64)     # Lithon: RCR error              CPython: 18446744073709551616
```

- **Left shifts wrap.** `5 << 63` is `2**63 + 2**65`; only the low 64 bits
  survive, which is `-2**63`. Nothing is trapped and nothing is auto-widened,
  because there is no wider type to widen to. `5 << 62` happens to agree with
  CPython — that is arithmetic luck, not a rule.
- **A count outside `0..63` is refused**, at compile time when the count is a
  literal and at run time when it is not. x86 masks the count to its low 6
  bits, so `1 << 64` would execute as a shift by `0` and return `1`, and
  `1 << -1` would shift by `63`. Lithon raises an error rather than returning
  the wrong answer. `1 << 64` is a program that cannot run.
- **No float punning.** `2.5 & 1` is a type error. There is no float bit
  pattern in Lithon to reinterpret, and inventing one would mean `&` means two
  different things depending on its operand types.

The result width is the **left** operand's width, so `a & b` with `a: int[8]`
is an `int[8]` and may be stored without narrowing. A shift into a target too
narrow to hold the result is rejected statically when the count is a constant
(`100 << 3` is `800`, which is not an `int[8]`).

The encodings are the ones x86 actually has, and which one is used depends on
whether the count is known at compile time — visible only in the generated
code:

```console
$ build/tier_runner prog.ir --dump-hex   # then: objdump -D -b binary -m i386:x86-64
shl rax,0x3      # 48 c1 e0 03   literal count 3, never touches CL
shl rax,1        # 48 d1 e0      literal count 1, the 2-byte short form
shl rax,cl       # 48 d3 e0      dynamic count
sar rax,cl       # 48 d3 f8      dynamic count
```

A dynamic shift has to be careful about RCX, which is a member of `kTempPool`
on POSIX and so can be holding the shift's own result. `shl rcx, cl` would
shift a register by its own low bits, so the result is computed in R10 and
moved into RCX — *after* the `pop` that restores the count, or the `pop`
overwrites the result with the count itself. `run_tier_diff.py`'s
`adversarial/dynamic_shift_rcx_dst` pins that exact allocation.

### Floating point, and what "identical" had to mean

`float` is implemented end-to-end: `ConstFloat`, load/store, `Add`/`Sub`/`Mul`/
`Div`/`Mod`, the three comparisons, and native `print()`. Both tiers now run
`float.ir`, `mixed_numeric.ir` and `comparison.ir` natively, with
`run_tier_diff.py` reporting 37/37 native and zero interpreter fallbacks.

The hard part was not the arithmetic — SSE2 is straightforward once the
encoding is right — it was making the two engines agree *byte for byte*, since
that is the property everything else is measured against:

- **Formatting is one function, called by both.** `host_format_double()` in
  `float_runtime.h` is what emitted code calls and what the interpreter calls.
  CPython's rule is the *shortest string that round-trips*, so neither `"%f"`
  (which prints `3.500000`) nor `"%.17g"` (which prints `0.10000000000000001`)
  is acceptable. `%.*g` is also wrong in a way that is easy to miss: it chooses
  exponent notation based on the *precision it needed*, whereas CPython's
  threshold is absolute — decimal exponent below −4 or above 16. That is why
  `924966630.0` must print in full, not as `9.2499663e+08`.
- **`Div` by zero traps on both engines**, matching Python's
  `ZeroDivisionError` rather than IEEE `inf`/`nan`. The check cannot be a
  single branch: `comisd` sets ZF, PF *and* CF together when the operands are
  unordered, so ZF alone cannot separate "equal" from "NaN". The emitted code
  computes `ZF AND !PF` in a GP register instead. A **NaN divisor must not
  trap** — Python propagates — and `-0.0` must, since it compares equal to `0.0`.
- **NaN compares false against everything**, including itself. The same
  unordered-flag problem applies to `Lt`/`Gt`/`Eq`, and is excluded with
  `setcc` + `AND setnp` rather than a parity branch: `0F 9A` is a byte-for-byte
  collision between `jp rel32` and `setp r/m8`, so a parity `Jcc` is not
  encodable here.
- **A float live across a call spills.** Every XMM in the temp pool is
  caller-saved on both ABIs, and `host_format_double` is an ordinary C function
  that clobbers all of them, so leaving a float in one across a `call print`
  silently corrupts it.
- **The guard refuses a variable stored both an `int` and a `float`.** Its kind
  joins to `Unknown`, and codegen only asks `is_float_value` — so it would lower
  the arithmetic as *integer* operations over a double's bit pattern. Printing
  the resulting `bool` hides this, because a comparison is always `bool` and so
  always passes the print check.

These are pinned by `float_format_test` (CPython `repr` transcribed by hand),
the SSE2 byte-exact assertions in `encoder_test`, and a dedicated
`tools/fuzz_diff.py --floats` mode — the general fuzzer annotates every variable
`int[64]` and so never reached any of it.
- **Arguments are capped at 2 per function and per call.**
- **Branchy loop bodies are not unrolled.** The unroller is implemented,
  correct, and fuzzed — but it measured **1.10× slower** on an if/else loop
  (1.05× with a heavier body), because a diamond's if/else test is irreducible
  and unrolling only inflates the loop ~2×. It is therefore **opt-in** behind
  `--unroll-diamonds`, not deleted.
- **No AOT backend.** Everything runs in-process; there is no `.bin`/`.exe` emit.
- **x86-64 only.** No ARM64 backend.

### Next

1. **`Phi`** — the last unemitted opcode, needed for `if`-as-expression
   lowering once both arms must merge without a stack round-trip.
2. **Lift the 2-argument cap** — most remaining test programs are blocked on it.
3. **ARM64 backend** — the genuinely arch-agnostic layers are `ir/`, `liveness.h`,
   and `optimize.h` (they name no registers at all). `register_alloc.h` names
   registers only via `abi::kPromotionPool`. The x86-specific surface is
   `x86_encoder.h` plus the emit calls in `compile_function.h`. `float_runtime.h`
   is in that last group only in the sense that its formatter is shared — the
   arithmetic and its `ZF AND !PF` zero test are not.
4. **AOT emit** — Phase II below.

> **Note:** Phase I is a work in progress. The engine is fast and well-tested on
> the subset it supports, and it **refuses** what it cannot prove — that refusal
> is the feature, not a workaround.

---

## 🧪 Testing

Work outward and stop when you are satisfied. Each layer is roughly an order of
magnitude slower than the one above it.

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

If you are on a machine with a small `/tmp` (a 100 MB tmpfs is enough to fail
this), point the compiler's scratch space somewhere roomier:

```bash
TMPDIR=/path/to/scratch cmake --build build -j$(nproc)
```

### Layer 1 — unit tests (~0.1 s)

```bash
ctest --test-dir build --output-on-failure        # 28/28
```

The tests are not all the same kind, and it is worth knowing which is which:

- **`liveness_test`, `regalloc_test`** — pure analysis tests. They build IR and
  call the pass directly, and never emit a byte. A green run here means the
  data structures are right, not that any machine code ran.
- **`loop_canon_test`, `ssa_test`, `mem2reg_test`, `optimize_ssa_test`** — the
  same shape, one level up: CFG/dominator analysis, Phi placement, promotion
  (including the cases that must *decline*), and the SSA copy/DSE passes. They
  assert structure rather than values, which is what catches "the merge is in
  the wrong block" — the failure mode a differential test reports only as a
  wrong number.
- **`compile_function_*`, `compile_module_*`, `optimize_lsr_test`,
  `optimize_accum_test`** — compile a module to machine code, mmap it, and call
  it through a function pointer, checking the returned values. These are the
  tests that would catch a bad encoding. `optimize_accum_test` also asserts the
  structural shape the accumulator pass must produce (one terminator per split
  block, the partial stores before it), which is what a dead-clone regression
  would violate.
- **`cpu_features_test`** — no IR and no machine code: it checks the CPUID
  parsing and the `usable_avx`/`usable_avx512` gating against pinned raw
  feature bits and a `--dump` of the host, so the AVX/AVX-512 gate can be
  trusted on machines the CI host is not.
- **`encoder_test`, `stack_test`, `branch_test`, `print_guard_*`** — the
  x86/ABI layer underneath, tested in isolation.
- **`float_format_test`** — pure computation, no JIT, no interpreter. Pins
  `host_format_double` against CPython's `repr` with expectations *transcribed
  by hand* rather than generated from the code under test, since generating
  them would only prove the formatter agrees with itself.
- **`compile_module_float_test`** — needs real machine code, so it is
  POSIX-only and uses `fork`: a float live across a call, a NaN divisor that
  must propagate, and `0.0` / `-0.0` divisors that must trap. The last two run
  in a child process because the trap handler calls `exit(1)`.

### Layer 2 — the full gate (~2 min)

```bash
bash tools/verify_all.sh                         # ctest, encoder-vs-as, CPUID, VEX, regressions, tier-diff, fuzz, ABI audit
python3 tools/run_regression.py                  # 15/15 untyped programs
python3 tools/run_typed_regression.py            # 13/13 typed programs
python3 tools/run_tier_diff.py                   # 39/39
```

`run_tier_diff.py` is the highest-value of the four. It runs every program
through **both** tiers and requires byte-identical stdout, and it reports which
tier actually ran — so a green run cannot hide "everything silently fell back
to the interpreter".

### Layer 3 — differential fuzzing (~5 min per mode)

```bash
python3 tools/fuzz_diff.py --count 300             # general programs
python3 tools/fuzz_diff.py --count 300 --lsr       # strength-reduction shapes
python3 tools/fuzz_diff.py --count 300 --diamond   # diamond-unroll shapes
python3 tools/fuzz_diff.py --count 300 --floats    # int/float mixes, div, calls
python3 tools/fuzz_diff.py --count 300 --bitwise   # & | ^ << >>, both encodings, count bounds, RCX hazard
python3 tools/fuzz_diff.py --mod --count 300       # modulo, any signs
python3 tools/fuzz_diff.py --mod-negatives --count 300   # modulo, negative operands
python3 tools/fuzz_diff.py --phi --count 300       # conditional-expression merges, run with AND without --ssa
python3 tools/fuzz_diff.py --phi --floats --count 300    # ...over float merges
python3 tools/fuzz_diff.py --accum --count 300      # accumulator-unroll shapes (--accum-unroll)
```

The JIT's output is compared against the interpreter, and the interpreter's
against CPython — reported **separately**, because Lithon deliberately diverges
from CPython for loop variables (`v == n` after a loop, not `n - 1`), so a
CPython disagreement is not by itself a bug in the JIT. The `--lsr` and
`--diamond` modes exist because the general
generator almost never reaches those two passes; without them they would be
essentially untested. `--floats` exists because the general generator annotates
every variable `int[64]` and so emits no `const_f64` at all — that mode found a
real miscompile (an `Unknown`-kind operand lowered as *integer* arithmetic) that
four general modes had never approached.

`--mod-negatives` is the mode to reach for when changing modulo, with one caveat:
because Lithon's `%` truncates and CPython's floors, most of its CPython
disagreements are *expected* language gaps rather than bugs, and the mode reports
them separately. The number that must stay at zero is the interpreter-vs-JIT
mismatch count. Neither modulo mode generates infinities or NaNs, so the
`0 * inf` class of bug needs the adversarial `run_tier_diff.py` case instead.

`--phi` is the only mode whose programs *need* a merge to be correct, so it is
the only one that tests Mem2Reg, Phi placement and copy resolution end to end.
It runs each program twice — plain `--auto` and `--auto --ssa` — and requires
both to match the interpreter, which turns "the pipeline changed the answer"
into a failure rather than a silent pass. That the harness can actually fail was
verified by mis-assigning Phi operands to the wrong predecessor: 25 of 25
programs mismatched immediately.

Mismatches are written to
`fuzz_failures/`, minimised,
and printed.

### Layer 4 — read the generated code

Often the most convincing check, because it needs no timing and no baseline:

```bash
./build/lithon_jit nested_loop.ir --dump-code /tmp/n.bin
objdump -D -b binary -mi386:x86-64 -M intel /tmp/n.bin

# Did strength reduction fire? nested_loop has one multiply per inner iteration.
objdump -D -b binary -mi386:x86-64 -M intel /tmp/n.bin | grep -c imul     # 0 = fired

# Same program with the pass disabled, to prove the above was the pass's doing.
./build/lithon_jit nested_loop.ir --no-lsr --dump-code /tmp/n2.bin
objdump -D -b binary -mi386:x86-64 -M intel /tmp/n2.bin | grep -c imul   # 4
```

### Proving an optimization individually

Every optimization is toggleable, so a claim can be checked rather than taken on
trust. This is how the numbers in [the roadmap above](#-roadmap-where-lithon-stands-today)
were established:

```bash
./build/lithon_jit prog.ir --no-opt          # constant folding + DCE
./build/lithon_jit prog.ir --no-promote      # register promotion
./build/lithon_jit prog.ir --no-rotate       # loop rotation
./build/lithon_jit prog.ir --unroll=1        # all loop unrolling off
./build/lithon_jit prog.ir --no-lsr          # strength reduction
./build/lithon_jit prog.ir --accum-unroll    # opt in to accumulator splitting (~1.6x on a float reduction; reassociates FP)
./build/lithon_jit prog.ir --unroll-diamonds # opt in to diamond unrolling
./build/lithon_jit                          # full flag list
```

`tier_runner` takes `--no-lsr`, `--accum-unroll`, and `--unroll-diamonds` too,
which is what the fuzzer uses; the other four are currently `lithon_jit` only.
`tools/native_bench.py --only float --runner-args='--accum-unroll'` measures the
opt-in pass end to end (the `float reduce 20M` workload is its target).

### Benchmarks

Use the existing harness — it ships the four official workloads plus three
stress cases, reports a **noise** column, and warns that results are unreliable
above ~15% noise.

```bash
python3 tools/native_bench.py --runs 30 --pin 2
```

To compare two states, write a baseline first and compare against it:

```bash
python3 tools/native_bench.py --runs 30 --json /tmp/before.json
# ...change something, rebuild...
python3 tools/native_bench.py --runs 30 --compare /tmp/before.json
```

> **Two traps.** **Always pass `--pin <cpu>`.** Unpinned, one run showed 26%
> noise on `branchy` and the numbers were unusable; pinned, the same comparison
> was stable to ~0.01 ms. And **do not** point `--compare` at
> `benchmarks/results/*.json` — those were written by `tools/bench.py`, which has
> a different schema and different workload names, so the `vs before` column
> comes out **silently empty** rather than erroring. Both sides must come from
> `native_bench.py`.

> **Read `min`, not `median`.** Noise from other processes, frequency scaling
> and cache state only ever *add* time, so the minimum is the least contaminated
> estimate. Differences under ~5% are not meaningful on a shared machine.

### What a green run does not prove

The test suite covers the subset of the language the engine supports today.
Floats, `Div` and `Mod` now work, so they have moved out of the gaps list; what
remains unimplemented is a `Phi` *emitter* and support for more than two
arguments, and those are listed
[above](#known-gaps--the-honest-list) precisely so a green run
is not mistaken for a complete one. If you add support for one of them, the
honest next step is to move it out of that list.

Two more limits worth stating plainly, because a passing run can obscure both:

- **Opcode coverage is not operand coverage.** 25 of 26 opcodes are emitted and
  each is exercised through `tier_runner --strict`, so a pass proves the opcode
  was genuinely executed natively rather than fallen back. It does *not* prove
  every operand shape is right — that is what `encoder_test`'s byte-exact
  assertions and the fuzz modes are for. `gt` and `not`, for instance, have a
  single native use each in the checked-in `.ir` corpus. `Mod` is a standing
  example of why the two are different: the general fuzzer only ever emits
  finite constants, so it cannot generate `1.0 % inf`, which was returning NaN
  natively while the interpreter was correct. That case is pinned by an
  adversarial `run_tier_diff.py` entry that *requires* the native tier, so a
  future guard change cannot make it silently fall back and hide the bug.
- **The interpreter is an oracle, not a specification.** Where Lithon and
  CPython disagree, `run_tier_diff.py` reports it separately as a language gap
  rather than a JIT bug — loop variables are one known case, deliberate. That
  is correct for *this* project, but it does mean the suite cannot catch a bug
  where both engines share the same wrong idea.

---

<!-- MAMBA:BENCHMARK:START -->

## ⚡ Latest Benchmark

| Benchmark | Lithon JIT | Reference | Speedup | Result |
|---|---:|---:|---:|---:|
| fib(30) | 6.9917 ms | 1459.0371 ms | 208.7× | 832040 |

**Status:** PASS

_Last updated by Lithon Reporter Mamba._

<!-- MAMBA:BENCHMARK:END -->

---

## 📜 License

Lithon is released under the [MIT License](LICENSE).
