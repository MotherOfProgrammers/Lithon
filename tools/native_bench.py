#!/usr/bin/env python3
"""Native-only micro-benchmark: fast (no CPython) and low-noise.

  python3 tools/native_bench.py                    # 4 official workloads + stress workloads
  python3 tools/native_bench.py --runs 30 --pin 2  # pin to CPU 2 (uses taskset)
  python3 tools/native_bench.py --json benchmarks/results/native_after.json
  python3 tools/native_bench.py --compare benchmarks/results/native_before.json

Each workload: frontend.py -> IR -> `tier_runner --strict`, wall-clock of the whole
process. The start-up cost (median-of-min of a trivial print(0) program) is
subtracted. The HEADLINE number is the MINIMUM of N runs: noise from other
processes, frequency scaling and cache state only ever ADDS time, so the minimum is
the least contaminated estimate. Median and max are printed so you can see how noisy
the machine is. Differences under ~5% between two runs are not meaningful.

Workloads marked (stress) exercise things the four official benchmarks do not:
register pressure, calls inside loops, and deep tail recursion.
"""
import argparse, json, pathlib, shutil, statistics, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parent.parent

def workloads():
    return {
        "fib(30)": "def fib(n):\n    if n < 2:\n        return n\n    return fib(n - 1) + fib(n - 2)\nprint(fib(30))\n",
        "sum 20M": "total = 0\nfor i in range(20000000):\n    total += i\nprint(total)\n",
        "nested 3000x3000": "total = 0\nfor i in range(3000):\n    for j in range(3000):\n        total += i * j\nprint(total)\n",
        "branchy 10M": "count = 0\nfor i in range(10000000):\n    if i < 5000000:\n        count += 1\n    else:\n        count += 2\nprint(count)\n",
        "(stress) 8 live vars 20M": ("a = 1\nb = 2\nc = 3\nd = 4\ne = 5\nf = 6\ng = 7\n"
            "for i in range(20000000):\n    a = a + b\n    b = b + c\n    c = c + d\n    d = d + e\n"
            "    e = e + f\n    f = f + g\n    g = g + 1\nprint(a + b + c + d + e + f + g)\n"),
        "(stress) call in loop 20M": "def inc(x):\n    return x + 1\nt = 0\nfor i in range(20000000):\n    t = inc(t)\nprint(t)\n",
        "(stress) tail rec 10M deep": "def acc(n, s):\n    if n < 1:\n        return s\n    return acc(n - 1, s + n)\nprint(acc(10000000, 0))\n",
    }

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=15)
    ap.add_argument("--pin", type=int, default=None, help="pin to this CPU core with taskset")
    ap.add_argument("--root", type=pathlib.Path, default=ROOT)
    ap.add_argument("--runner", type=pathlib.Path, default=None)
    ap.add_argument("--json", type=pathlib.Path, default=None)
    ap.add_argument("--compare", type=pathlib.Path, default=None, help="earlier --json file to compare against")
    ap.add_argument("--only", default=None, help="substring filter on workload names")
    a = ap.parse_args()

    runner = a.runner or a.root / "build" / "tier_runner"
    frontend = a.root / "src" / "frontend" / "frontend.py"
    if not runner.exists():
        print(f"error: {runner} not found -- build it first"); return 2
    prefix = []
    if a.pin is not None:
        if shutil.which("taskset"): prefix = ["taskset", "-c", str(a.pin)]
        else: print("warning: taskset not found, not pinning")

    def compile_ir(tmp, name, src):
        py = tmp / f"{name}.py"; py.write_text(src)
        r = subprocess.run([sys.executable, str(frontend), str(py)], capture_output=True, text=True)
        if r.returncode: raise RuntimeError(f"frontend failed: {r.stderr.strip()[:200]}")
        ir = tmp / f"{name}.ir"; ir.write_text(r.stdout); return ir

    def once(ir):
        t = time.perf_counter()
        r = subprocess.run(prefix + [str(runner), str(ir), "--strict"], capture_output=True, text=True)
        ms = (time.perf_counter() - t) * 1000
        if r.returncode: raise RuntimeError(f"tier_runner exited {r.returncode}: {r.stderr.strip()[:200]}")
        return ms, r.stdout.strip()

    def sample(ir):
        once(ir)   # warm-up
        ts, out = [], ""
        for _ in range(a.runs):
            ms, out = once(ir); ts.append(ms)
        return ts, out

    prev = {}
    if a.compare:
        prev = {r["program"]: r for r in json.loads(a.compare.read_text())["rows"]}

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        base_ts, _ = sample(compile_ir(tmp, "base", "print(0)\n"))
        base = min(base_ts)
        print(f"{a.runs} runs each, min is the headline; start-up baseline (min) {base:.2f} ms"
              + (f"; pinned to CPU {a.pin}" if prefix else "; NOT pinned (try --pin 2)") + "\n")
        hdr = f"{'program':<28}{'min ms':>9}{'median':>9}{'max':>9}{'noise':>8}"
        print(hdr + (f"{'vs before':>12}" if prev else ""))
        for i, (name, src) in enumerate(workloads().items()):
            if a.only and a.only not in name: continue
            try:
                ts, out = sample(compile_ir(tmp, f"w{i}", src))
            except RuntimeError as e:
                print(f"{name:<28}  ERROR: {e}"); rows.append({"program": name, "error": str(e)}); continue
            mn, md, mx = min(ts) - base, statistics.median(ts) - base, max(ts) - base
            mn = max(mn, 0.001)
            noise = (statistics.median(ts) - min(ts)) / max(min(ts), 1e-9) * 100
            line = f"{name:<28}{mn:>9.2f}{md:>9.2f}{mx:>9.2f}{noise:>7.0f}%"
            if name in prev and "min_ms" in prev[name]:
                d = (mn - prev[name]["min_ms"]) / prev[name]["min_ms"] * 100
                line += f"{d:>+11.0f}%"
            print(line)
            rows.append({"program": name, "min_ms": mn, "median_ms": md, "max_ms": mx, "output": out})
    print("\n'noise' = (median - min) / min: if it is above ~15%, the machine is busy; results are unreliable.")
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps({"runs": a.runs, "baseline_ms": base, "rows": rows}, indent=2))
        print(f"wrote {a.json}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
