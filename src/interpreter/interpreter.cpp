#include "interpreter.h"
#include "runtime/value.h"

#include <iostream>
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
        return LithonValue::make_float(a / b);
    }

    if (either_float) {
        double a = lhs.is_float() ? lhs.as_float() : static_cast<double>(lhs.as_int());
        double b = rhs.is_float() ? rhs.as_float() : static_cast<double>(rhs.as_int());
        switch (op) {
            case Op::Add: return LithonValue::make_float(a + b);
            case Op::Sub: return LithonValue::make_float(a - b);
            case Op::Mul: return LithonValue::make_float(a * b);
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
        default:
            throw std::runtime_error("interpreter: not a binary arithmetic op");
    }
}

// Comparisons: numeric only in this slice, int/float mix allowed
// (promoted to double for the comparison), matching 0.2's "mixed
// numeric -> bool" rule.
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
    // Per V1_SPEC 0.2 truthiness: False, 0, 0.0, None are false.
    // and/or here follow Python's short-circuit VALUE semantics
    // (return one of the operands, not necessarily a bool) --
    // matches "0 and 1" -> 0, "1 or 0" -> 1 in boolean.py.
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
        double f = v.as_float();
        if (f == static_cast<int64_t>(f)) {
            std::cout << static_cast<int64_t>(f) << ".0\n";
        } else {
            std::cout << f << "\n";
        }
    }
    else throw std::runtime_error("interpreter: print() of an unsupported value kind in this slice");
}

const Function* find_function(const Module& module, const std::string& name) {
    for (const auto& fn : module.functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

} // namespace

void run_main(const Module& module) {
    const Function* main_fn = find_function(module, "main");
    if (!main_fn) {
        throw std::runtime_error("interpreter: no 'main' function in module");
    }

    Frame frame;

    for (const auto& block : main_fn->blocks) {
        for (const auto& instr : block.instrs) {
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
                case Op::Call:
                    if (instr.name == "print") {
                        do_print(frame.get_reg(instr.args.at(0)));
                    } else {
                        throw std::runtime_error("interpreter: unknown call target '" + instr.name + "'");
                    }
                    break;
                case Op::Return:
                    return;
                default:
                    throw std::runtime_error("interpreter: opcode not yet implemented in this slice");
            }
        }
    }
}

} // namespace lithon::interp
