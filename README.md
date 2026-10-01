<div align="center">

<img src="assets/boa.png" alt="Lithon Logo" width="380" />

# Lithon

### Give Python wings. Bare-metal speed with zero external dependencies.

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen?style=flat-square&logo=github-actions)](https://github.com/your-username/lithon)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Architecture](https://img.shields.io/badge/arch-x86--64-red?style=flat-square)](https://en.wikipedia.org/wiki/X86-64)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-success?style=flat-square)](#)
[![Typing](https://img.shields.io/badge/typing-mandatory%20static-green?style=flat-square)](#)
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

## ⚡ Quickstart (Under 10 Seconds)

Lithon requires **zero external dependencies**. No GCC, no LLVM, no Python installation needed to run compiled scripts.

```bash
# 1. Clone the repository
git clone [https://github.com/your-username/lithon.git](https://github.com/your-username/lithon.git)
cd lithon

# 2. Build the zero-dependency C++ engine
make -j$(nproc)

# 3. Run a benchmark script with native x64 JIT compilation
./lithon run examples/fib.py --dump-hex
```

### Terminal Output Preview
```text
[+] Lithon v1.0.0-beta [x86-64 Native JIT]
[+] Static Flow Verifier: Passed (0 errors).
[+] Emitted 206 bytes x64 machine code @ 0x7f9a12b00000 (PROT_READ|PROT_EXEC)
[+] Payload: 55 48 89 e5 48 81 ec 20 00 00 00 48 89 7d f8 ...
[+] Result: fib(30) = 832040
[+] Execution Time: 6.90 ms (210.7x faster than CPython 3.12)
```

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
| SysV x64 ABI | Shipped | `src/jit/jit_abi.h`; `check_stack_alignment.py` proves callee-saved + 16-byte stack alignment at every call/ret |
| Windows x64 ABI | Implemented, **not yet verified** | `#if defined(_WIN32)` in `jit_abi.h`; the audit only exercises the host ABI, so the Win64 path has no test evidence yet |
| Tier-0 interpreter fallback | Shipped | `tier_runner --auto` falls back on unprovable output (`print_guard.h`) |
| Static type flow verifier | Shipped | `tools/typecheck.py`, `run_typed_regression.py` (12/12) |
| Liveness + register allocation | Shipped | `liveness_test`, `regalloc_test` |
| IR text format | Shipped | `src/ir/text_parser.cpp` — no Python dependency in the engine |
| Function calls, recursion, TCO | Shipped | `compile_module_call_test`, `fib_test`; self-tail-calls become loops, O(1) stack |

### Optimization pipeline (this is the active work)

Every optimization is independently switchable, so its effect can be measured
rather than assumed.

| Change | Lives in | Isolated measurement vs. HEAD |
| :--- | :--- | :--- |
| `strength_reduce_multiplies` — invariant × induction-variable → repeated add | `optimize.h` | **nested 1.062×** |
| Callee-saved borrowing — temps live across a call borrow a callee-saved register instead of the stack | `register_alloc.h` | **fib 1.063×** |
| Shared virtual-temp liveness — one `VirtualTemps` set, excluded *before* live ranges are computed | `liveness.h` | correctness, not speed |
| `fold_constants`, `eliminate_dead_code`, `convert_self_tail_calls` | `optimize.h` | bundled, not isolated |

Measured on an Intel i3-3110M (Sandy Bridge), 12 interleaved rounds, CPU-time
clock, one pinned core. See [Testing](#-testing) to reproduce them yourself.

### Known gaps — the honest list

- **`Phi` has no emitter.** The IR can express it; the encoder cannot yet. It
  is the only missing opcode, and is needed for `if`-as-expression lowering
  once both arms must merge without a stack round-trip.
- Net: **19 of the 20 IR opcodes are emitted.** The one missing one is `Phi`.

### Floating point, and what "identical" had to mean

`float` is implemented end-to-end: `ConstFloat`, load/store, `Add`/`Sub`/`Mul`/
`Div`, the three comparisons, and native `print()`. Both tiers now run
`float.ir`, `mixed_numeric.ir` and `comparison.ir` natively, with
`run_tier_diff.py` reporting 33/33 native and zero interpreter fallbacks.

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

### Layer 1 — unit tests (~1 s)

```bash
ctest --test-dir build --output-on-failure        # 15/15
```

The 15 tests are not all the same kind, and it is worth knowing which is which:

- **`liveness_test`, `regalloc_test`** — pure analysis tests. They build IR and
  call the pass directly, and never emit a byte. A green run here means the
  data structures are right, not that any machine code ran.
- **`compile_function_*`, `compile_module_*`, `optimize_lsr_test`** — compile a
  module to machine code, mmap it, and call it through a function pointer,
  checking the returned values. These are the tests that would catch a bad
  encoding.
- **`encoder_test`, `stack_test`, `branch_test`, `print_guard_*`** — the
  x86/ABI layer underneath, tested in isolation.

### Layer 2 — the full gate (~2 min)

```bash
bash tools/verify_all.sh                         # ABI, stack alignment, callee-saved audit
python3 tools/run_regression.py                  # 12/12 untyped programs
python3 tools/run_typed_regression.py            # 12/12 typed programs
python3 tools/run_tier_diff.py                   # 33/33
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
four general modes had never approached. Mismatches are written to
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
./build/lithon_jit prog.ir --unroll-diamonds # opt in to diamond unrolling
./build/lithon_jit                          # full flag list
```

`tier_runner` takes `--no-lsr` and `--unroll-diamonds` too, which is what the
fuzzer uses; the other four are currently `lithon_jit` only.

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

The test suite covers the subset of the language the engine supports today. It
does not mean floats, `Div`, `Phi`, or more than two arguments work — those are
listed as gaps [above](#known-gaps--the-honest-list) precisely so a green run is
not mistaken for a complete one. If you add support for one of them, the
honest next step is to move it out of that list.

---

<!-- MAMBA:BENCHMARK:START -->

## ⚡ Latest Benchmark

| Benchmark | Lithon JIT | Reference | Speedup | Result |
|---|---:|---:|---:|---:|
| fib(30) | 6.9902 ms | 1487.4944 ms | 212.8× | 832040 |

**Status:** PASS

_Last updated by Lithon Reporter Mamba._

<!-- MAMBA:BENCHMARK:END -->

---

## 📜 License

Lithon is released under the [MIT License](LICENSE).
