#include "interpreter.h"
#include "runtime/value.h"
#include "jit/float_runtime.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <stdexcept>

namespace lithon::interp {

using lithon::LithonValue;
using namespace lithon::ir;

namespace {

class Frame {
public:
    std::unordered_map<ValueId, LithonValue> regs;
    std::unordered_map<std::string, LithonValue> vars;

    LithonValue get_reg(ValueId id) const {
        auto it = regs.find(id);
        if (it == regs.end()) {
            throw std::runtime_error("interpreter: reference to undefined value id");
        }
        return it->second;
    }

    LithonValue get_var(const std::string& name) const {
        auto it = vars.find(name);
        if (it == vars.end()) {
            throw std::runtime_error("interpreter: reference to undefined variable '" + name + "'");
        }
        return it->second;
    }
};

LithonValue apply_binop(Op op, LithonValue lhs, LithonValue rhs) {
    bool either_float = lhs.is_float() || rhs.is_float();

    if (!lhs.is_int() && !lhs.is_float()) {
        throw std::runtime_error("interpreter: binop on a non-numeric value");
    }
    if (!rhs.is_int() && !rhs.is_float()) {
        throw std::runtime_error("interpreter: binop on a non-numeric value");
    }

    if (op == Op::Div) {
        double a = lhs.is_float() ? lhs.as_float() : static_cast<double>(lhs.as_int());
        double b = rhs.is_float() ? rhs.as_float() : static_cast<double>(rhs.as_int());
        // Python raises ZeroDivisionError for any zero divisor, including
        // floats and -0.0, rather than producing inf/nan the way raw IEEE
        // hardware division does. The JIT's Div emits a comisd check for the
        // same condition, so the two engines agree here by construction --
        // which is what lets run_tier_diff.py treat the interpreter as an
        // oracle for float code at all.
        if (b == 0.0) throw std::runtime_error("interpreter: division by zero");
        return LithonValue::make_float(a / b);
    }

    if (either_float) {
        double a = lhs.is_float() ? lhs.as_float() : static_cast<double>(lhs.as_int());
        double b = rhs.is_float() ? rhs.as_float() : static_cast<double>(rhs.as_int());
        switch (op) {
            case Op::Add: return LithonValue::make_float(a + b);
            case Op::Sub: return LithonValue::make_float(a - b);
            case Op::Mul: return LithonValue::make_float(a * b);
            case Op::Mod: {
                // Same zero-divisor rule as Div above, for the same reason: the
                // JIT checks it, so the two engines agree by construction. NaN
                // divisors propagate rather than trap, again matching Div.
                if (b == 0.0) throw std::runtime_error("interpreter: modulo by zero");
                // fmod is exactly the truncated (sign-follows-dividend)
                // remainder, so the float path needs no sign correction.
                double r = std::fmod(a, b);
                // A zero result is normalized to +0.0. fmod keeps the dividend's
                // sign, so fmod(-0.0, 2.0) is -0.0, but the JIT's four-instruction
                // a - trunc(a/b)*b sequence cannot: IEEE defines x - x as +0.0,
                // so it necessarily loses the sign on an exact division. Rather
                // than spend three more instructions (shift the sign bit out, shift
                // it back, XOR) to reproduce a distinction print shows but
                // arithmetic never observes, both engines return +0.0, which is
                // also what Python does: -0.0 % 2.0 is 0.0.
                return LithonValue::make_float(r == 0.0 ? 0.0 : r);
            }
            default:
                throw std::runtime_error("interpreter: not a binary arithmetic op");
        }
    }

    int64_t a = lhs.as_int();
    int64_t b = rhs.as_int();
    switch (op) {
        case Op::Add: return LithonValue::make_int(a + b);
        case Op::Sub: return LithonValue::make_int(a - b);
        case Op::Mul: return LithonValue::make_int(a * b);
        case Op::Mod: {
            if (b == 0) throw std::runtime_error("interpreter: modulo by zero");
            // int64_t has no representable result for INT64_MIN % -1: it is 2^63,
            // one past the maximum. C++ leaves that undefined and x86 `idiv`
            // raises #DE, which would take the whole process down instead of
            // producing a value. Wraparound arithmetic elsewhere in this
            // interpreter is implemented as C++ overflow, so the defined
            // answer here is the wrapped one: 0, which is also what the JIT
            // emits. Both engines must agree, and 0 is the only choice that
            // does not involve trapping.
            if (a == std::numeric_limits<int64_t>::min() && b == -1) return LithonValue::make_int(0);
            return LithonValue::make_int(a % b);
        }
        default:
            throw std::runtime_error("interpreter: not a binary arithmetic op");
    }
}

LithonValue apply_compare(Op op, LithonValue lhs, LithonValue rhs) {
    if ((!lhs.is_int() && !lhs.is_float()) || (!rhs.is_int() && !rhs.is_float())) {
        throw std::runtime_error("interpreter: comparison on a non-numeric value");
    }
    double a = lhs.is_float() ? lhs.as_float() : static_cast<double>(lhs.as_int());
    double b = rhs.is_float() ? rhs.as_float() : static_cast<double>(rhs.as_int());
    switch (op) {
        case Op::Lt: return LithonValue::make_bool(a < b);
        case Op::Gt: return LithonValue::make_bool(a > b);
        case Op::Eq: return LithonValue::make_bool(a == b);
        default:
            throw std::runtime_error("interpreter: not a comparison op");
    }
}

LithonValue apply_boolop(Op op, LithonValue lhs, LithonValue rhs) {
    switch (op) {
        case Op::And: return lhs.is_truthy() ? rhs : lhs;
        case Op::Or:  return lhs.is_truthy() ? lhs : rhs;
        default:
            throw std::runtime_error("interpreter: not a bool op");
    }
}

void do_print(LithonValue v) {
    if (v.is_bool())        std::cout << (v.as_bool() ? "True" : "False") << "\n";
    else if (v.is_int())    std::cout << v.as_int() << "\n";
    else if (v.is_float()) {
        // Deliberately the SAME function the JIT's emitted code calls, not
        // std::cout. Two reasons, and the second is the important one:
        //
        //   1. Correctness. `std::cout << double` defaults to 6 significant
        //      digits and prints "0.333333" for 1/3 and "1" for
        //      1.0000000000000002. CPython's repr is the shortest string
        //      that round-trips, so it prints "0.3333333333333333" and
        //      "1.0000000000000002". The interpreter is used as the oracle
        //      for float code, so a lossy default here would make correct
        //      JIT output look like a JIT bug.
        //   2. The whole point of the tiered engine is that both tiers
        //      produce byte-identical stdout. Sharing one formatter makes
        //      that structural instead of a coincidence to be maintained,
        //      and it keeps signed zero and the sign of NaN (which the
        //      stream operator also disagrees with CPython about) in step.
        std::cout << lithon::jit::host_format_double(v.as_float()) << "\n";
    }
    else throw std::runtime_error("interpreter: print() of an unsupported value kind in this slice");
}

const Function* find_function(const Module& module, const std::string& name) {
    for (const auto& fn : module.functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

std::pair<std::string, std::string> split_branch_targets(const std::string& s) {
    size_t comma = s.find(',');
    if (comma == std::string::npos) {
        throw std::runtime_error("interpreter: malformed branch targets: " + s);
    }
    return {s.substr(0, comma), s.substr(comma + 1)};
}

// Executes one function call: builds a fresh Frame, binds arg_values to
// fn.params, walks blocks until Return, and returns the result. Calls
// to user-defined functions recurse into this same routine -- each
// recursive call gets its own Frame on the real C++ call stack, which
// is what makes recursion (fib.py etc.) work correctly with isolated
// locals per call, with no extra machinery needed.
LithonValue execute_function(const Module& module, const Function& fn,
                              const std::vector<LithonValue>& arg_values) {
    if (arg_values.size() != fn.params.size()) {
        throw std::runtime_error("interpreter: argument count mismatch calling '" + fn.name + "'");
    }
    if (fn.blocks.empty()) {
        throw std::runtime_error("interpreter: function '" + fn.name + "' has no blocks");
    }

    Frame frame;
    for (size_t i = 0; i < fn.params.size(); ++i) {
        frame.vars[fn.params[i]] = arg_values[i];
    }

    std::unordered_map<std::string, const BasicBlock*> label_to_block;
    for (const auto& block : fn.blocks) {
        label_to_block[block.label] = &block;
    }

    LithonValue return_value = LithonValue::make_none();
    const BasicBlock* cur = &fn.blocks.front();

    while (true) {
        bool jumped = false;
        bool returned = false;

        for (const auto& instr : cur->instrs) {
            switch (instr.op) {
                case Op::ConstInt:
                    frame.regs[instr.result] = LithonValue::make_int(instr.int_imm);
                    break;
                case Op::ConstFloat:
                    frame.regs[instr.result] = LithonValue::make_float(instr.float_imm);
                    break;
                case Op::ConstBool:
                    frame.regs[instr.result] = LithonValue::make_bool(instr.int_imm != 0);
                    break;
                case Op::Load:
                    frame.regs[instr.result] = frame.get_var(instr.name);
                    break;
                case Op::Store:
                    frame.vars[instr.name] = frame.get_reg(instr.args.at(0));
                    break;
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Mod:
                    frame.regs[instr.result] = apply_binop(
                        instr.op, frame.get_reg(instr.args.at(0)), frame.get_reg(instr.args.at(1)));
                    break;
                case Op::Lt:
                case Op::Gt:
                case Op::Eq:
                    frame.regs[instr.result] = apply_compare(
                        instr.op, frame.get_reg(instr.args.at(0)), frame.get_reg(instr.args.at(1)));
                    break;
                case Op::And:
                case Op::Or:
                    frame.regs[instr.result] = apply_boolop(
                        instr.op, frame.get_reg(instr.args.at(0)), frame.get_reg(instr.args.at(1)));
                    break;
                case Op::Not: {
                    LithonValue v = frame.get_reg(instr.args.at(0));
                    frame.regs[instr.result] = LithonValue::make_bool(!v.is_truthy());
                    break;
                }
                case Op::Call: {
                    if (instr.name == "print") {
                        do_print(frame.get_reg(instr.args.at(0)));
                        break;
                    }
                    const Function* callee = find_function(module, instr.name);
                    if (!callee) {
                        throw std::runtime_error("interpreter: unknown call target '" + instr.name + "'");
                    }
                    std::vector<LithonValue> args;
                    for (ValueId id : instr.args) {
                        args.push_back(frame.get_reg(id));
                    }
                    LithonValue result = execute_function(module, *callee, args);
                    if (instr.result != kInvalidValue) {
                        frame.regs[instr.result] = result;
                    }
                    break;
                }
                case Op::Branch: {
                    LithonValue cond = frame.get_reg(instr.args.at(0));
                    auto [then_label, else_label] = split_branch_targets(instr.name);
                    const std::string& target = cond.is_truthy() ? then_label : else_label;
                    auto it = label_to_block.find(target);
                    if (it == label_to_block.end()) {
                        throw std::runtime_error("interpreter: branch to unknown block '" + target + "'");
                    }
                    cur = it->second;
                    jumped = true;
                    break;
                }
                case Op::Jump: {
                    auto it = label_to_block.find(instr.name);
                    if (it == label_to_block.end()) {
                        throw std::runtime_error("interpreter: jump to unknown block '" + instr.name + "'");
                    }
                    cur = it->second;
                    jumped = true;
                    break;
                }
                case Op::Return:
                    if (!instr.args.empty()) {
                        return_value = frame.get_reg(instr.args.at(0));
                    }
                    returned = true;
                    break;
                default:
                    throw std::runtime_error("interpreter: opcode not yet implemented in this slice");
            }
            if (jumped || returned) break;
        }

        if (returned) return return_value;
        if (!jumped) return return_value; // fell off the end -- safety net
    }
}

} // namespace

void run_main(const Module& module) {
    // Entry point: the frontend emits __main__ (so user code may define its own
    // `main`); hand-written IR may still use a plain `main`.
    const Function* main_fn = find_function(module, "__main__");
    if (!main_fn) main_fn = find_function(module, "main");
    if (!main_fn) {
        throw std::runtime_error("interpreter: no '__main__' or 'main' function in module");
    }
    execute_function(module, *main_fn, {});
}

} // namespace lithon::interp
