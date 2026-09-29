#!/usr/bin/env python3
"""Native Lithon vs CPython, on the same source programs.

  python3 tools/bench.py                 # 10 timed runs per program
  python3 tools/bench.py --runs 20 --json benchmarks/results/vs_cpython.json
  python3 tools/bench.py --quick         # smaller sizes, for a smoke test

How it measures (so the numbers can be defended):
  * Each program is compiled once: frontend.py -> IR -> `tier_runner --strict`.
    --strict means native only: if the print guard or the JIT refuses, the run
    FAILS instead of silently falling back to the interpreter.
  * Native time = wall-clock of the whole tier_runner process (start, parse,
    typecheck, JIT compile, run) MINUS the median of a trivial `print(0)`
    program, so process start-up is not billed to the workload. The raw and
    baseline numbers are printed too.
  * CPython time = exec() of the same source in-process with stdout captured,
    so interpreter start-up is not billed either.
  * Median of N runs after one discarded warm-up run; min and max shown.
  * Both sides must print identical output, or the row is marked MISMATCH.
  * Warns if the build was not made with CMAKE_BUILD_TYPE=Release.
"""
import argparse, contextlib, io, json, pathlib, platform, statistics, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Sources that go into build/tier_runner. If any is newer than the binary, the binary does not
# contain your latest changes and every result below would be about the OLD code.
RUNNER_INPUTS = ["src/jit/tier_runner.cpp", "src/jit/*.h", "src/ir/*", "src/interpreter/*",
                 "src/typecheck/*", "src/runtime/*"]


def stale_runner_inputs(root, runner):
    t = runner.stat().st_mtime
    newer = []
    for pattern in RUNNER_INPUTS:
        for p in root.glob(pattern):
            if p.is_file() and p.suffix in (".cpp", ".h") and p.stat().st_mtime > t:
                newer.append(p)
    return sorted(set(newer))


def workloads(quick):
    s = 20 if quick else 1
    fib_n = 25 if quick else 30
    loop_n = 20_000_000 // s
    nest_n = 3000 if not quick else 700
    branch_n = 10_000_000 // s
    return {
        f"fib({fib_n}) recursion": f"""
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
print(fib({fib_n}))
""",
        f"sum loop {loop_n:,}": f"""
total = 0
for i in range({loop_n}):
    total += i
print(total)
""",
        f"nested loops {nest_n}x{nest_n}": f"""
total = 0
for i in range({nest_n}):
    for j in range({nest_n}):
        total += i * j
print(total)
""",
        f"branchy loop {branch_n:,}": f"""
count = 0
for i in range({branch_n}):
    if i < {branch_n // 2}:
        count += 1
    else:
        count += 2
print(count)
""",
    }


def cpython_once(code):
    buf = io.StringIO()
    t = time.perf_counter()
    with contextlib.redirect_stdout(buf):
        exec(code, {"__name__": "__bench__"})
    return (time.perf_counter() - t) * 1000.0, buf.getvalue()


def native_once(runner, ir_path):
    t = time.perf_counter()
    r = subprocess.run([str(runner), str(ir_path), "--strict"], capture_output=True, text=True)
    ms = (time.perf_counter() - t) * 1000.0
    if r.returncode != 0:
        raise RuntimeError(f"tier_runner exited {r.returncode}: {(r.stderr or '').strip()[:300]}")
    return ms, r.stdout


def sample(fn, runs):
    fn()                                   # warm-up, discarded
    times, out = [], None
    for _ in range(runs):
        ms, out = fn()
        times.append(ms)
    return times, out


def build_type(root):
    cache = root / "build" / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("CMAKE_BUILD_TYPE"):
            return line.split("=", 1)[1].strip()
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--quick", action="store_true", help="smaller problem sizes")
    ap.add_argument("--root", type=pathlib.Path, default=ROOT)
    ap.add_argument("--runner", type=pathlib.Path, default=None, help="default: <root>/build/tier_runner")
    ap.add_argument("--json", type=pathlib.Path, default=None)
    args = ap.parse_args()

    root = args.root
    runner = args.runner or root / "build" / "tier_runner"
    frontend = root / "src" / "frontend" / "frontend.py"
    if not runner.exists():
        print(f"error: {runner} not found -- build first (cmake --build build)"); return 2

    stale = stale_runner_inputs(root, runner)
    if stale:
        print(f"error: {runner} is older than {len(stale)} of its source files (e.g. "
              f"{stale[0].relative_to(root)}); it does not contain your latest changes. Rebuild it first.")
        return 2

    bt = build_type(root)
    if bt != "Release":
        what = ("no build/CMakeCache.txt found" if not (root / "build" / "CMakeCache.txt").exists()
                else f"CMAKE_BUILD_TYPE is {bt!r}" if bt else "CMAKE_BUILD_TYPE is empty")
        print(f"WARNING: {what}, not 'Release'. JIT-generated code is unaffected, but the C++ compiler, parser "
              f"and interpreter are unoptimized. Reconfigure with -DCMAKE_BUILD_TYPE=Release before quoting "
              f"numbers.\n")

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)

        def compile_ir(name, src):
            py = tmp / f"{name}.py"; py.write_text(src)
            r = subprocess.run([sys.executable, str(frontend), str(py)], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(f"frontend failed: {r.stderr.strip()[:300]}")
            ir = tmp / f"{name}.ir"; ir.write_text(r.stdout)
            return ir

        try:
            base_ir = compile_ir("baseline", "print(0)\n")
            base_times, _ = sample(lambda: native_once(runner, base_ir), args.runs)
        except RuntimeError as e:
            print(f"error: cannot run even a trivial program with {runner}\n  {e}"); return 2
        baseline = statistics.median(base_times)

        for i, (label, src) in enumerate(workloads(args.quick).items()):
            row = {"program": label}
            try:
                ir = compile_ir(f"w{i}", src)
                n_times, n_out = sample(lambda: native_once(runner, ir), args.runs)
                code = compile(src, label, "exec")
                c_times, c_out = sample(lambda: cpython_once(code), args.runs)
            except RuntimeError as e:
                row["error"] = str(e); rows.append(row); continue
            n_raw = statistics.median(n_times)
            n_net = max(n_raw - baseline, 0.001)
            c_med = statistics.median(c_times)
            row.update(
                cpython_ms=c_med, cpython_min=min(c_times), cpython_max=max(c_times),
                native_raw_ms=n_raw, native_net_ms=n_net, native_min=min(n_times), native_max=max(n_times),
                speedup=c_med / n_net, output_match=(n_out.strip() == c_out.strip()))
            rows.append(row)

    print(f"CPython {platform.python_version()}  |  {platform.platform()}  |  build type: {bt or 'unknown'}")
    print(f"{args.runs} timed runs each (median), 1 warm-up; native start-up baseline: {baseline:.2f} ms\n")
    print(f"{'program':<26}{'CPython ms':>12}{'native ms':>12}{'(raw)':>10}{'speedup':>10}   check")
    for r in rows:
        if "error" in r:
            print(f"{r['program']:<26}  ERROR: {r['error']}"); continue
        speed = f"{r['speedup']:>9.1f}x" if r["output_match"] else f"{'n/a':>10}"
        print(f"{r['program']:<26}{r['cpython_ms']:>12.1f}{r['native_net_ms']:>12.2f}{r['native_raw_ms']:>10.2f}"
              f"{speed}   {'ok' if r['output_match'] else 'OUTPUT MISMATCH'}")
    print("\nnative ms = median wall-clock minus start-up baseline; (raw) is the unadjusted median.")
    print("A native time near 1 ms is at the timer/process noise floor: use a bigger problem size before "
          "trusting the ratio.")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"python": platform.python_version(), "platform": platform.platform(),
                                         "build_type": bt, "runs": args.runs, "baseline_ms": baseline,
                                         "rows": rows}, indent=2))
        print(f"wrote {args.json}")
    bad = [r for r in rows if "error" in r or not r.get("output_match", False)]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
