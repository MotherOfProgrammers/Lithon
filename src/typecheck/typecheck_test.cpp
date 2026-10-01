// Pins the typechecker's rules for putting a value into a typed location,
// which is where the two typecheckers (this one and tools/typecheck.py) are
// easiest to let drift apart.
//
// The case that motivated all of this: `j: float[64] = 0` used to be accepted
// here, because the literal branch only range-checked int targets and then
// returned. The frontend stored an int into a float[64] variable, the type
// checker said "OK", and the program only failed much later in codegen, with
// the print guard complaining that "a value stored both an int and a float
// reaches here" -- a message that pointed at the loop and never mentioned the
// literal the user actually wrote. An int literal is not a float, and a
// float-typed variable holds a float, so this is now a type error naming the
// literal and suggesting the fix.
//
// The two rules that must NOT be confused with it, both covered below:
// expression-level promotion (`7 + 0.5` is 7.5) and int -> float for values
// whose provenance is not a literal (`b: float[64] = a` where a is an int).

#include "typecheck.h"
#include "ir/text_parser.h"
#include <cstdio>
#include <string>
#include <vector>

using lithon::ir::parse_ir_text;
using lithon::typecheck::check_module;

namespace {

int failures = 0;

std::string errors_for(const std::string& ir) {
    auto module = parse_ir_text(ir);
    std::string joined;
    for (const auto& e : check_module(module)) {
        if (!joined.empty()) joined += " | ";
        joined += e.message;
    }
    return joined;
}

void expect_accepts(const char* what, const std::string& ir) {
    std::string errs = errors_for(ir);
    if (errs.empty()) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s: expected accepted, got: %s\n", what, errs.c_str());
        ++failures;
    }
}

void expect_rejects(const char* what, const std::string& ir, const char* needle) {
    std::string errs = errors_for(ir);
    if (errs.find(needle) == std::string::npos) {
        std::printf("  FAIL %s: expected a rejection mentioning \"%s\", got: %s\n",
                    what, needle, errs.empty() ? "(accepted)" : errs.c_str());
        ++failures;
    } else {
        std::printf("  ok   %s\n", what);
    }
}

const char* kFloatDeclIntLiteral = R"(
function __main__():
block0:
    %0 = const_i64 0
    store j, %0 : float[64]
    %1 = load j
    call print, %1
    return
)";

} // namespace

int main() {
    std::printf("typecheck: literal kind vs declared type\n");

    // The regression this file exists for.
    expect_rejects("int literal into float[64] declaration", kFloatDeclIntLiteral,
                   "is an int, but float[64] must hold a float -- write 0.0");

    // Same rule on re-assignment, where the value is no longer the initialiser.
    expect_rejects("int literal re-assigned to a float variable", R"(
function __main__():
block0:
    %0 = const_f64 0.0
    store j, %0 : float[64]
    %1 = const_i64 5
    store j, %1
    return
)", "is an int, but float[64] must hold a float -- write 5.0");

    // The correct spelling, and the reason the message can suggest it.
    expect_accepts("float literal into float[64]", R"(
function __main__():
block0:
    %0 = const_f64 0.0
    store j, %0 : float[64]
    return
)");

    // Promotion still applies to a non-literal int. This is tests/
    // typed_programs/int_to_float_ok.py, which must keep passing.
    expect_accepts("int VARIABLE into float[64] still promotes", R"(
function __main__():
block0:
    %0 = const_i64 5
    store a, %0 : int[64]
    %1 = load a
    store b, %1 : float[64]
    return
)");

    // Expression-level promotion is a different rule and never comes through
    // the assignment path: 7 + 0.5 is 7.5, not a rejection.
    expect_accepts("expression promotion 7 + 0.5", R"(
function __main__():
block0:
    %0 = const_i64 7
    %1 = const_f64 0.5
    %2 = add %0, %1
    call print, %2
    return
)");

    // int % int stays int, so an int literal is correct there.
    expect_accepts("int literal into int[64]", R"(
function __main__():
block0:
    %0 = const_i64 0
    store i, %0 : int[64]
    return
)");

    // The pre-existing literal-range rule must be untouched by any of this.
    expect_rejects("int literal that does not fit int[8]", R"(
function __main__():
block0:
    %0 = const_i64 199
    store x, %0 : int[8]
    return
)", "does not fit int[8]");

    // And float -> int remains a hard error with its own message.
    expect_rejects("float into int[8]", R"(
function __main__():
block0:
    %0 = const_f64 3.5
    store x, %0 : int[8]
    return
)", "float -> int conversion does not exist");

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
