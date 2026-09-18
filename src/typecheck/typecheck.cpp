#include "typecheck.h"

#include <unordered_map>
#include <cstdint>
#include <algorithm>

namespace lithon::typecheck {

using namespace lithon::ir;

namespace {

struct LType {
    std::string kind;
    int width = -1;

    bool operator==(const LType& other) const {
        return kind == other.kind && width == other.width;
    }
};

std::pair<int64_t, int64_t> int_range(int width) {
    int64_t half = int64_t(1) << (width - 1);
    return {-half, half - 1};
}

std::string type_str(const LType& t) {
    if (t.width < 0) return t.kind;
    return t.kind + "[" + std::to_string(t.width) + "]";
}

class FunctionChecker {
public:
    FunctionChecker(const Module& module, const Function& fn, std::vector<RCRError>& errors)
        : module_(module), fn_(fn), errors_(errors) {}

    void run() {
        for (size_t i = 0; i < fn_.params.size(); ++i) {
            if (fn_.param_type_kinds[i].empty()) {
                error("function '" + fn_.name + "': parameter '" + fn_.params[i] +
                      "' has no type annotation (V1_SPEC 0.6.1, 0.6.8)");
                continue;
            }
            LType t{fn_.param_type_kinds[i], fn_.param_type_widths[i]};
            scope_[fn_.params[i]] = t;
        }

        bool has_return_type = !fn_.return_type_kind.empty();
        if (fn_.name != "main" && !has_return_type) {
            error("function '" + fn_.name + "' has no return type annotation (V1_SPEC 0.6.8)");
        }

        // First slice: straight-line only -- branch/merge (full 0.6.10)
        // is a later stage. Walk every block's instructions in file
        // order, tracking per-register types as we go.
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                check_instr(instr, has_return_type);
            }
        }
    }

private:
    const Module& module_;
    const Function& fn_;
    std::vector<RCRError>& errors_;
    std::unordered_map<std::string, LType> scope_;
    std::unordered_map<ValueId, LType> reg_types_;

    void error(const std::string& msg) {
        errors_.push_back(RCRError{msg});
    }

    bool reg_type(ValueId id, LType& out) {
        auto it = reg_types_.find(id);
        if (it == reg_types_.end()) return false;
        out = it->second;
        return true;
    }

    // Finds the ConstInt instruction (if any) that produced `id`, for
    // exact-value overflow/range checks. Linear scan -- fine for the
    // straight-line-only scope of this slice.
    const Instr* find_producing_const(ValueId id) {
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op == Op::ConstInt && instr.result == id) return &instr;
            }
        }
        return nullptr;
    }

    // 0.6.11: is it legal for `source` to flow into a slot declared `target`?
    void check_assignment_compatible(const LType& source, const LType& target,
                                      const std::string& context) {
        if (source.kind == target.kind) {
            if (source.width < 0 && target.width < 0) return; // bool -> bool
            if (source.width >= 0 && target.width >= 0) {
                if (target.width >= source.width) return;
                error(context + ": cannot narrow " + type_str(source) + " into " +
                      type_str(target) + " -- narrowing is never allowed (V1_SPEC 0.6.11)");
                return;
            }
            error(context + ": incompatible " + type_str(source) + " and " + type_str(target));
            return;
        }
        if (source.kind == "int" && target.kind == "float") return;
        if (source.kind == "float" && target.kind == "int") {
            error(context + ": float -> int conversion does not exist in Lithon "
                  "(V1_SPEC 0.6.11) -- no cast can perform this");
            return;
        }
        error(context + ": cannot convert " + type_str(source) + " to " + type_str(target) +
              " -- no such conversion exists");
    }

    // Given an operand register, returns its provable value range: an
    // exact (v, v) if it came from a literal, or its declared type's
    // full range otherwise. Returns false if the type isn't int.
    bool operand_range(ValueId id, int64_t& lo, int64_t& hi) {
        if (const Instr* c = find_producing_const(id)) {
            lo = hi = c->int_imm;
            return true;
        }
        LType t;
        if (!reg_type(id, t) || t.kind != "int") return false;
        auto [l, h] = int_range(t.width);
        lo = l; hi = h;
        return true;
    }

    void check_binop_fits_target(const Instr& binop_instr, const LType& target,
                                  const std::string& context) {
        if (target.kind != "int") return;
        if (binop_instr.op != Op::Add && binop_instr.op != Op::Sub && binop_instr.op != Op::Mul) return;

        int64_t lo1, hi1, lo2, hi2;
        if (!operand_range(binop_instr.args.at(0), lo1, hi1)) return;
        if (!operand_range(binop_instr.args.at(1), lo2, hi2)) return;

        int64_t possible_lo, possible_hi;
        if (binop_instr.op == Op::Add) {
            possible_lo = lo1 + lo2; possible_hi = hi1 + hi2;
        } else if (binop_instr.op == Op::Sub) {
            possible_lo = lo1 - hi2; possible_hi = hi1 - lo2;
        } else {
            int64_t corners[4] = {lo1*lo2, lo1*hi2, hi1*lo2, hi1*hi2};
            possible_lo = *std::min_element(corners, corners + 4);
            possible_hi = *std::max_element(corners, corners + 4);
        }

        auto [target_lo, target_hi] = int_range(target.width);
        if (possible_lo < target_lo || possible_hi > target_hi) {
            error(context + ": type " + type_str(target) + " is not wide enough -- this "
                  "operation can produce " + std::to_string(possible_lo) + ".." +
                  std::to_string(possible_hi) + ", which exceeds " + type_str(target) +
                  "'s range " + std::to_string(target_lo) + ".." + std::to_string(target_hi) +
                  ". Declare a wider type explicitly (V1_SPEC 0.5, 0.6.5) -- the compiler "
                  "will not auto-widen it for you.");
        }
    }

    // Finds the instruction (in any block) that produced `id`, for
    // binop-range checking at Store/Return sites.
    const Instr* find_producing_instr(ValueId id) {
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.result == id) return &instr;
            }
        }
        return nullptr;
    }

    void check_instr(const Instr& instr, bool has_return_type) {
        switch (instr.op) {
            case Op::ConstInt:
                reg_types_[instr.result] = LType{"int", 64};
                return;
            case Op::ConstFloat:
                reg_types_[instr.result] = LType{"float", 64};
                return;
            case Op::ConstBool:
                reg_types_[instr.result] = LType{"bool", -1};
                return;
            case Op::Load: {
                auto it = scope_.find(instr.name);
                if (it == scope_.end()) {
                    error("'" + instr.name + "' is not definitely assigned here (V1_SPEC 0.6.10)");
                    return;
                }
                reg_types_[instr.result] = it->second;
                return;
            }
            case Op::Add:
            case Op::Sub:
            case Op::Mul: {
                LType lhs, rhs;
                if (!reg_type(instr.args.at(0), lhs) || !reg_type(instr.args.at(1), rhs)) return;
                if (lhs.kind == "float" || rhs.kind == "float") {
                    reg_types_[instr.result] = LType{"float", 64};
                } else if (lhs.kind == "int" && rhs.kind == "int") {
                    reg_types_[instr.result] = LType{"int", std::max(lhs.width, rhs.width)};
                }
                return;
            }
            case Op::Store: {
                if (instr.type_kind.empty()) {
                    auto it = scope_.find(instr.name);
                    if (it == scope_.end()) {
                        error("'" + instr.name + "' is assigned without a type annotation "
                              "(V1_SPEC 0.6.1) -- write '" + instr.name + ": <type> = ...' first");
                        return;
                    }
                    check_value_into_target(instr.args.at(0), it->second,
                                             "re-assignment of '" + instr.name + "'");
                    return;
                }
                LType declared{instr.type_kind, instr.type_width};
                check_value_into_target(instr.args.at(0), declared,
                                         "declaration of '" + instr.name + "'");
                scope_[instr.name] = declared;
                return;
            }
            case Op::Return: {
                if (instr.args.empty()) return;
                if (!has_return_type) return; // already flagged missing annotation once
                LType target{fn_.return_type_kind, fn_.return_type_width};
                check_value_into_target(instr.args.at(0), target,
                                         "return in '" + fn_.name + "'");
                return;
            }
            default:
                return;
        }
    }

    // Shared by Store and Return: checks the value in register `id`
    // against `target` -- literal overflow (0.6.5), binop provable-range
    // overflow (0.5/0.6.5), or general conversion compatibility (0.6.11).
    void check_value_into_target(ValueId id, const LType& target, const std::string& context) {
        if (const Instr* c = find_producing_const(id)) {
            if (target.kind == "int") {
                auto [lo, hi] = int_range(target.width);
                if (c->int_imm < lo || c->int_imm > hi) {
                    error(context + ": literal " + std::to_string(c->int_imm) + " does not fit " +
                          type_str(target) + " (valid range " + std::to_string(lo) + ".." +
                          std::to_string(hi) + ") -- V1_SPEC 0.6.5");
                }
            }
            return;
        }

        if (const Instr* producer = find_producing_instr(id)) {
            if (producer->op == Op::Add || producer->op == Op::Sub || producer->op == Op::Mul) {
                check_binop_fits_target(*producer, target, context);
                return;
            }
        }

        LType source;
        if (reg_type(id, source)) {
            check_assignment_compatible(source, target, context);
        }
    }
};

} // namespace

std::vector<RCRError> check_module(const Module& module) {
    std::vector<RCRError> errors;
    for (const auto& fn : module.functions) {
        FunctionChecker checker(module, fn, errors);
        checker.run();
    }
    return errors;
}

} // namespace lithon::typecheck
