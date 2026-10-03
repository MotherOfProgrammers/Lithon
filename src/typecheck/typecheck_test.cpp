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

    // --- bitwise and shifts -------------------------------------------------
    // These pin the rules that the two checkers must agree on, in both
    // directions: the rejections below have to be rejections in
    // tools/typecheck.py too, and the acceptances have to be acceptances.

    // A left shift multiplies, so a value that fits its own declared type can
    // still leave the type it is being stored into. 100 << 3 is 800, which is
    // not an int[8].
    expect_rejects("shl overflowing the target int[8]", R"(
function __main__():
block0:
    %0 = const_i64 100
    store a, %0 : int[8]
    %1 = load a
    %2 = const_i64 3
    %3 = shl %1, %2
    store b, %3 : int[8]
    return
)", "is not wide enough");

    // The same shift into a target that can hold it is fine. This is the
    // negative case that keeps the rule from being "reject every shl".
    expect_accepts("shl that fits the target int[16]", R"(
function __main__():
block0:
    %0 = const_i64 100
    store a, %0 : int[8]
    %1 = load a
    %2 = const_i64 3
    %3 = shl %1, %2
    store b, %3 : int[16]
    return
)");

    // A count outside 0..63 is a compile-time error when it is a literal,
    // because x86 masks the count to 6 bits and would otherwise silently
    // shift by a different amount (64 would execute as 0, -1 as 63).
    expect_rejects("literal shift count 64", R"(
function __main__():
block0:
    %0 = const_i64 1
    %1 = const_i64 64
    %2 = shl %0, %1
    return
)", "out of range 0..63");

    expect_rejects("negative literal shift count", R"(
function __main__():
block0:
    %0 = const_i64 1
    %1 = const_i64 -1
    %2 = shl %0, %1
    return
)", "out of range 0..63");

    // 63 is the largest legal count and must be accepted, not lumped in with
    // the rejections above.
    expect_accepts("literal shift count 63", R"(
function __main__():
block0:
    %0 = const_i64 1
    %1 = const_i64 63
    %2 = shl %0, %1
    return
)");

    // Bitwise ops are integer-only. There is no float bit pattern to
    // reinterpret, so this is a type error rather than a punning of the
    // double's bytes.
    expect_rejects("float operand to a bitwise op", R"(
function __main__():
block0:
    %0 = const_f64 2.5
    %1 = const_i64 1
    %2 = band %0, %1
    return
)", "integer-only");

    expect_accepts("integer bitwise and", R"(
function __main__():
block0:
    %0 = const_i64 5
    %1 = const_i64 3
    %2 = band %0, %1
    return
)");

    // A binop's result is the LEFT operand's width, so bitand of int[8] and
    // int[64] is an int[8] and may be stored in an int[8] without narrowing.
    expect_accepts("bitand result takes the left width", R"(
function __main__():
block0:
    %0 = const_i64 5
    store a, %0 : int[8]
    %1 = const_i64 3
    %2 = load a
    %3 = band %2, %1
    store b, %3 : int[8]
    return
)");

    // check_binop_fits_target answers whether the VALUES fit the target, which
    // is a different question from whether the result's WIDTH fits. An int[64]
    // result holding a small number is still an illegal narrowing, and the
    // early return that used to skip this check let `return 100 << 3` through
    // as int[16] while tools/typecheck.py rejected it.
    expect_rejects("shl result narrowed into a smaller int", R"(
function f() -> int[16]:
block0:
    %0 = const_i64 100
    %1 = const_i64 3
    %2 = shl %0, %1
    return %2
    return
)", "cannot narrow");

    // A conditional expression lowers to a temporary named __ifexprN that
    // both arms store and the join loads. The name is minted by the frontend,
    // so there is no source annotation for V1_SPEC 0.6.1 to demand -- the type
    // has to be inferred from the arms, or every typed program using a ternary
    // is rejected for a declaration it cannot have.
    expect_accepts("merge temp inferred from int arms", R"(
function __main__():
block0:
    %0 = const_i64 1
    %1 = const_i64 0
    branch %0, block1, block2
block1:
    %2 = const_i64 7
    store __ifexpr0, %2
    jump block3
block2:
    %3 = const_i64 9
    store __ifexpr0, %3
    jump block3
block3:
    %4 = load __ifexpr0
    call print, %4
    return
)");

    // The inference must produce the ARM's type, not merely "some type": a
    // merge temp that came back as an int would put an int where the join
    // expects the float, and print the wrong thing.
    expect_accepts("merge temp inferred as float from float arms", R"(
function __main__():
block0:
    %0 = const_i64 1
    branch %0, block1, block2
block1:
    %1 = const_f64 1.5
    store __ifexpr0, %1
    jump block3
block2:
    %2 = const_f64 2.5
    store __ifexpr0, %2
    jump block3
block3:
    %3 = load __ifexpr0
    call print, %3
    return
)");

    // ...and it is the float type in fact, not a pass: the merged value has to
    // satisfy a float[64] declaration.
    expect_accepts("merged float satisfies a float[64] declaration", R"(
function __main__():
block0:
    %0 = const_i64 1
    branch %0, block1, block2
block1:
    %1 = const_f64 1.5
    store __ifexpr0, %1
    jump block3
block2:
    %2 = const_f64 2.5
    store __ifexpr0, %2
    jump block3
block3:
    %3 = load __ifexpr0
    store d, %3 : float[64]
    return
)");

    // Accepting the temporary must not mean accepting anything. The two arms
    // disagree about the type, which no source-level declaration could have
    // expressed either, so it stays an error: Lithon is statically typed and
    // `1.5 if c else 0` is not a float-or-int, it is two different types.
    expect_rejects("merge temp whose arms disagree on type", R"(
function __main__():
block0:
    %0 = const_i64 1
    branch %0, block1, block2
block1:
    %1 = const_f64 1.5
    store __ifexpr0, %1
    jump block3
block2:
    %2 = const_i64 0
    store __ifexpr0, %2
    jump block3
block3:
    %3 = load __ifexpr0
    call print, %3
    return
)", "disagrees across branches");

    // Same, for widths: int[8] and int[64] arms are both ints but not the same
    // int, so the merge still has to be pinned down rather than silently
    // widened.
    expect_rejects("merge temp whose arms disagree on width", R"(
function __main__():
block0:
    %0 = const_i64 1
    branch %0, block1, block2
block1:
    %1 = const_i64 7
    store __ifexpr0, %1
    jump block3
block2:
    %2 = const_i64 9
    store __ifexpr0, %2 : int[8]
    jump block3
block3:
    %3 = load __ifexpr0
    call print, %3
    return
)", "disagrees across branches");

    // A merge temp is not a hole in definite assignment: only the arms may
    // store it, and the join may only load it once every arm has. Here block2
    // stores nothing, so the load is genuinely reading an undefined value.
    expect_rejects("merge temp loaded on a path that never stored it", R"(
function __main__():
block0:
    %0 = const_i64 1
    branch %0, block1, block2
block1:
    %1 = const_i64 7
    store __ifexpr0, %1
    jump block3
block2:
    jump block3
block3:
    %3 = load __ifexpr0
    call print, %3
    return
)", "not definitely assigned");

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
