#include "typecheck.h"

#include <unordered_map>
#include <cstdint>

namespace lithon::typecheck {

using namespace lithon::ir;

namespace {

struct LType {
    std::string kind;   // "int" | "float" | "str" | "bool"
    int width = -1;      // -1 for bool

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
        // Seed scope with parameters -- every param must be typed (0.6.8).
        for (size_t i = 0; i < fn_.params.size(); ++i) {
            if (fn_.param_type_kinds[i].empty()) {
                error("function '" + fn_.name + "': parameter '" + fn_.params[i] +
                      "' has no type annotation (V1_SPEC 0.6.1, 0.6.8)");
                continue;
            }
            LType t{fn_.param_type_kinds[i], fn_.param_type_widths[i]};
            scope_[fn_.params[i]] = t;
        }

        if (fn_.name != "main" && fn_.return_type_kind.empty()) {
            error("function '" + fn_.name + "' has no return type annotation (V1_SPEC 0.6.8)");
        }

        // First slice: straight-line only. Walk every block's
        // instructions in file order -- branch/merge handling is a
        // later stage (0.6.10's full form).
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                check_instr(instr);
            }
        }
    }

private:
    const Module& module_;
    const Function& fn_;
    std::vector<RCRError>& errors_;
    std::unordered_map<std::string, LType> scope_;

    void error(const std::string& msg) {
        errors_.push_back(RCRError{msg});
    }

    void check_instr(const Instr& instr) {
        switch (instr.op) {
            case Op::Store: {
                // 0.6.1: a Store with no type_kind means the frontend
                // emitted it from a bare, untyped assignment -- reject,
                // UNLESS this name was already declared with a type
                // (0.6.4 re-assignment).
                if (instr.type_kind.empty()) {
                    auto it = scope_.find(instr.name);
                    if (it == scope_.end()) {
                        error("'" + instr.name + "' is assigned without a type annotation "
                              "(V1_SPEC 0.6.1) -- write '" + instr.name + ": <type> = ...' first");
                        return;
                    }
                    // 0.6.4: re-assignment, checked against the declared type.
                    check_overflow_if_literal(instr, it->second);
                    return;
                }

                // 0.6.1/0.6.4: first declaration (or an explicit
                // re-declaration -- always starts a fresh binding).
                LType declared{instr.type_kind, instr.type_width};
                check_overflow_if_literal(instr, declared);
                scope_[instr.name] = declared;
                return;
            }
            default:
                return; // other opcodes not yet checked in this first slice
        }
    }

    // 0.6.5: if the value being stored came directly from a literal
    // const instruction earlier in the SAME block, check it against
    // `declared`'s range. This first slice only looks at the
    // immediately preceding const instruction in program order as a
    // simple heuristic -- full dataflow tracking comes with the
    // branch-aware rewrite in a later stage.
    void check_overflow_if_literal(const Instr& store_instr, const LType& declared) {
        if (declared.kind != "int") return;
        // Look up the producing instruction for store_instr.args[0]
        // by scanning this function's blocks -- first slice, linear
        // scan is fine given straight-line-only scope.
        for (const auto& block : fn_.blocks) {
            for (const auto& candidate : block.instrs) {
                if (candidate.op == Op::ConstInt && candidate.result == store_instr.args.at(0)) {
                    auto [lo, hi] = int_range(declared.width);
                    if (candidate.int_imm < lo || candidate.int_imm > hi) {
                        error("literal " + std::to_string(candidate.int_imm) +
                              " does not fit " + type_str(declared) +
                              " (valid range " + std::to_string(lo) + ".." + std::to_string(hi) +
                              ") -- V1_SPEC 0.6.5");
                    }
                    return;
                }
            }
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
