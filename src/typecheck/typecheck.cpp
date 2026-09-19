#include "typecheck.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lithon::typecheck {

using ValueId = uint32_t;
constexpr ValueId kInvalidValue = 0xFFFFFFFF;

enum class Op { Add, Sub, Mul, Const };

struct Type {
    int width;
    bool operator==(const Type& other) const { return width == other.width; }
    bool operator!=(const Type& other) const { return !(*this == other); }
};

struct Instr {
    Op op;
    ValueId result;
    ValueId op1;
    ValueId op2;
    int64_t const_val;
};

struct BasicBlock {
    std::vector<Instr> instrs;
};

struct Function {
    std::vector<BasicBlock> blocks;
};

using Scope = std::unordered_map<std::string, Type>;

std::pair<int64_t, int64_t> int_range(int width) {
    if (width <= 0) return {0, 0};
    if (width >= 64) {
        return {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    }
    int64_t half = int64_t(1) << (width - 1);
    return {-half, half - 1};
}

class FunctionChecker {
private:
    const Function& fn_;
    std::unordered_map<ValueId, const Instr*> def_map_;
    std::unordered_map<ValueId, int64_t> const_map_;
    std::vector<RCRError> errors_;

    void build_def_map() {
        def_map_.clear();
        const_map_.clear();
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.result != kInvalidValue) {
                    def_map_[instr.result] = &instr;
                    if (instr.op == Op::Const) {
                        const_map_[instr.result] = instr.const_val;
                    }
                }
            }
        }
    }

    const Instr* find_producing_instr(ValueId id) const {
        auto it = def_map_.find(id);
        return it != def_map_.end() ? it->second : nullptr;
    }

    const int64_t* find_producing_const(ValueId id) const {
        auto it = const_map_.find(id);
        return it != const_map_.end() ? &it->second : nullptr;
    }

    std::pair<int64_t, int64_t> operand_range(ValueId id) const {
        if (const int64_t* val = find_producing_const(id)) {
            return {*val, *val};
        }
        return {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    }

public:
    explicit FunctionChecker(const Function& fn) : fn_(fn) {
        build_def_map();
    }

    bool check_binop_fits_target(const Instr& binop_instr, const Type& target) const {
        auto [lo1, hi1] = operand_range(binop_instr.op1);
        auto [lo2, hi2] = operand_range(binop_instr.op2);

        __int128 p_lo = 0, p_hi = 0;
        __int128 l1 = lo1, h1 = hi1, l2 = lo2, h2 = hi2;

        if (binop_instr.op == Op::Add) {
            p_lo = l1 + l2;
            p_hi = h1 + h2;
        } else if (binop_instr.op == Op::Sub) {
            p_lo = l1 - h2;
            p_hi = h1 - l2;
        } else if (binop_instr.op == Op::Mul) {
            __int128 corners[4] = {l1 * l2, l1 * h2, h1 * l2, h1 * h2};
            p_lo = *std::min_element(corners, corners + 4);
            p_hi = *std::max_element(corners, corners + 4);
        } else {
            return false;
        }

        auto [target_lo, target_hi] = int_range(target.width);
        return p_lo >= target_lo && p_hi <= target_hi;
    }

    void merge_scopes(const std::vector<Scope>& pred_scopes, Scope& merged) {
        if (pred_scopes.empty()) return;

        std::unordered_map<std::string, const Type*> candidate_types;
        std::unordered_set<std::string> mismatch_vars;

        for (const auto& scope : pred_scopes) {
            for (const auto& [name, type] : scope) {
                auto it = candidate_types.find(name);
                if (it == candidate_types.end()) {
                    candidate_types[name] = &type;
                } else if (*it->second != type) {
                    mismatch_vars.insert(name);
                }
            }
        }

        for (const auto& [name, type_ptr] : candidate_types) {
            if (mismatch_vars.count(name)) {
                errors_.push_back(RCRError{"type of '" + name + "' disagrees across branches"});
                continue;
            }

            bool present_everywhere = true;
            for (const auto& scope : pred_scopes) {
                if (scope.find(name) == scope.end()) {
                    present_everywhere = false;
                    break;
                }
            }

            if (present_everywhere) {
                merged[name] = *type_ptr;
            }
        }
    }

    const std::vector<RCRError>& get_errors() const { return errors_; }
};

std::vector<RCRError> check_module(const lithon::ir::Module& module) {
    (void)module;
    return {};
}

} // namespace lithon::typecheck
