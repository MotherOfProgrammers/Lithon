import json
import re
from pathlib import Path

README = Path("README.md")
RESULT = Path("benchmarks/results/latest.json")

START = "<!-- MAMBA:BENCHMARK:START -->"
END = "<!-- MAMBA:BENCHMARK:END -->"

data = json.loads(RESULT.read_text())

section = f"""\
{START}

## ⚡ Latest Benchmark

| Benchmark | Lithon JIT | Reference | Speedup | Result |
|---|---:|---:|---:|---:|
| {data["benchmark"]} | {data["native_ms"]:.4f} ms | {data["reference_ms"]:.4f} ms | {data["speedup"]:.1f}× | {data["result"]} |

**Status:** {data["status"]}

_Last updated by Lithon Reporter Mamba._

{END}
"""

readme = README.read_text()

pattern = re.escape(START) + r".*?" + re.escape(END)

if re.search(pattern, readme, re.DOTALL):
    readme = re.sub(pattern, section.rstrip(), readme, count=1, flags=re.DOTALL)
else:
    readme = readme.rstrip() + "\n\n" + section

README.write_text(readme)
