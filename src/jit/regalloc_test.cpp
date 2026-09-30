// RegisterAllocator tests, written against the current allocator:
//   * temporaries: linear scan over abi::kTempPool, spill on pressure or when
//     the live range spans a Call
//   * named variables: promoted to abi::kPromotionPool (callee-saved) only when
//     select_promoted_variables() says the weighted use count justifies the
//     save/restore; otherwise a fixed stack slot
//   * r10/r11 and the argument registers are never handed to a value
//
//   g++ -std=c++20 -O2 -Isrc -Isrc/jit -o build/regalloc_test src/jit/regalloc_test.cpp

#include <cstdio>
#include <set>
#include <string>
#include "ir/ir.h"
#include "register_alloc.h"

using namespace lithon::ir;
using namespace lithon::jit;

static int g_failed = 0;

static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) ++g_failed;
}

// A variable's location as a comparable value: register number (0..15), or a
// stack offset encoded as (-1000 - offset) so it can never collide with a
// register id. Only used to assert two variables don't alias.
static int location_key(const RegisterAllocator& alloc, const std::string& name) {
    if (alloc.variable_in_register(name))
        return static_cast<int>(alloc.variable_register(name));
    return -1000 - alloc.variable_offset(name);
}

static Instr instr(Op op, ValueId result, std::vector<ValueId> args = {}, const char* name = "") {
    Instr i;
    i.op = op;
    i.result = result;
    i.args = std::move(args);
    i.name = name;
    return i;
}

static Instr const_int(ValueId result, int64_t v) {
    Instr i = instr(Op::ConstInt, result);
    i.int_imm = v;
    return i;
}

static bool is_temp_pool_reg(Reg r) {
    for (Reg p : abi::kTempPool) if (p == r) return true;
    return false;
}

static bool is_callee_saved_pool_reg(Reg r) {
    for (Reg p : abi::kPromotionPool) if (p == r) return true;
    return false;
}

int main() {
    // ---- lightly used params are NOT promoted (save/restore would cost more) ----
    {
        Function fn;
        fn.name = "add";
        fn.params = {"a", "b"};
        BasicBlock b0; b0.label = "block0";
        b0.instrs = {instr(Op::Load, 0, {}, "a"), instr(Op::Load, 1, {}, "b"),
                     instr(Op::Add, 2, {0, 1}), instr(Op::Return, kInvalidValue, {2})};
        fn.blocks = {b0};

        RegisterAllocator alloc(fn);
        check(alloc.has_variable("a") && alloc.has_variable("b"), "a and b are recognized as variables");
        check(location_key(alloc, "a") != location_key(alloc, "b"), "a and b do not alias the same location");
        check(!alloc.variable_in_register("a") && !alloc.variable_in_register("b"),
              "one entry store + one load each (weight 2) is below the promotion threshold: stack slots");
        check(alloc.callee_saved_slots().empty(), "nothing promoted, so nothing to save/restore");
        check(alloc.frame_size() % 16 == 0 && alloc.frame_size() >= 16, "frame holds both slots, 16-byte aligned");
        for (ValueId id : {0u, 1u, 2u})
            check(alloc.temp_location(id).in_register, "each of the 3 temporaries fits in a register");
    }

    // ---- a variable used inside a loop IS promoted, into a callee-saved register ----
    {
        Function fn;
        fn.name = "count";
        BasicBlock b0; b0.label = "block0";
        b0.instrs = {const_int(0, 0), instr(Op::Store, kInvalidValue, {0}, "i"),
                     instr(Op::Jump, kInvalidValue, {}, "loop")};
        BasicBlock lp; lp.label = "loop";
        lp.instrs = {instr(Op::Load, 1, {}, "i"), const_int(2, 10), instr(Op::Lt, 3, {1, 2}),
                     instr(Op::Branch, kInvalidValue, {3}, "body,exit")};
        BasicBlock body; body.label = "body";
        body.instrs = {instr(Op::Load, 4, {}, "i"), const_int(5, 1), instr(Op::Add, 6, {4, 5}),
                       instr(Op::Store, kInvalidValue, {6}, "i"), instr(Op::Jump, kInvalidValue, {}, "loop")};
        BasicBlock ex; ex.label = "exit";
        ex.instrs = {instr(Op::Return, kInvalidValue)};
        fn.blocks = {b0, lp, body, ex};

        RegisterAllocator alloc(fn);
        check(alloc.variable_in_register("i"), "loop variable is promoted to a register");
        bool callee_saved = false;
        for (Reg r : abi::kPromotionPool) if (alloc.variable_register("i") == r) callee_saved = true;
        check(callee_saved, "...and that register is from the callee-saved promotion pool");
        check(alloc.callee_saved_slots().size() == 1, "exactly the one register used is saved/restored");
        check(!is_temp_pool_reg(alloc.variable_register("i")), "a promoted variable never shares a register with the temp pool");
    }

    // ---- more simultaneously-live temporaries than the temp pool: something spills ----
    {
        const int pool = static_cast<int>(abi::kTempPool.size());
        for (int n : {pool - 1, pool + 1}) {   // n live constants + the running sum
            Function fn;
            fn.name = "wide";
            BasicBlock b0; b0.label = "block0";
            ValueId next = 0;
            std::vector<ValueId> consts;
            for (int i = 0; i < n; ++i) { b0.instrs.push_back(const_int(next, i + 1)); consts.push_back(next++); }
            ValueId acc = consts[0];
            for (int i = 1; i < n; ++i) {
                b0.instrs.push_back(instr(Op::Add, next, {acc, consts[static_cast<size_t>(i)]}));
                acc = next++;
            }
            b0.instrs.push_back(instr(Op::Return, kInvalidValue, {acc}));
            fn.blocks = {b0};

            RegisterAllocator alloc(fn);
            if (n == pool - 1)
                check(alloc.frame_size() == 0, "temps that exactly fill the pool (constants + running sum) need no stack frame");
            else
                check(alloc.frame_size() > 0, "more simultaneously-live temps than pool registers: something spills to the stack");

            std::set<int> used;
            bool clean = true;
            for (ValueId id = 0; id < next; ++id) {
                const auto& loc = alloc.temp_location(id);
                if (!loc.in_register) continue;
                Reg r = loc.reg;
                if (r == abi::kScratchLeft || r == abi::kScratchRight) clean = false;
                for (Reg a : abi::kArgRegs) if (r == a) clean = false;
                if (!is_temp_pool_reg(r)) clean = false;
                used.insert(static_cast<int>(r));
            }
            check(clean, "temps only ever land in the temp pool: never r10/r11 scratch or an argument register");
        }
    }

    // ---- a temp live across a call must not sit in a CALLER-SAVED register ----
    // It used to be spilled to the stack, which is correct but costs a store
    // and a reload straddling the call. A callee-saved register no promoted
    // variable is using is strictly better, and is what it gets now.
    {
        Function fn;
        fn.name = "spans_call";
        BasicBlock b0; b0.label = "block0";
        b0.instrs = {const_int(0, 7), instr(Op::Call, kInvalidValue, {}, "other"),
                     instr(Op::Add, 1, {0, 0}), instr(Op::Return, kInvalidValue, {1})};
        fn.blocks = {b0};

        RegisterAllocator alloc(fn);
        const auto& loc = alloc.temp_location(0);
        check(loc.in_register, "a call-spanning temp borrows a register rather than the stack");
        check(!is_temp_pool_reg(loc.reg),
              "...and that register is callee-saved, so the call cannot destroy it");
        check(!loc.in_register || is_callee_saved_pool_reg(loc.reg),
              "...and it comes from the callee-saved pool");
        check(alloc.temp_location(1).in_register, "the value computed after the call needs no spill");
        check(alloc.frame_size() % 16 == 0, "frame size is 16-byte aligned");

        // The borrow is only legal if the prologue saves it and every Return
        // restores it -- otherwise the CALLER's register is destroyed.
        bool saved = false;
        for (const auto& s : alloc.callee_saved_slots()) if (s.first == loc.reg) saved = true;
        check(saved, "the borrowed register is in callee_saved_slots(), so it is saved and restored");
    }

    // ---- a promoted variable's register is never lent to a temp ----
    {
        Function fn;
        fn.name = "promoted_then_call";
        BasicBlock b0; b0.label = "block0";
        b0.instrs = {instr(Op::Load, 0, {}, "hot"), const_int(1, 7),
                     instr(Op::Call, kInvalidValue, {}, "other"),
                     instr(Op::Add, 2, {0, 1}), instr(Op::Return, kInvalidValue, {2})};
        // Make "hot" promoted by loading it several times, as select_promoted_
        // variables would. Inserted BEFORE fn.blocks is assigned, since that
        // assignment copies the block.
        for (int i = 0; i < 4; ++i)
            b0.instrs.insert(b0.instrs.begin() + 1, instr(Op::Load, 10 + static_cast<unsigned>(i), {}, "hot"));
        fn.blocks = {b0};

        RegisterAllocator alloc(fn);
        Reg hot = alloc.variable_register("hot");
        check(alloc.variable_in_register("hot"), "hot variable is promoted");
        bool clash = false;
        for (ValueId id : {0u, 1u, 2u}) {
            const auto& l = alloc.temp_location(id);
            if (l.in_register && l.reg == hot) clash = true;
        }
        check(!clash, "no temporary shares the promoted variable's register");
    }

    // ---- more hot variables than promotion registers ----
    {
        const int pool = static_cast<int>(abi::kPromotionPool.size());
        const int nvars = pool + 1;
        Function fn;
        fn.name = "many_vars";
        BasicBlock b0; b0.label = "block0";
        ValueId next = 0;
        std::vector<std::string> names;
        for (int i = 0; i < nvars; ++i) {
            names.push_back("v" + std::to_string(i));
            b0.instrs.push_back(const_int(next, i));
            b0.instrs.push_back(instr(Op::Store, kInvalidValue, {next++}, names.back().c_str()));
            for (int k = 0; k < 3; ++k)                       // weight 1 store + 3 loads > threshold
                b0.instrs.push_back(instr(Op::Load, next++, {}, names.back().c_str()));
        }
        b0.instrs.push_back(instr(Op::Return, kInvalidValue));
        fn.blocks = {b0};

        RegisterAllocator alloc(fn);
        int in_reg = 0, on_stack = 0;
        for (const auto& n : names) (alloc.variable_in_register(n) ? in_reg : on_stack)++;
        std::printf("%d variables: %d in registers, %d on the stack\n", nvars, in_reg, on_stack);
        check(in_reg == pool, "exactly as many variables are promoted as the pool has registers");
        check(on_stack == 1, "the extra variable falls back to a stack slot");
        check(static_cast<int>(alloc.callee_saved_slots().size()) == pool, "every promoted register gets a save slot");

        bool distinct = true;
        for (size_t i = 0; i < names.size(); ++i)
            for (size_t j = i + 1; j < names.size(); ++j)
                if (location_key(alloc, names[i]) == location_key(alloc, names[j])) distinct = false;
        check(distinct, "all variables occupy distinct locations");
        check(alloc.frame_size() % 16 == 0, "frame size is 16-byte aligned");
    }

    std::printf(g_failed == 0 ? "PASS\n" : "FAIL\n");
    return g_failed == 0 ? 0 : 1;
}
