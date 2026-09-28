// Unit tests for print_guard.h.
//
// Build:
//   g++ -std=c++20 -Wall -Wextra -Isrc -Isrc/jit -o print_guard_test
//       src/jit/print_guard_test.cpp src/ir/text_parser.cpp

#include <cstdio>
#include <string>
#include "ir/text_parser.h"
#include "print_guard.h"

using lithon::jit::check_print_safety;

static int g_failed = 0;
static int g_total = 0;

static void expect(const char* name, const std::string& ir_text, bool want_safe) {
    ++g_total;
    auto module = lithon::ir::parse_ir_text(ir_text);
    auto verdict = check_print_safety(module);
    bool ok = verdict.native_safe == want_safe;
    std::printf("[%s] %s (want %s, got %s)\n", ok ? "PASS" : "FAIL", name,
                want_safe ? "safe" : "refuse", verdict.native_safe ? "safe" : "refuse");
    if (!ok) ++g_failed;
    for (const auto& r : verdict.reasons) std::printf("        reason: %s\n", r.c_str());
}

int main() {
    // ---- must be REFUSED: a printed value may be bool / float / unknown ----

    expect("print(1 < 2) is a bool", R"(
function main():
block0:
    %0 = const_i64 1
    %1 = const_i64 2
    %2 = lt %0, %1
    call print, %2
    return
)", false);

    expect("print(not 5) is a bool", R"(
function main():
block0:
    %0 = const_i64 5
    %1 = not %0
    call print, %1
    return
)", false);

    expect("bool literal through and/or", R"(
function main():
block0:
    %0 = const_bool 1
    %1 = const_bool 0
    %2 = and %0, %1
    call print, %2
    return
)", false);

    expect("int and bool mixed in and/or", R"(
function main():
block0:
    %0 = const_i64 5
    %1 = const_bool 0
    %2 = or %0, %1
    call print, %2
    return
)", false);

    expect("bool stored then loaded", R"(
function main():
block0:
    %0 = const_i64 3
    %1 = const_i64 4
    %2 = lt %0, %1
    store flag, %2
    %3 = load flag
    call print, %3
    return
)", false);

    expect("variable is int on one path, bool on another", R"(
function main():
block0:
    %0 = const_i64 1
    store x, %0
    %1 = const_i64 1
    %2 = const_i64 2
    %3 = lt %1, %2
    store x, %3
    %4 = load x
    call print, %4
    return
)", false);

    expect("bool returned by a function and printed", R"(
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
)", false);

    expect("bool argument printed inside callee", R"(
function show(v):
block0:
    %0 = load v
    call print, %0
    return

function main():
block0:
    %0 = const_i64 1
    %1 = const_i64 2
    %2 = lt %0, %1
    call show, %2
    return
)", false);

    expect("float printed", R"(
function main():
block0:
    %0 = const_f64 3.5
    call print, %0
    return
)", false);

    expect("int / int is a float in the interpreter", R"(
function main():
block0:
    %0 = const_i64 6
    %1 = const_i64 3
    %2 = div %0, %1
    call print, %2
    return
)", false);

    expect("bool arithmetic is not provably int", R"(
function main():
block0:
    %0 = const_bool 1
    %1 = const_i64 2
    %2 = add %0, %1
    call print, %2
    return
)", false);

    expect("uncalled function with untyped param may get anything", R"(
function helper(x):
block0:
    %0 = load x
    call print, %0
    return

function main():
block0:
    return
)", false);

    expect("print with no arguments", R"(
function main():
block0:
    call print
    return
)", false);

    // ---- must be SAFE: every printed value is provably int ----

    expect("int arithmetic", R"(
function main():
block0:
    %0 = const_i64 2
    %1 = const_i64 3
    %2 = add %0, %1
    %3 = mul %2, %1
    call print, %3
    return
)", true);

    expect("and/or over ints returns an int operand", R"(
function main():
block0:
    %0 = const_i64 5
    %1 = const_i64 0
    %2 = and %0, %1
    call print, %2
    %3 = or %0, %1
    call print, %3
    return
)", true);

    expect("untyped recursion: param kind inferred from call sites", R"(
function fib(n):
block0:
    %0 = load n
    %1 = const_i64 2
    %2 = lt %0, %1
    branch %2, block1, block2
block1:
    %3 = load n
    return %3
    jump block2
block2:
    %4 = load n
    %5 = const_i64 1
    %6 = sub %4, %5
    %7 = call fib, %6
    %8 = load n
    %9 = const_i64 2
    %10 = sub %8, %9
    %11 = call fib, %10
    %12 = add %7, %11
    return %12
    return

function main():
block0:
    %0 = const_i64 10
    %1 = call fib, %0
    call print, %1
    return
)", true);

    expect("bool used only as a branch condition, never printed", R"(
function main():
block0:
    %0 = const_i64 1
    %1 = const_i64 2
    %2 = lt %0, %1
    branch %2, block1, block2
block1:
    %3 = const_i64 7
    call print, %3
    return
block2:
    return
)", true);

    expect("int stored and reloaded through a variable", R"(
function main():
block0:
    %0 = const_i64 9
    store x, %0
    %1 = load x
    call print, %1
    return
)", true);

    expect("dead trailing return does not poison the return kind", R"(
function one():
block0:
    %0 = const_i64 1
    return %0
    return

function main():
block0:
    %0 = call one
    call print, %0
    return
)", true);

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
