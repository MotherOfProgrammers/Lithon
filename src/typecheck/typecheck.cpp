#include "typecheck.h"

#include <unordered_map>
#include <unordered_set>
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

using Scope = std::unordered_map<std::string, LType>;

struct FnMeta {
    std::vector<LType> param_types;
    LType return_type;
    bool has_return_type = false;
};

class FunctionChecker {
public:
    FunctionChecker(const Module& module, const Function& fn, std::vector<RCRError>& errors,
                     const std::unordered_map<std::string, FnMeta>& all_fns)
        : module_(module), fn_(fn), errors_(errors), all_fns_(all_fns) {}

    void run() {
        bool has_return_type = !fn_.return_type_kind.empty();
        if (has_return_type) {
            return_type_ = LType{fn_.return_type_kind, fn_.return_type_width};
        }
        has_return_type_ = has_return_type;

        Scope entry_scope;
        for (size_t i = 0; i < fn_.params.size(); ++i) {
            if (fn_.param_type_kinds[i].empty()) {
                error("function '" + fn_.name + "': parameter '" + fn_.params[i] +
                      "' has no type annotation (V1_SPEC 0.6.1, 0.6.8)");
                continue;
            }
            entry_scope[fn_.params[i]] = LType{fn_.param_type_kinds[i], fn_.param_type_widths[i]};
        }
        if (fn_.name != "main" && !has_return_type) {
            error("function '" + fn_.name + "' has no return type annotation (V1_SPEC 0.6.8)");
        }

        build_block_graph();
        exempt_increment_stores_ = find_loop_increment_stores();
        walk_blocks(entry_scope);
        check_for_loop_shapes();
    }

private:
    const Module& module_;
    const Function& fn_;
    std::vector<RCRError>& errors_;
    const std::unordered_map<std::string, FnMeta>& all_fns_;
    std::unordered_map<ValueId, LType> reg_types_;

    LType return_type_;
    bool has_return_type_ = false;

    std::unordered_map<std::string, size_t> block_index_;
    std::unordered_map<std::string, std::vector<std::string>> predecessors_;
    std::unordered_map<std::string, Scope> out_scope_;
    std::unordered_map<std::string, Scope> in_scope_by_block_;
    std::unordered_set<const Instr*> exempt_increment_stores_;

    void error(const std::string& msg) {
        errors_.push_back(RCRError{msg});
    }

    void add_edge(const std::string& from, const std::string& to) {
        predecessors_[to].push_back(from);
    }

    void build_block_graph() {
        for (size_t i = 0; i < fn_.blocks.size(); ++i) {
            block_index_[fn_.blocks[i].label] = i;
        }
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op == Op::Jump) {
                    add_edge(block.label, instr.name);
                } else if (instr.op == Op::Branch) {
                    size_t comma = instr.name.find(',');
                    add_edge(block.label, instr.name.substr(0, comma));
                    add_edge(block.label, instr.name.substr(comma + 1));
                }
            }
        }
    }

    // Detects the exact "i = i + 1" shape frontend.py's build_for
    // always emits as a loop's increment step. Already guaranteed
    // safe by the separate 0.6.12 range-fit check on the loop's
    // bound, so it is exempt from the general provable-range check.
    std::unordered_set<const Instr*> find_loop_increment_stores() {
        std::unordered_set<const Instr*> exempt;
        for (const auto& block : fn_.blocks) {
            for (size_t i = 0; i + 3 < block.instrs.size(); ++i) {
                const Instr& load_i  = block.instrs[i];
                const Instr& const_1 = block.instrs[i + 1];
                const Instr& add_i   = block.instrs[i + 2];
                const Instr& store_i = block.instrs[i + 3];

                if (load_i.op != Op::Load) continue;
                if (const_1.op != Op::ConstInt || const_1.int_imm != 1) continue;
                if (add_i.op != Op::Add) continue;
                if (add_i.args.size() != 2) continue;
                if (add_i.args[0] != load_i.result || add_i.args[1] != const_1.result) continue;
                if (store_i.op != Op::Store) continue;
                if (store_i.name != load_i.name) continue;
                if (store_i.args.empty() || store_i.args[0] != add_i.result) continue;

                exempt.insert(&store_i);
            }
        }
        return exempt;
    }

    Scope merge_scopes(const std::vector<const Scope*>& preds, const std::string& block_label) {
        Scope merged;
        if (preds.empty()) return merged;
        if (preds.size() == 1) return *preds[0];

        std::unordered_set<std::string> all_names;
        for (const auto* s : preds)
            for (const auto& [name, t] : *s) all_names.insert(name);

        for (const auto& name : all_names) {
            bool present_everywhere = true;
            const LType* first = nullptr;
            bool disagreement = false;
            for (const auto* s : preds) {
                auto it = s->find(name);
                if (it == s->end()) { present_everywhere = false; continue; }
                if (!first) first = &it->second;
                else if (!(*first == it->second)) disagreement = true;
            }
            if (disagreement) {
                error("type of '" + name + "' disagrees across branches merging into '" +
                      block_label + "' (V1_SPEC 0.6.10)");
                continue;
            }
            if (present_everywhere && first) merged[name] = *first;
        }
        return merged;
    }

    void walk_blocks(const Scope& entry_scope) {
        for (size_t idx = 0; idx < fn_.blocks.size(); ++idx) {
            const BasicBlock& block = fn_.blocks[idx];

            Scope in_scope;
            auto pred_it = predecessors_.find(block.label);
            if (pred_it == predecessors_.end() || pred_it->second.empty()) {
                in_scope = entry_scope;
            } else {
                std::vector<const Scope*> ready_preds;
                for (const auto& pred_label : pred_it->second) {
                    auto bi = block_index_.find(pred_label);
                    if (bi != block_index_.end() && bi->second < idx) {
                        auto os = out_scope_.find(pred_label);
                        if (os != out_scope_.end()) ready_preds.push_back(&os->second);
                    }
                }
                in_scope = ready_preds.empty() ? entry_scope : merge_scopes(ready_preds, block.label);
            }

            in_scope_by_block_[block.label] = in_scope;

            Scope scope = in_scope;
            for (const auto& instr : block.instrs) {
                check_instr(instr, scope);
            }
            out_scope_[block.label] = std::move(scope);
        }
    }

    bool reg_type(ValueId id, LType& out) {
        auto it = reg_types_.find(id);
        if (it == reg_types_.end()) return false;
        out = it->second;
        return true;
    }

    const Instr* find_producing_const(ValueId id) {
        for (const auto& block : fn_.blocks)
            for (const auto& instr : block.instrs)
                if (instr.op == Op::ConstInt && instr.result == id) return &instr;
        return nullptr;
    }

    const Instr* find_producing_instr(ValueId id) {
        for (const auto& block : fn_.blocks)
            for (const auto& instr : block.instrs)
                if (instr.result == id) return &instr;
        return nullptr;
    }

    void check_assignment_compatible(const LType& source, const LType& target,
                                      const std::string& context) {
        if (source.kind == target.kind) {
            if (source.width < 0 && target.width < 0) return;
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

    bool operand_range(ValueId id, int64_t& lo, int64_t& hi) {
        if (const Instr* c = find_producing_const(id)) { lo = hi = c->int_imm; return true; }
        LType t;
        if (!reg_type(id, t) || t.kind != "int") return false;
        auto [l, h] = int_range(t.width);
        lo = l; hi = h;
        return true;
    }

    void check_binop_fits_target(const Instr& binop_instr, const LType& target,
                                  const std::string& context) {
        if (target.kind != "int") return;
        if (target.width >= 64) return; // 0.6.5's documented exception: int64 is the
                                          // max width, so overflow here is a runtime
                                          // trap, not a static rejection -- and there
                                          // is no wider type to require anyway. This
                                          // also avoids signed-overflow UB when
                                          // combining two near-INT64_MAX ranges below.
        if (binop_instr.op != Op::Add && binop_instr.op != Op::Sub && binop_instr.op != Op::Mul) return;

        int64_t lo1, hi1, lo2, hi2;
        if (!operand_range(binop_instr.args.at(0), lo1, hi1)) return;
        if (!operand_range(binop_instr.args.at(1), lo2, hi2)) return;

        int64_t possible_lo, possible_hi;
        if (binop_instr.op == Op::Add) { possible_lo = lo1 + lo2; possible_hi = hi1 + hi2; }
        else if (binop_instr.op == Op::Sub) { possible_lo = lo1 - hi2; possible_hi = hi1 - lo2; }
        else {
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

    void check_print_call(const Instr& instr) {
        if (instr.args.size() != 1) {
            error("print() with exactly one argument is supported");
            return;
        }
        LType t;
        if (!reg_type(instr.args.at(0), t)) return;
        static const std::unordered_set<std::string> allowed = {"int", "float", "str", "bool"};
        if (allowed.find(t.kind) == allowed.end()) {
            error("print() does not accept " + type_str(t) + " -- V1_SPEC 0.6.9's closed "
                  "overload set is int[N], float[N], str[N], bool only");
        }
    }

    void check_user_call(const Instr& instr) {
        auto it = all_fns_.find(instr.name);
        if (it == all_fns_.end()) {
            error("call to unknown function '" + instr.name + "'");
            return;
        }
        const FnMeta& callee = it->second;
        if (instr.args.size() != callee.param_types.size()) {
            error("'" + instr.name + "' expects " + std::to_string(callee.param_types.size()) +
                  " argument(s), got " + std::to_string(instr.args.size()) + " (V1_SPEC 0.6.8)");
            return;
        }
        for (size_t i = 0; i < instr.args.size(); ++i) {
            check_value_into_target(instr.args[i], callee.param_types[i],
                "argument " + std::to_string(i + 1) + " to '" + instr.name + "'");
        }
        if (instr.result != kInvalidValue && callee.has_return_type) {
            reg_types_[instr.result] = callee.return_type;
        }
    }

    void check_instr(const Instr& instr, Scope& scope) {
        switch (instr.op) {
            case Op::ConstInt:   reg_types_[instr.result] = LType{"int", 64}; return;
            case Op::ConstFloat: reg_types_[instr.result] = LType{"float", 64}; return;
            case Op::ConstBool:  reg_types_[instr.result] = LType{"bool", -1}; return;
            case Op::Load: {
                auto it = scope.find(instr.name);
                if (it == scope.end()) {
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
                if (lhs.kind == "float" || rhs.kind == "float") reg_types_[instr.result] = LType{"float", 64};
                else if (lhs.kind == "int" && rhs.kind == "int")
                    reg_types_[instr.result] = LType{"int", std::max(lhs.width, rhs.width)};
                return;
            }
            case Op::Div:
                // Per V1_SPEC 0.2: any division is true division and
                // always produces float.
                reg_types_[instr.result] = LType{"float", 64};
                return;
            case Op::Lt: case Op::Gt: case Op::Eq:
            case Op::And: case Op::Or: case Op::Not:
                reg_types_[instr.result] = LType{"bool", -1};
                return;
            case Op::Store: {
                if (instr.type_kind.empty()) {
                    auto it = scope.find(instr.name);
                    if (it == scope.end()) {
                        error("'" + instr.name + "' is assigned without a type annotation "
                              "(V1_SPEC 0.6.1) -- write '" + instr.name + ": <type> = ...' first");
                        return;
                    }
                    if (exempt_increment_stores_.count(&instr)) {
                        return;
                    }
                    check_value_into_target(instr.args.at(0), it->second,
                                             "re-assignment of '" + instr.name + "'");
                    return;
                }
                LType declared{instr.type_kind, instr.type_width};
                check_value_into_target(instr.args.at(0), declared,
                                         "declaration of '" + instr.name + "'");
                scope[instr.name] = declared;
                return;
            }
            case Op::Call: {
                if (instr.name == "print") {
                    check_print_call(instr);
                } else {
                    check_user_call(instr);
                }
                return;
            }
            case Op::Return: {
                if (instr.args.empty()) return;
                if (!has_return_type_) return;
                check_value_into_target(instr.args.at(0), return_type_,
                                         "return in '" + fn_.name + "'");
                return;
            }
            default:
                return;
        }
    }

    void check_for_loop_shapes() {
        for (const auto& block : fn_.blocks) {
            for (size_t i = 0; i + 1 < block.instrs.size(); ++i) {
                const Instr& load_instr = block.instrs[i];
                const Instr& lt_instr = block.instrs[i + 1];
                if (load_instr.op != Op::Load || lt_instr.op != Op::Lt) continue;
                if (lt_instr.args.size() != 2 || lt_instr.args[0] != load_instr.result) continue;

                const std::string& loop_var = load_instr.name;
                auto scope_it = in_scope_by_block_.find(block.label);
                if (scope_it == in_scope_by_block_.end()) continue;
                auto type_it = scope_it->second.find(loop_var);
                if (type_it == scope_it->second.end() || type_it->second.kind != "int") continue;

                const Instr* bound_const = find_producing_const(lt_instr.args[1]);
                if (!bound_const) continue;

                int64_t n = bound_const->int_imm;
                auto [lo, hi] = int_range(type_it->second.width);
                int64_t max_produced = n - 1;
                if (max_produced > hi || lo > 0) {
                    error("for-loop: range(" + std::to_string(n) + ") produces values up to " +
                          std::to_string(max_produced) + ", which does not fit " +
                          type_str(type_it->second) + " (valid range " + std::to_string(lo) +
                          ".." + std::to_string(hi) + ") -- V1_SPEC 0.6.12");
                }
            }
        }
    }
};

} // namespace

std::vector<RCRError> check_module(const Module& module) {
    std::vector<RCRError> errors;

    std::unordered_map<std::string, FnMeta> all_fns;
    for (const auto& fn : module.functions) {
        FnMeta meta;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            meta.param_types.push_back(LType{fn.param_type_kinds[i], fn.param_type_widths[i]});
        }
        meta.has_return_type = !fn.return_type_kind.empty();
        if (meta.has_return_type) {
            meta.return_type = LType{fn.return_type_kind, fn.return_type_width};
        }
        all_fns[fn.name] = meta;
    }

    for (const auto& fn : module.functions) {
        FunctionChecker checker(module, fn, errors, all_fns);
        checker.run();
    }
    return errors;
}

} // namespace lithon::typecheck
