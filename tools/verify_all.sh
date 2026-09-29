#!/usr/bin/env bash
# Build (Release) and run every check. Stops at the first failure.
#   tools/verify_all.sh            # everything except the benchmark
#   tools/verify_all.sh --bench    # ...then the CPython benchmark
set -euo pipefail
cd "$(dirname "$0")/.."

step() { printf '\n==== %s\n' "$*"; }

step "configure + build (Release)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
grep '^CMAKE_BUILD_TYPE' build/CMakeCache.txt

step "tier_runner (used by run_tier_diff.py, bench.py and the lithon package)"
if cmake --build build --target help 2>/dev/null | grep -qw tier_runner; then
  cmake --build build -j"$(nproc)" --target tier_runner
else
  echo "tier_runner is not a CMake target in this tree, so CMake never rebuilds it; building it directly"
  g++ -std=c++20 -O2 -Isrc -Isrc/jit -o build/tier_runner \
      src/jit/tier_runner.cpp src/ir/text_parser.cpp src/interpreter/interpreter.cpp src/typecheck/typecheck.cpp
fi
if [[ -n "$(find src/jit/tier_runner.cpp src/jit/*.h src/ir src/interpreter src/typecheck src/runtime \
      \( -name '*.cpp' -o -name '*.h' \) -newer build/tier_runner -print -quit)" ]]; then
  echo "ERROR: build/tier_runner is older than its sources"; exit 1
fi

step "ctest (all JIT/allocator/liveness/encoder targets)"
ctest --test-dir build --output-on-failure

step "standalone tests not yet in CMakeLists.txt"
mkdir -p build
g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Isrc/jit          -o build/print_guard_entry_test src/jit/print_guard_entry_test.cpp
g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Isrc/typecheck    -o build/typecheck_entry_test   src/typecheck/typecheck_entry_test.cpp src/typecheck/typecheck.cpp
g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Isrc/interpreter  -o build/interpreter_entry_test src/interpreter/interpreter_entry_test.cpp src/interpreter/interpreter.cpp
g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Isrc/jit -Isrc/ir -o build/print_guard_test      src/jit/print_guard_test.cpp src/ir/text_parser.cpp
for t in print_guard_entry_test typecheck_entry_test interpreter_entry_test print_guard_test; do
  echo "-- $t"; ./build/$t | tail -3
done

step "encoder vs GNU as"
python3 tools/check_encoder_vs_as.py

step "frontend"
python3 tools/test_frontend_entry.py

step "regression suites (interpreter oracle vs CPython) + typed suite"
python3 tools/run_regression.py
python3 tools/run_typed_regression.py

step "tier diff: native output must equal interpreter output"
python3 tools/run_tier_diff.py

if [[ "${1:-}" == "--bench" ]]; then
  step "benchmark vs CPython"
  python3 tools/bench.py --runs 10 --json benchmarks/results/vs_cpython.json
fi
printf '\nALL CHECKS PASSED\n'
