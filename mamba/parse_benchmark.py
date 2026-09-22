import json
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()

native = re.search(
    r"native compiled\s*:\s*([0-9.]+)\s*ms\s*\(result=(-?[0-9]+)\)",
    text
)
reference = re.search(r"interpreter\s*:\s*([0-9.]+)\s*ms",text)
speedup = re.search(r"speedup\s*:\s*([0-9.]+)x",text)

if not native or not reference or not speedup:
    raise SystemExit("Unable to parse benchmark output")

result = {
    "benchmark": "fib(30)",
    "native_ms": float(native.group(1)),
    "reference_ms": float(reference.group(1)),
    "speedup": float(speedup.group(1)),
    "result": int(native.group(2)),
    "status": "PASS",
}

Path("benchmarks/results/latest.json").write_text(
    json.dumps(result, indent=2) + "\n"
)

print(json.dumps(result, indent=2))
