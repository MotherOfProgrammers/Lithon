// print_guard checks that need no IR text parser: IR is built by hand, so this
// runs anywhere print_guard.h compiles.
//
//   g++ -std=c++20 -O2 -Isrc -Isrc/jit -o build/print_guard_entry_test src/jit/print_guard_entry_test.cpp

#include <cstdio>
#include <string>
#include "ir/ir.h"
#include "print_guard.h"

using namespace lithon::ir;
using lithon::jit::check_print_safety;

static int g_failed = 0;

static void expect(const char* name, const Module& m, bool want_safe) {
    auto v = check_print_safety(m);
    bool ok = v.native_safe == want_safe;
    std::printf("[%s] %s (want %s, got %s)\n", ok ? "PASS" : "FAIL", name,
                want_safe ? "safe" : "refuse", v.native_safe ? "safe" : "refuse");
    for (const auto& r : v.reasons) std::printf("        reason: %s\n", r.c_str());
    if (!ok) ++g_failed;
}

static Instr I(Op op, ValueId result, std::vector<ValueId> args = {}, std::string name = "", int64_t imm = 0) {
    Instr i;
    i.op = op; i.result = result; i.args = std::move(args); i.name = std::move(name); i.int_imm = imm;
    return i;
}

static Function fn(const std::string& name, std::vector<std::string> params, std::vector<Instr> body) {
    Function f;
    f.name = name;
    f.params = std::move(params);
    BasicBlock b; b.label = "block0"; b.instrs = std::move(body);
    f.blocks = {b};
    return f;
}

int main() {
    const ValueId none = kInvalidValue;

    // print(1 < 2): a bool. The JIT prints it as True/False, so native is safe.
    for (const char* entry : {"main", "__main__"}) {
        Module m;
        m.functions = {fn(entry, {}, {I(Op::ConstInt, 0, {}, "", 1), I(Op::ConstInt, 1, {}, "", 2),
                                      I(Op::Lt, 2, {0, 1}), I(Op::Call, none, {2}, "print"), I(Op::Return, none)})};
        expect((std::string("bool print is native-safe, entry '") + entry + "'").c_str(), m, true);
    }

    // or(5, False): an int joined with a bool is not provably either.
    {
        Module m;
        m.functions = {fn("__main__", {}, {I(Op::ConstInt, 0, {}, "", 5), I(Op::ConstBool, 1, {}, "", 0),
                                           I(Op::Or, 2, {0, 1}), I(Op::Call, none, {2}, "print"), I(Op::Return, none)})};
        expect("int/bool mixed through or is refused", m, false);
    }

    // int / int is a float in the interpreter.
    {
        Module m;
        m.functions = {fn("__main__", {}, {I(Op::ConstInt, 0, {}, "", 6), I(Op::ConstInt, 1, {}, "", 3),
                                           I(Op::Div, 2, {0, 1}), I(Op::Call, none, {2}, "print"), I(Op::Return, none)})};
        expect("int / int (a float) is refused", m, false);
    }

    // helper(x) is called from __main__ with an int, so x is provably int.
    Function helper = fn("helper", {"x"}, {I(Op::Load, 0, {}, "x"), I(Op::Call, none, {0}, "print"), I(Op::Return, none)});
    {
        Module m;
        m.functions = {helper, fn("__main__", {}, {I(Op::ConstInt, 0, {}, "", 4), I(Op::Call, none, {0}, "helper"),
                                                   I(Op::Return, none)})};
        expect("callee param inferred int from a call in __main__", m, true);
    }

    // helper(x) is never called: it could be entered with anything.
    {
        Module m;
        m.functions = {helper, fn("__main__", {}, {I(Op::Return, none)})};
        expect("uncalled function with an untyped param is refused", m, false);
    }

    // bool passed to a callee that prints its param.
    {
        Module m;
        m.functions = {helper, fn("__main__", {}, {I(Op::ConstInt, 0, {}, "", 1), I(Op::ConstInt, 1, {}, "", 2),
                                                   I(Op::Lt, 2, {0, 1}), I(Op::Call, none, {2}, "helper"),
                                                   I(Op::Return, none)})};
        expect("bool argument printed inside a callee is native-safe", m, true);
    }

    std::printf(g_failed == 0 ? "PASS\n" : "FAIL\n");
    return g_failed == 0 ? 0 : 1;
}
