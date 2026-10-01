#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "float_runtime.h"
#include "ir/ir.h"
#include "jit_abi.h"
#include "liveness.h"
#include "optimize.h"
#include "print_guard.h"
#include "register_alloc.h"
#include "x86_encoder.h"

// The bridge from typed ir::Functions to runnable x86-64 machine code.
//
// Pipeline per function:
//   optimize_function      constant folding + dead-code elimination
//   select_promoted_...    hot variables -> callee-saved registers
//   plan_function          decide, per %N temp, whether it needs a
//                          register at all:
//                            Const      -> an immediate, no instruction
//                            Alias      -> "is" a promoted variable's
//                                          register, no copy
//                            FusedCmp   -> folded into the Branch that
//                                          consumes it (cmp + jcc)
//                            FusedStore -> computed directly in the
//                                          variable's register
//   RegisterAllocator      linear scan for what is left
//   emit                   machine code, with loop rotation and
//                          (for simple single-block bodies) unrolling
//
// For `while i < N: total = total + i; i = i + 1` this turns the old
//   mov rcx,[rbp-8]; mov rax,N; cmp; setl; movzx; test; jnz; ...
// (three memory operations per variable update) into
//   add r13, r12 ; add r12, 1 ; cmp r12, N ; jl body
//
// SCRATCH ROLES: r10 = left operand / spilled result, r11 = right
// operand / indirect-call target. Two independent scratch registers make
// the "both operands spilled" clobber bug structurally impossible.
//
// print() FORMATTING: which format a print() call uses is decided by the
// same whole-module Kind analysis print_guard.h uses to gate the
// tier_runner (infer_value_kinds, computed once up front from the
// *original* module -- folding/DCE never change a surviving value's id,
// so looking values up by id against the pre-optimization module stays
// correct after optimize_function rewrites a private copy). A provably-
// Bool argument prints True/False; a provably-Int argument prints as a
// decimal. Anything else (float, or a join of incomparable kinds) throws,
// so a caller either doesn't reach this compiler at all (tier_runner's
// guard already refused it) or gets a clear, specific error (lithon_jit,
// which has no guard) instead of silently mis-printing a float as a
// truncated integer.
//
namespace lithon::jit {

namespace {
static const char kIntPrintFormat[] = "%lld\n";
// No format specifiers, so these are passed directly as printf's sole
// argument (its "format string") -- safe since printf treats a string
// with no '%' as a literal, and it saves marshalling a second argument.
static const char kBoolTrueLiteral[] = "True\n";
static const char kBoolFalseLiteral[] = "False\n";
// A double is printed by handing printf a pre-formatted string, NOT with
// "%f": CPython renders a float with repr(), which is the shortest decimal
// string that round-trips, so 3.5 prints as "3.5" and 7.0 as "7.0". "%f"
// would print "3.500000" and "7.000000", and since run_tier_diff.py diffs
// stdout against the interpreter byte-for-byte, that mismatch would be
// reported as a JIT bug on every single float. format_double() below
// produces CPython's exact text, so the format string carries no specifier
// and can be passed as printf's sole argument like the bool case.
static const char kFloatPrintFormat[] = "%s\n";
}

struct CompileOptions {
    bool optimize = true;          // constant folding + dead-code elimination
    bool strength_reduce = true;   // invariant*IV -> repeated add (needs optimize)
    bool promote_registers = true; // keep hot variables in registers
    // Let a temporary that is live across a call borrow a callee-saved
    // register no promoted variable is using, instead of living on the stack.
    bool borrow_callee_saved = true;
    bool rotate_loops = true;      // duplicate small loop headers at the back edge
    int unroll_factor = 4;         // copies of a simple loop body per back edge (1 = off)
    // Also unroll loops whose body is an if/else diamond. Off by default: it is
    // correct and it is what `--diamond` fuzzes, but it measured consistently
    // SLOWER than not unrolling on Sandy Bridge -- 10% on a tight
    // if/else-with-multiply loop, 3.6% with a heavier body. A diamond's
    // if/else test is irreducible, so unrolling cannot remove a branch per
    // iteration the way it does for a straight-line body; it only amortises
    // the back edge, while the four copies inflate the loop ~2x and cost more
    // in the loop buffer / uop cache than the saved back edge is worth.
    // Straight-line unrolling above is the variant that pays.
    bool unroll_diamonds = false;
};

struct CompiledModule {
    std::vector<uint8_t> code;
    std::unordered_map<std::string, size_t> function_offset;
    // Raw double bits of every ConstFloat that survived to codegen,
    // appended after the last function so it sits inside the same
    // executable mapping. x86-64 has no "mov xmm, imm64", so a double
    // literal is only ever reachable as a load from memory; the pool IS
    // that memory. Empty for every module with no float constants,
    // which is all pre-float code.
    std::vector<uint64_t> float_pool;
};

namespace detail {

struct TempInfo {
    enum class Kind : uint8_t { Normal, Const, Alias, FusedCmp, FusedStore };
    Kind kind = Kind::Normal;
    int64_t imm = 0;
    // The double this temp folds to, when kind == Const and the value is a
    // float. The int `imm` above cannot hold one: a double needs all 64 bits
    // of significand and exponent, which an int64_t would round. A Const temp
    // with dbl set is a float constant that plan_function recorded; the
    // "Const" case in load_float_value reads exactly this.
    double dbl = 0.0;
    Reg alias = Reg::RAX;
};

struct FunctionPlan {
    std::unordered_map<lithon::ir::ValueId, TempInfo> info;
    // The single source of truth for "this value never occupies a register":
    // every id marked Const/Alias/FusedCmp/FusedStore below. Liveness and
    // register allocation both consume this exact set (see liveness.h).
    VirtualTemps virtual_temps;
    std::vector<std::vector<uint8_t>> skip_instr;   // [block][pos]: instruction fused away
};

inline FunctionPlan plan_function(const lithon::ir::Function& fn, const PromotionMap& promoted) {
    using namespace lithon::ir;
    FunctionPlan plan;
    plan.skip_instr.resize(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) plan.skip_instr[b].assign(fn.blocks[b].instrs.size(), 0);

    struct Site { size_t b, p; };
    std::unordered_map<ValueId, std::vector<Site>> uses;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (size_t p = 0; p < fn.blocks[b].instrs.size(); ++p)
            for (auto arg : fn.blocks[b].instrs[p].args) uses[arg].push_back({b, p});

    auto set = [&](ValueId id, TempInfo ti) {
        plan.info[id] = ti;
        plan.virtual_temps.insert(id);
    };

    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& ins = fn.blocks[b].instrs;
        for (size_t p = 0; p < ins.size(); ++p) {
            const Instr& in = ins[p];

            if (in.op == Op::ConstInt || in.op == Op::ConstBool) {
                // Both store their value in int_imm (ir.h / text_parser.cpp).
                TempInfo ti; ti.kind = TempInfo::Kind::Const; ti.imm = in.int_imm;
                set(in.result, ti);
                continue;
            }

            if (in.op == Op::ConstFloat) {
                // Same virtual-temp treatment as an integer constant: the
                // double is re-emitted as a pool load at each use rather than
                // occupying a register. The value lives in `dbl` because
                // float_imm cannot be squeezed into int_imm without rounding.
                TempInfo ti; ti.kind = TempInfo::Kind::Const; ti.dbl = in.float_imm;
                set(in.result, ti);
                continue;
            }

            if (in.op == Op::Load && promoted.count(in.name)) {
                // A load of a promoted variable needs no copy when the
                // variable cannot change while the loaded value is live:
                // every use is later in the same block, with no Store to
                // that variable in between.
                const auto& us = uses[in.result];
                bool ok = true;
                size_t last = p;
                for (const auto& u : us) {
                    if (u.b != b || u.p <= p) { ok = false; break; }
                    last = std::max(last, u.p);
                }
                for (size_t q = p + 1; ok && q < last; ++q) {
                    if (ins[q].op == Op::Store && ins[q].name == in.name) ok = false;
                }
                if (ok) {
                    TempInfo ti; ti.kind = TempInfo::Kind::Alias; ti.alias = promoted.at(in.name);
                    set(in.result, ti);
                }
                continue;
            }

            const bool has_next = p + 1 < ins.size();

            if ((in.op == Op::Lt || in.op == Op::Gt || in.op == Op::Eq) && has_next &&
                ins[p + 1].op == Op::Branch && ins[p + 1].args.size() == 1 &&
                ins[p + 1].args[0] == in.result && uses[in.result].size() == 1) {
                TempInfo ti; ti.kind = TempInfo::Kind::FusedCmp;
                set(in.result, ti);
                continue;
            }

            if ((in.op == Op::Add || in.op == Op::Sub || in.op == Op::Mul) && has_next &&
                ins[p + 1].op == Op::Store && ins[p + 1].args.size() == 1 &&
                ins[p + 1].args[0] == in.result && uses[in.result].size() == 1 &&
                promoted.count(ins[p + 1].name)) {
                TempInfo ti; ti.kind = TempInfo::Kind::FusedStore;
                set(in.result, ti);
                plan.skip_instr[b][p + 1] = 1;
                continue;
            }
        }
    }
    return plan;
}

// A loop header is worth duplicating at its back edge when it is tiny,
// side-effect free, and ends in a conditional branch.
inline bool is_rotatable_header(const lithon::ir::BasicBlock& block) {
    using lithon::ir::Op;
    if (block.instrs.empty() || block.instrs.size() > 10) return false;
    if (block.instrs.back().op != Op::Branch) return false;
    for (const auto& in : block.instrs) {
        switch (in.op) {
            case Op::ConstInt: case Op::ConstBool: case Op::Load:
            case Op::Add: case Op::Sub: case Op::Mul:
            case Op::Lt: case Op::Gt: case Op::Eq: case Op::Branch:
                break;
            default:
                return false;
        }
    }
    return true;
}

// A loop body qualifies for unrolling when it is straight-line, pure
// arithmetic/moves that ends in the back-edge Jump: no calls, returns
// or inner control flow to duplicate.
inline bool is_unrollable_body(const lithon::ir::BasicBlock& block) {
    using lithon::ir::Op;
    if (block.instrs.size() < 2 || block.instrs.size() > 16) return false;
    if (block.instrs.back().op != Op::Jump) return false;
    for (size_t i = 0; i + 1 < block.instrs.size(); ++i) {
        switch (block.instrs[i].op) {
            case Op::ConstInt: case Op::ConstBool: case Op::Load: case Op::Store:
            case Op::Add: case Op::Sub: case Op::Mul:
            case Op::Lt: case Op::Gt: case Op::Eq:
            case Op::And: case Op::Or: case Op::Not:
                break;
            default:
                return false;
        }
    }
    return true;
}

// Flags for emit_block_body.
enum : unsigned {
    kAllowRotate = 1,          // a back-edge Jump may be replaced by an inlined header
    kStopBeforeTerminator = 2, // emit everything except the block's final Jump
    kExitOnlyBranch = 4        // Branch: jump out when false, fall through when true
};
constexpr size_t kNoLocal = static_cast<size_t>(-1);

// Where a Branch should land when its block is being inlined into a
// straight-line body. kNoLocal on both fields means "use the block's real
// IR targets". A Branch may set at most one.
struct LocalBranch {
    size_t on_true = kNoLocal;      // jcc  cond  -> here
    size_t on_false = kNoLocal;     // jcc !cond  -> here
    std::string exit_label;         // jcc !cond  -> this real IR block
    bool active() const {
        return on_true != kNoLocal || on_false != kNoLocal || !exit_label.empty();
    }
};

// Recognises the loop shape the aggressive unroller handles:
//
//     H:  test; branch -> D, exit          (loop header)
//     D:  test; branch -> A, E             (the diamond)
//     A:  body; jump B
//     E:  body; jump B
//     B:  latch; jump H
//
// A and E must be the only predecessors of B, and D the only predecessor of A
// and E, so inlining all four into one straight-line body duplicates no work
// and skips none. Everything else is rejected.
struct DiamondUnroll {
    size_t header = 0;     // H
    size_t diamond = 0;    // D
    size_t arm_then = 0;   // A
    size_t arm_else = 0;   // E
    size_t latch = 0;      // B
    std::string exit_label;
};
inline bool match_diamond_unroll(const lithon::ir::Function& fn,
                                 const std::unordered_map<std::string, size_t>& block_index,
                                 size_t latch, DiamondUnroll& out) {
    using namespace lithon::ir;
    if (latch == 0 || latch >= fn.blocks.size()) return false;
    if (!is_unrollable_body(fn.blocks[latch])) return false;
    const Instr& back = fn.blocks[latch].instrs.back();
    if (back.op != Op::Jump) return false;
    auto hit = block_index.find(back.name);
    if (hit == block_index.end() || hit->second == 0 || hit->second >= latch) return false;
    const size_t header = hit->second;
    if (!is_rotatable_header(fn.blocks[header])) return false;
    auto ht = branch_targets(fn.blocks[header].instrs.back());
    if (ht.size() != 2 || ht[0] == ht[1]) return false;
    auto dit = block_index.find(ht[0]);
    if (dit == block_index.end() || dit->second >= latch) return false;
    const size_t diamond = dit->second;
    if (!is_rotatable_header(fn.blocks[diamond])) return false;
    auto dt = branch_targets(fn.blocks[diamond].instrs.back());
    if (dt.size() != 2 || dt[0] == dt[1]) return false;
    auto at = block_index.find(dt[0]), et = block_index.find(dt[1]);
    if (at == block_index.end() || et == block_index.end()) return false;
    const size_t arm_then = at->second, arm_else = et->second;
    if (arm_then >= latch || arm_else >= latch || arm_then == arm_else) return false;
    for (size_t arm : {arm_then, arm_else}) {
        if (!is_unrollable_body(fn.blocks[arm])) return false;
        if (fn.blocks[arm].instrs.back().name != fn.blocks[latch].label) return false;
    }
    // Sole-predecessor checks: the inlined body must be entered from exactly
    // the block we inline it after, or the arms would run on paths that never
    // tested the diamond.
    std::unordered_map<std::string, std::vector<size_t>> preds;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (const std::string& t : branch_targets(fn.blocks[b].instrs.back()))
            preds[t].push_back(b);
    auto only_pred = [&](const std::string& label, size_t want) {
        auto p = preds.find(label);
        return p != preds.end() && p->second.size() == 1 && p->second[0] == want;
    };
    if (!only_pred(fn.blocks[arm_then].label, diamond)) return false;
    if (!only_pred(fn.blocks[arm_else].label, diamond)) return false;
    if (!only_pred(fn.blocks[latch].label, latch)) {
        // B is reached from both arms, so it has two predecessors; what must
        // hold is that those are exactly A and E.
        auto p = preds.find(fn.blocks[latch].label);
        if (p == preds.end() || p->second.size() != 2) return false;
        if (std::find(p->second.begin(), p->second.end(), arm_then) == p->second.end()) return false;
        if (std::find(p->second.begin(), p->second.end(), arm_else) == p->second.end()) return false;
    }
    out.header = header;
    out.diamond = diamond;
    out.arm_then = arm_then;
    out.arm_else = arm_else;
    out.latch = latch;
    out.exit_label = ht[1];
    return true;
}

} // namespace detail

inline CompiledModule compile_module(const lithon::ir::Module& module,
                                     const CompileOptions& options = CompileOptions{}) {
    using namespace lithon::ir;
    using detail::TempInfo;

    CodeBuffer code;
    std::unordered_map<std::string, size_t> function_offset;

    struct PendingCallPatch {
        JumpPatch patch;
        std::string target_function;
    };
    std::vector<PendingCallPatch> pending_calls;

    // The double constant pool, shared by every function in the module
    // and appended once at the very end. Sharing matters: `0.5` in three
    // different functions is one 8-byte entry, and the dedup is by exact
    // bit pattern so 0.0 and -0.0 stay distinct as IEEE requires.
    std::vector<uint64_t> float_pool;
    std::unordered_map<uint64_t, size_t> float_pool_index_;
    // A reference to a pool entry whose disp32 cannot be resolved until
    // the pool's final position is known, since it is RIP-relative.
    struct PendingFloatPoolRef {
        size_t disp_offset;   // where the disp32 itself starts in `code`
        size_t pool_index;    // which entry of float_pool it wants
    };
    std::vector<PendingFloatPoolRef> pending_float_pool_refs;

    // One source of truth for "this value is a double": the same Kind
    // lattice that gates print(). Codegen and the gate therefore cannot
    // disagree about which values are floats, which is the property that
    // makes it safe to enable the float path from the guard alone.
    auto is_float_value = [](const std::vector<Kind>& kinds, lithon::ir::ValueId id) {
        return id < kinds.size() && kinds[id] == Kind::Float;
    };

    // Computed once, on the ORIGINAL (pre-optimization) module: see the
    // "print() FORMATTING" note above the class comment block for why
    // this stays valid after each function's private copy is folded/DCE'd.
    const std::vector<std::vector<Kind>> module_kinds = infer_value_kinds(module);

    for (size_t fn_index = 0; fn_index < module.functions.size(); ++fn_index) {
        const Function& original_fn = module.functions[fn_index];
        const std::vector<Kind>& value_kinds = module_kinds[fn_index];
        if (function_offset.count(original_fn.name)) {
            throw std::runtime_error(
                "compile_module: duplicate function name '" + original_fn.name + "'");
        }
        function_offset[original_fn.name] = code.size();

        if (original_fn.params.size() > 2) {
            throw std::runtime_error(
                "compile_module: function '" + original_fn.name + "' has more than 2 "
                "parameters -- only two register arguments are supported in this slice");
        }

        Function fn = original_fn;
        if (options.optimize) {
            OptimizePasses passes;
            passes.strength_reduce = options.strength_reduce;
            optimize_function(fn, passes);
        }

        PromotionMap promoted = options.promote_registers ? select_promoted_variables(fn)
                                                           : PromotionMap{};

        // A promoted variable lives in a general-purpose register, which
        // cannot hold a double. select_promoted_variables is float-blind --
        // it ranks purely by load/store count -- so any variable that ever
        // holds a float is dropped from the promotion set here, before
        // planning, so that:
        //   * plan_function never marks a load of it an Alias (that is what
        //     produced "float value aliases a general-purpose register"), and
        //   * assign_variable_slots gives it a frame slot, which is where the
        //     float load/store path above expects to find it.
        // This is the one place the float and integer register worlds meet,
        // and it is a subtraction, not an addition: the GP machinery is
        // untouched.
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            for (const auto& in : fn.blocks[b].instrs) {
                if (in.op == Op::Store && !in.args.empty() &&
                    is_float_value(value_kinds, in.args.at(0))) {
                    promoted.erase(in.name);
                } else if (in.op == Op::Load && is_float_value(value_kinds, in.result)) {
                    promoted.erase(in.name);
                }
            }
        }
        detail::FunctionPlan plan = detail::plan_function(fn, promoted);
        // Every value the guard proved is a double, in one list. Handing the
        // allocator that list -- rather than letting it re-derive kinds --
        // is what keeps allocation and codegen reading the same lattice.
        std::vector<ValueId> float_values;
        for (ValueId id = 0; id < value_kinds.size(); ++id) {
            if (value_kinds[id] == Kind::Float) float_values.push_back(id);
        }
        RegisterAllocator alloc(fn, promoted, plan.virtual_temps,
                                options.borrow_callee_saved, float_values);

        constexpr Reg kL = abi::kScratchLeft;
        constexpr Reg kR = abi::kScratchRight;

        auto info_of = [&](ValueId id) -> const TempInfo& {
            static const TempInfo normal{};
            auto it = plan.info.find(id);
            return it != plan.info.end() ? it->second : normal;
        };

        // dst := value of a temp, wherever it lives.
        auto materialize_into = [&](Reg dst, ValueId id) {
            const TempInfo& ti = info_of(id);
            switch (ti.kind) {
                case TempInfo::Kind::Const:
                    emit_mov_reg_imm(code, dst, ti.imm);
                    return;
                case TempInfo::Kind::Alias:
                    if (ti.alias != dst) emit_mov_reg_reg(code, dst, ti.alias);
                    return;
                case TempInfo::Kind::Normal: {
                    const ValueLocation& loc = alloc.temp_location(id);
                    if (loc.in_register) {
                        if (loc.reg != dst) emit_mov_reg_reg(code, dst, loc.reg);
                    } else {
                        emit_load_rbp_offset(code, dst, loc.stack_slot);
                    }
                    return;
                }
                default:
                    throw std::logic_error("compile_module: read of a fused temporary");
            }
        };

        // A register holding the value; uses `scratch` only if it must load or build it.
        auto read_in = [&](ValueId id, Reg scratch) -> Reg {
            const TempInfo& ti = info_of(id);
            if (ti.kind == TempInfo::Kind::Alias) return ti.alias;
            if (ti.kind == TempInfo::Kind::Normal) {
                const ValueLocation& loc = alloc.temp_location(id);
                if (loc.in_register) return loc.reg;
            }
            materialize_into(scratch, id);
            return scratch;
        };
        auto read_left = [&](ValueId id) { return read_in(id, kL); };
        auto read_right = [&](ValueId id) { return read_in(id, kR); };

        auto imm32_of = [&](ValueId id, int32_t& out) -> bool {
            const TempInfo& ti = info_of(id);
            if (ti.kind != TempInfo::Kind::Const || !fits_imm32(ti.imm)) return false;
            out = static_cast<int32_t>(ti.imm);
            return true;
        };

        auto compute_dest = [&](ValueId id) -> Reg {
            const ValueLocation& loc = alloc.temp_location(id);
            return loc.in_register ? loc.reg : kL;
        };
        auto commit_result = [&](ValueId id, Reg computed_in) {
            const ValueLocation& loc = alloc.temp_location(id);
            if (!loc.in_register) emit_store_rbp_offset(code, computed_in, loc.stack_slot);
        };

        auto require_variable = [&](const std::string& name, const char* what) {
            if (!alloc.has_variable(name)) {
                throw std::runtime_error(std::string("compile_module: ") + what +
                                         " undeclared variable '" + name + "'");
            }
        };

        // Emits a call to the host handler that reports a runtime error the
        // way the interpreter's exception does, so a JIT program and an
        // interpreted one report the same failure on stderr. Declared before
        // the float helpers because emit_float_arith uses it.
        //
        // The handler must not return: it exits the process, exactly as an
        // uncaught std::runtime_error would at the top of hello/tier_runner.
        // Returning would fall through into whatever instruction follows the
        // call and execute garbage.
        // Report a fatal error through the host and return. The message must
        // be spelled exactly as the interpreter spells it, prefix included:
        // the interpreter throws runtime_error("interpreter: division by
        // zero") and hello.cpp/tier_runner print "error: " + what(), so the
        // interpreter's stderr line is "error: interpreter: division by
        // zero". run_tier_diff.py compares stderr, so anything less than an
        // exact match here is a diff.
        auto emit_host_error_trap = [&](const char* message) {
            emit_mov_reg_imm(code, abi::kArgRegs[0], reinterpret_cast<int64_t>(message));
            emit_mov_reg_imm(code, kR, reinterpret_cast<int64_t>(&host_report_error));
            emit_xor_zero(code, Reg::RAX);
            if (abi::kShadowSpace) emit_sub_rsp_imm32(code, abi::kShadowSpace);
            emit_call_reg(code, kR);
            if (abi::kShadowSpace) emit_add_rsp_imm32(code, abi::kShadowSpace);
        };

        auto emit_float_zero_division_trap = [&]() {
            emit_host_error_trap("error: interpreter: division by zero\n");
        };

        // The integer counterpart. Distinct text on purpose: the interpreter
        // says "modulo by zero", and run_tier_diff.py matches stderr exactly,
        // so reusing the division string here would show up as a diff.
        auto emit_int_zero_modulo_trap = [&]() {
            emit_host_error_trap("error: interpreter: modulo by zero\n");
        };

        // Integer remainder, C semantics: the result takes the sign of the
        // dividend, so this is truncation toward zero, NOT Python's floored
        // `%`. Both engines implement the same rule, which is what makes the
        // tier diff meaningful.
        //
        // Two shapes:
        //
        //  * A constant divisor that is a positive power of two becomes
        //    `and` with (b-1). That equals `%` only for a non-negative
        //    dividend -- for a negative one the `and` yields the low bits
        //    with a cleared sign, so b has to be subtracted back out.
        //    Worked example, because getting this wrong is the whole bug:
        //    -7 % 4 is -3, and -7 & 3 is 1, so the fixup must be 1 - 4.
        //
        //    The sign test has to read the ORIGINAL dividend, not the masked
        //    result, and the ordering below is load-bearing. Masking clears the
        //    sign bit, so testing `dst` after the `and` sees a non-negative
        //    value and the fixup never runs: -6 % 8 masked to -6 & 7 is 2, and
        //    2 is positive, so the answer stays 2 instead of becoming -6. The
        //    sar therefore runs against `lhs` first, while `lhs` still holds
        //    the dividend.
        //
        //    And the subtraction is conditional on the masked result being
        //    NON-ZERO, because a negative dividend that is an exact multiple
        //    of b has remainder 0, not -b: -4 % 4 is 0, and an unconditional
        //    fixup turns -4 & 3 (which is 0) into 0 - 4 = -4.
        //
        //  * Anything else is the hardware instruction: cqo to sign-extend
        //    RAX into RDX, then idiv, which leaves the remainder in RDX.
        //    RAX and RDX are BOTH in the allocatable temp pool (abi::kTempPool),
        //    so clobbering them can destroy a live temporary. They are pushed
        //    and popped around the sequence, and the result is staged in kR
        //    before the pops, because `dst` may itself be RAX or RDX -- popping
        //    the saved value over the top of the result would be a silent
        //    wrong answer. The allocator has no notion of "this instruction
        //    needs RAX:RDX", so it cannot be relied on to keep them free.
        auto emit_int_modulo = [&](const Instr& in, Reg dst, Reg lhs, Reg rhs, bool rhs_is_imm,
                                   int32_t imm) {
            if (rhs_is_imm && imm > 0 && (imm & (imm - 1)) == 0) {
                if (imm == 1) {
                    // b == 1: the remainder is 0 whatever the dividend is.
                    if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                    emit_xor_zero(code, dst);
                    return;
                }
                // Sign of the ORIGINAL dividend, captured into kR before any
                // masking. sar shifts in place, so kR is loaded from lhs first.
                // kR is safe as scratch: it is permanent scratch and is never
                // allocated to a value, while `dst` is either a pool register
                // or kL, so the two can never collide.
                // The dividend is normalized into `dst` FIRST, and the sign is
                // derived from `dst` while it still holds the unmasked value.
                // Reading the sign out of `lhs` instead looks equivalent and is
                // not: if `lhs` happens to BE kR, the sar overwrites the
                // dividend before the copy to dst, and dst then receives the
                // already-shifted value. That only misfires for particular
                // register assignments, which is why it survived until a
                // fuzzer program with one fewer print landed on it.
                //
                // dst is never kR (a temp is either in kTempPool, which
                // excludes R10/R11, or spilled and therefore reported as kL),
                // so kR is always safe as the sign scratch here.
                if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                emit_mov_reg_reg(code, kR, dst);
                emit_sar_reg_imm8(code, kR, 63);
                // The mask is b-1, which only fits a sign-extended byte for
                // b <= 128. Above that it has to be the imm32 form: `and r,
                // 1023` encoded as 83 /4 ib sign-extends 0xFF to all-ones and
                // masks with nothing, so a large power-of-two divisor silently
                // returned the dividend unchanged.
                const int32_t mask = imm - 1;
                if (fits_imm8(mask)) emit_and_reg_imm8(code, dst, static_cast<int8_t>(mask));
                else emit_and_reg_imm32(code, dst, mask);
                // if (dst != 0 && kR != 0) dst -= b
                //
                // Both guards jump to the END of the sequence, so the fall
                // through is the "do the subtraction" case: the dividend was
                // negative AND the masked remainder is nonzero. Written the
                // other way round -- jumping to the end when the remainder is
                // NONZERO -- the whole fixup is skipped for every dividend that
                // actually needs it, which is the `-1 % 8 == 7` instead of -1
                // case.
                emit_test_reg_reg(code, dst);
                JumpPatch remainder_is_zero = emit_jcc_rel32(code, Cond::Equal);
                emit_test_reg_reg(code, kR);
                JumpPatch skip = emit_jcc_rel32(code, Cond::Equal);
                emit_sub_reg_imm32(code, dst, imm);
                resolve_jump_patch(code, skip, code.size());
                resolve_jump_patch(code, remainder_is_zero, code.size());
                return;
            }

            // idiv raises #DE on a zero divisor, which would kill the process
            // with a signal instead of the interpreter's clean error + exit(1).
            if (rhs_is_imm && imm == 0) {
                emit_int_zero_modulo_trap();
                return;
            }
            if (rhs_is_imm && imm == -1) {
                // a % -1 is 0 for every representable a, INCLUDING
                // INT64_MIN, whose quotient would be 2^63 and which is the one
                // input that makes idiv raise #DE. Testing for the divisor up
                // front is both the fix and a speedup: the whole division
                // collapses to a zero.
                if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                emit_xor_zero(code, dst);
                return;
            }
            if (!rhs_is_imm) {
                if (rhs == Reg::RAX || rhs == Reg::RDX) {
                    // The divisor is about to be overwritten by the sequence.
                    emit_mov_reg_reg(code, kR, rhs);
                    rhs = kR;
                }
                // Runtime divisor: two guards, because idiv faults on two
                // different inputs. A zero divisor, and a -1 divisor paired
                // with an INT64_MIN dividend (quotient 2^63, unrepresentable).
                // The -1 case folds to a zero for every other dividend, so it
                // doubles as the fast path.
                emit_test_reg_reg(code, rhs);
                JumpPatch divisor_ok = emit_jcc_rel32(code, Cond::NotEqual);
                emit_int_zero_modulo_trap();
                resolve_jump_patch(code, divisor_ok, code.size());

                emit_cmp_reg_imm32(code, rhs, -1);
                JumpPatch not_minus_one = emit_jcc_rel32(code, Cond::NotEqual);
                if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                emit_xor_zero(code, dst);
                JumpPatch past_zero = emit_jmp_rel32(code);
                resolve_jump_patch(code, not_minus_one, code.size());
                emit_push_reg(code, Reg::RAX);
                emit_push_reg(code, Reg::RDX);
                if (lhs != Reg::RAX) emit_mov_reg_reg(code, Reg::RAX, lhs);
                emit_cqo(code);
                emit_idiv_reg(code, rhs);
                // Stage the remainder somewhere that is neither RAX nor RDX
                // before unwinding the saved pair.
                if (kR != Reg::RDX) emit_mov_reg_reg(code, kR, Reg::RDX);
                emit_pop_reg(code, Reg::RDX);
                emit_pop_reg(code, Reg::RAX);
                if (dst != kR) emit_mov_reg_reg(code, dst, kR);
                resolve_jump_patch(code, past_zero, code.size());
                return;
            }

            emit_push_reg(code, Reg::RAX);
            emit_push_reg(code, Reg::RDX);
            if (lhs != Reg::RAX) emit_mov_reg_reg(code, Reg::RAX, lhs);
            emit_cqo(code);
            emit_mov_reg_imm(code, kR, imm);
            emit_idiv_reg(code, kR);
            if (kR != Reg::RDX) emit_mov_reg_reg(code, kR, Reg::RDX);
            emit_pop_reg(code, Reg::RDX);
            emit_pop_reg(code, Reg::RAX);
            if (dst != kR) emit_mov_reg_reg(code, dst, kR);
        };

        // ---- float (SSE2) helpers ---------------------------------------
        //
        // Every one of these works in XMM registers and never touches the GP
        // pools, so the integer machinery above is untouched by their
        // existence. The XMM scratch register is a single reserved register
        // (abi::kScratchFloat) rather than two like r10/r11, because each SSE
        // op here is strictly two-operand and its own destination: there is
        // never a moment where two unrelated XMM values must both be live in
        // scratch.

        // Where the result of a float op should be written, honouring the
        // FusedStore optimization: if the very next instruction stores this
        // value into a variable, write straight to the variable's slot and
        // skip the intermediate entirely.
        auto float_dest = [&](ValueId id) -> Xmm {
            if (info_of(id).kind == TempInfo::Kind::FusedStore) return abi::kScratchFloat;
            return alloc.float_in_register(id) ? alloc.float_register(id) : abi::kScratchFloat;
        };

        // Write a computed double back to where its value lives. A no-op in
        // every case except one: a Normal temp that the allocator decided to
        // spill. A Const temp is virtual (re-materialized as a pool load at
        // each use, so it has no home at all), a FusedStore/FusedCmp temp is
        // consumed by the very next instruction, and an in-register temp is
        // already in place. Calling emit_store here for any of those would
        // store a general-purpose register into a slot that was never
        // allocated -- slot 0, which is the return address.
        auto commit_float_result = [&](ValueId id, Xmm d) {
            if (info_of(id).kind != TempInfo::Kind::Normal) return;
            if (alloc.float_in_register(id)) return;
            if (!alloc.is_float(id)) return;   // virtual temp: no location to write
            emit_movsd_rbp_mem(code, d, alloc.float_stack_slot(id));
        };

        // A float VARIABLE lives in its own frame slot as raw double bits. A
        // promoted variable is a GP register, which cannot hold a double, so
        // float variables are never promoted -- select_promoted_variables is
        // float-blind, so this must be enforced here rather than assumed.
        auto float_var_offset = [&](const std::string& name) {
            if (!alloc.has_variable(name)) {
                throw std::runtime_error(
                    "compile_module: load/store of undeclared variable '" + name + "'");
            }
            return alloc.variable_offset(name);
        };

        auto load_float_var = [&](Xmm dst, const std::string& name) {
            // Encoded as a movsd from [rbp+disp32]; the GP emitters above use
            // a dedicated rbp-relative form, and this is its XMM counterpart.
            emit_movsd_xmm_rbp(code, dst, float_var_offset(name));
        };

        auto store_float_var = [&](Xmm src, const std::string& name) {
            emit_movsd_rbp_mem(code, src, float_var_offset(name));
        };



        // Emit `movsd dst, [rip+pool_entry]`, deferring the displacement
        // until the pool is placed. Records the disp32 offset for the
        // post-pass at the end of compile_module.
        auto emit_load_pool_double = [&](Xmm dst, uint64_t bits) {
            size_t index;
            auto it = float_pool_index_.find(bits);
            if (it != float_pool_index_.end()) {
                index = it->second;
            } else {
                index = float_pool.size();
                float_pool.push_back(bits);
                float_pool_index_[bits] = index;
            }
            emit_movsd_xmm_rip(code, dst);
            pending_float_pool_refs.push_back({movsd_rip_disp_offset(code), index});
        };

        // Materialize a float value into dst, whatever it currently lives in.
        auto load_float_value = [&](Xmm dst, ValueId id) {
            const TempInfo& ti = info_of(id);
            if (ti.kind == TempInfo::Kind::Const) {
                uint64_t bits;
                std::memcpy(&bits, &ti.dbl, sizeof(bits));
                emit_load_pool_double(dst, bits);
                return;
            }
            if (ti.kind == TempInfo::Kind::Alias) {
                // A float alias would name a promoted GP register, which
                // cannot hold a double. Aliasing is only ever assigned to
                // int/bool temps, so reaching here means the print guard and
                // the planner disagree -- refuse rather than reinterpret bits.
                throw std::runtime_error(
                    "compile_module: float value aliases a general-purpose register");
            }
            if (alloc.float_in_register(id)) {
                Xmm src = alloc.float_register(id);
                if (src != dst) emit_movsd_xmm_xmm(code, dst, src);
            } else {
                emit_movsd_xmm_rbp(code, dst, alloc.float_stack_slot(id));
            }
        };

        auto read_float = [&](ValueId id) -> Xmm {
            if (info_of(id).kind == TempInfo::Kind::Normal && alloc.float_in_register(id)) {
                return alloc.float_register(id);
            }
            load_float_value(abi::kScratchFloat, id);
            return abi::kScratchFloat;
        };

        // A ConstFloat is planned as a virtual temp (like ConstInt), so it is
        // re-materialized as a pool load at every use and never needs a
        // location of its own. This exists to make that explicit at the
        // definition site: emitting nothing here is correct, and anything else
        // would be a wasted register or slot for a value with no run-time
        // identity.
        auto load_float_const = [&](ValueId /*id*/, double /*d*/) {};

        // Coerce an int operand to double, because the guard's arith() rule
        // promotes int+float to Float. Without this, addsd would consume the
        // bit pattern of an integer and produce nonsense.
        auto coerce_to_float = [&](Xmm dst, ValueId id) {
            if (is_float_value(value_kinds, id)) { load_float_value(dst, id); return; }
            Reg gp = read_left(id);   // int source
            emit_cvtsi2sd(code, dst, gp);
        };

        // Emits Add/Sub/Mul/Div for a float result.
        //
        // Register discipline, which is the whole difficulty: kScratchFloat
        // and kScratchFloatB are reserved and never allocated, so there are
        // always two free XMM registers to stage operands in, and `dst` is
        // either a value's own pool register or kScratchFloat. The two
        // scratch registers are never a value's home, so nothing here can
        // destroy a value another instruction still needs.
        auto emit_float_arith = [&](const Instr& in) {
            const Xmm lhs_r = abi::kScratchFloat;
            const Xmm rhs_r = abi::kScratchFloatB;
            Xmm dst = float_dest(in.result);

            // Both operands are staged in scratch BEFORE dst is written, so
            // dst (which may be a value's own register) can never clobber an
            // operand mid-computation. Loads cannot disturb dst because dst is
            // not used until the last two instructions.
            coerce_to_float(lhs_r, in.args.at(0));
            coerce_to_float(rhs_r, in.args.at(1));

            if (in.op == Op::Div || in.op == Op::Mod) {
                // Python raises ZeroDivisionError for any zero divisor, so the
                // JIT checks and calls the same host handler the interpreter
                // ends up in. Mod reuses the whole mechanism with its own
                // message, because the interpreter says "modulo by zero" and
                // run_tier_diff.py compares stderr byte-for-byte.
                //
                // A NaN divisor must NOT trap: Python propagates nan
                // (1.0/nan is nan, not an error). That case cannot be handled
                // by branching on a single condition, because comisd sets ZF,
                // PF and CF all at once when the operands are unordered. ZF
                // alone cannot separate "equal" from "unordered" -- both set
                // it -- and Cond::NotEqual therefore does NOT skip the trap
                // for a NaN divisor, it takes it. (Branching on Parity would
                // disambiguate, but 0F 9A is a byte-for-byte collision between
                // `jp rel32` and `setp r/m8`; see emit_float_compare.)
                //
                // So the zero test is materialized in a GP register instead,
                // the same setcc-and-not-parity shape the comparison uses:
                //   kL = ZF (equal OR unordered)  AND  !PF (ordered)
                // which is 1 exactly when the divisor is an ordered zero.
                // -0.0 compares equal to 0.0, so it traps too, matching the
                // interpreter's `b == 0.0`.
                constexpr Reg kL = abi::kScratchLeft;
                constexpr Reg kR = abi::kScratchRight;
                emit_xorpd_zero(code, abi::kScratchFloatZero);
                emit_comisd(code, rhs_r, abi::kScratchFloatZero);
                emit_setcc(code, Cond::Equal, kL);
                emit_movzx_reg_reg8(code, kL, kL);
                emit_setcc(code, Cond::NotParity, kR);
                emit_movzx_reg_reg8(code, kR, kR);
                emit_and_reg_reg(code, kL, kR);
                emit_test_reg_reg(code, kL);
                // test sets ZF iff kL == 0, and Cond::Equal reads ZF, so this
                // jumps OVER the trap exactly when the divisor is not an
                // ordered zero. A NaN divisor leaves kL clear (PF forced
                // !PF to 0) and therefore propagates instead of trapping.
                JumpPatch skip_trap = emit_jcc_rel32(code, Cond::Equal);
                if (in.op == Op::Div) emit_float_zero_division_trap();
                else emit_int_zero_modulo_trap();
                resolve_jump_patch(code, skip_trap, code.size());
            }

            if (dst != lhs_r) emit_movsd_xmm_xmm(code, dst, lhs_r);
            if (in.op == Op::Add) emit_addsd(code, dst, rhs_r);
            else if (in.op == Op::Sub) emit_subsd(code, dst, rhs_r);
            else if (in.op == Op::Mul) emit_mulsd(code, dst, rhs_r);
            else if (in.op == Op::Div) {
                emit_divsd(code, dst, rhs_r);
            } else {
                // There is no SSE2 fmod: `fmod` is a libm call, and calling
                // one per modulo would be far more expensive than the
                // interpreter this is supposed to be matching. C defines fmod
                // as a - n*b for the integer n = trunc(a/b), and when n is 0 the
                // answer is just a, so the general form is:
                //   movsd    tmp, dst, a         copy the dividend (3rd XMM)
                //   divsd    tmp, tmp, rhs       a / b
                //   roundsd  tmp, tmp, to-zero   n = trunc(a/b)
                //   mulsd    tmp, tmp, rhs       n * b
                //   subsd    dst, dst, tmp       a - n*b
                //
                // The two operands are in scratch registers (lhs_r/rhs_r), and
                // dst may be either lhs_r or a pool register, so n is built in
                // kScratchFloatC and subtracted from the ORIGINAL dividend,
                // which is still live in dst. Clobbering dst with the quotient
                // first and multiplying it in place computes (a/b)*trunc(a/b) - b,
                // which is not fmod at all.
                constexpr Xmm kT = abi::kScratchFloatC;
                emit_movsd_xmm_xmm(code, kT, dst);
                emit_divsd(code, kT, rhs_r);
                emit_roundsd_imm8(code, kT, kT, kRoundTowardZeroSuppressInexact);

                // Skip the last two steps when n is an ordered zero, because
                // dst already holds the correct answer. n == 0 is exactly the
                // case |a| < |b|, where fmod(a, b) is a, and it is also the ONLY
                // case where the multiply can go wrong: IEEE makes 0 * inf a
                // NaN, but C's fmod(1.0, inf) is 1.0, since n*b is 0 for every
                // b when n is 0. So without this guard the sequence returns NaN
                // for a finite dividend and an infinite divisor.
                //
                // A NaN quotient is unordered, so comisd sets ZF for it as well;
                // the setcc-and-not-parity shape distinguishes a real zero from an
                // unordered one, and a NaN must fall through to the multiply,
                // which reproduces the NaN. (The other two infinite cases need no
                // special handling: fmod(inf, 1.0) is inf - inf = NaN, and
                // fmod(inf, inf) is inf - NaN = NaN.)
                //
                // kScratchFloatZero is live here: the zero-divisor test above
                // materialized it, and nothing since has overwritten it.
                emit_comisd(code, kT, abi::kScratchFloatZero);
                emit_setcc(code, Cond::Equal, abi::kScratchLeft);
                emit_movzx_reg_reg8(code, abi::kScratchLeft, abi::kScratchLeft);
                emit_setcc(code, Cond::NotParity, abi::kScratchRight);
                emit_movzx_reg_reg8(code, abi::kScratchRight, abi::kScratchRight);
                emit_and_reg_reg(code, abi::kScratchLeft, abi::kScratchRight);
                emit_test_reg_reg(code, abi::kScratchLeft);
                // test sets ZF iff kL == 0, i.e. iff n is NOT an ordered zero, so
                // Cond::NotEqual (ZF clear) is taken exactly when n IS an ordered
                // zero and dst already holds the right answer.
                //
                // The jump target must therefore land after the multiply and
                // subtract: those two are the steps the guard exists to skip,
                // since 0 * inf is the NaN this branch exists to avoid. Landing
                // on the addsd instead would still run the multiply.
                //
                // The addsd sits after the subsd so that both paths reach it.
                // dst is left as the raw dividend, so an exact division of a
                // negative dividend would otherwise yield -0.0 where the
                // subsd path yields +0.0. IEEE defines x - x as +0.0, and the
                // interpreter normalizes fmod's zero result to +0.0 to match,
                // so the skip path has to add the zero register to force the
                // same normalization: -0.0 + 0.0 is +0.0, and adding 0.0 to any
                // other value is a no-op.
                JumpPatch ordered_zero = emit_jcc_rel32(code, Cond::NotEqual);
                emit_mulsd(code, kT, rhs_r);
                emit_subsd(code, dst, kT);
                resolve_jump_patch(code, ordered_zero, code.size());
                emit_addsd(code, dst, abi::kScratchFloatZero);
            }
            commit_float_result(in.result, dst);
        };

        // Lt/Gt/Eq with at least one double operand. The result is a Bool,
        // so it lives in a general-purpose register; only the comparison
        // itself is floating point.
        //
        // Flag semantics: ucomisd sets CF like an unsigned compare, so
        //   lhs <  rhs  ->  CF set                (Cond::Below)
        //   lhs >  rhs  ->  CF clear and ZF clear (Cond::Above)
        //   lhs == rhs  ->  ZF set, and NOT unordered
        // An unordered compare (either operand NaN) sets ZF, PF and CF all
        // at once, which is deliberately ambiguous: it makes the "less
        // than" reading true, the "equal" reading true, and the "greater
        // than" reading false. Every ordered operator therefore has to
        // exclude the unordered case, or NaN would compare as less than
        // everything. Python agrees: 1.0 < nan, 1.0 > nan and 1.0 == nan
        // are all False.
        //
        // That exclusion is done with setcc + and rather than a parity
        // branch. Branching on Parity would be the obvious encoding, but
        // 0F 9A is a byte-for-byte collision between `jp rel32` and
        // `setp r/m8` -- the CPU picks between them by looking at the
        // ModRM byte that follows, and a rel32 displacement that looks
        // like a non-register ModRM (mod=00, rm=101 gives 0x0d, which a
        // small forward displacement very often does) makes the CPU
        // execute a store to a wild address instead of a branch. So:
        //   dst = <the flag test>  AND  <not unordered>
        // Two setcc, one and, no branches, and the disassembly is exact.
        auto emit_float_compare = [&](const Instr& in) {
            const Xmm lhs_r = abi::kScratchFloat;
            const Xmm rhs_r = abi::kScratchFloatB;
            coerce_to_float(lhs_r, in.args.at(0));
            coerce_to_float(rhs_r, in.args.at(1));
            Reg dst = compute_dest(in.result);
            // The operands are already in XMM registers, so kL is free as a
            // bit to compute the "not unordered" flag in.
            constexpr Reg kL = abi::kScratchLeft;

            if (in.op == Op::Gt) {
                // ucomisd dst, src reads flags for src < dst, so comparing
                // with the operands swapped turns Cond::Below into lhs > rhs.
                emit_ucomisd(code, rhs_r, lhs_r);
            } else {
                emit_ucomisd(code, lhs_r, rhs_r);
            }

            // Eq tests ZF; Lt and Gt both test CF, since Gt swapped its
            // operands above.
            emit_setcc(code, in.op == Op::Eq ? Cond::Equal : Cond::Below, dst);
            emit_movzx_reg_reg8(code, dst, dst);
            // setnp (not parity) is 1 exactly when the compare was ordered,
            // which is what keeps every NaN comparison False.
            emit_setcc(code, Cond::NotParity, kL);
            emit_movzx_reg_reg8(code, kL, kL);
            emit_and_reg_reg(code, dst, kL);
            commit_result(in.result, dst);
        };

        // ---- prologue ---------------------------------------------------
        emit_prologue(code, alloc.frame_size());
        for (const auto& saved : alloc.callee_saved_slots()) {
            emit_store_rbp_offset(code, saved.first, saved.second);
        }
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (alloc.variable_in_register(fn.params[i])) {
                emit_mov_reg_reg(code, alloc.variable_reg(fn.params[i]), abi::kArgRegs[i]);
            } else {
                emit_store_rbp_offset(code, abi::kArgRegs[i], alloc.variable_offset(fn.params[i]));
            }
        }

        struct PendingBlockPatch {
            JumpPatch patch;
            std::string target_label;
        };
        std::vector<PendingBlockPatch> pending_blocks;
        std::unordered_map<std::string, size_t> block_offset;
        std::unordered_map<std::string, size_t> block_index;
        for (size_t b = 0; b < fn.blocks.size(); ++b) block_index[fn.blocks[b].label] = b;

        auto emit_branch_to = [&](Cond cond, const std::string& then_label,
                                  const std::string& else_label, const std::string& next_label) {
            if (then_label == next_label) {
                pending_blocks.push_back({emit_jcc_rel32(code, invert(cond)), else_label});
            } else if (else_label == next_label) {
                pending_blocks.push_back({emit_jcc_rel32(code, cond), then_label});
            } else {
                pending_blocks.push_back({emit_jcc_rel32(code, cond), then_label});
                pending_blocks.push_back({emit_jmp_rel32(code), else_label});
            }
        };

        // Local join points for the aggressive unroller: a target that is a
        // position in the emitted stream rather than an IR label, bound once
        // the stream has been extended past it.
        struct PendingLocalPatch {
            JumpPatch patch;
            size_t local_id;
        };
        std::vector<PendingLocalPatch> pending_locals;
        std::unordered_map<size_t, size_t> local_offsets;
        size_t next_local = 0;
        auto new_local = [&]() { return next_local++; };
        auto bind_local = [&](size_t id) { local_offsets[id] = code.size(); };

        // Emits block `bi`'s instructions. `next_label` is the label of the
        // block that will physically follow the emitted code (used to elide
        // jumps to the fall-through block). `then_local`/`else_local` are
        // local join points used instead of the block's real branch targets.
        std::function<void(size_t, const std::string&, unsigned, detail::LocalBranch)> emit_block_body_fn =
            [&](size_t bi, const std::string& next_label, unsigned flags, detail::LocalBranch lb) {
            const BasicBlock& block = fn.blocks[bi];
            const bool allow_rotate = (flags & detail::kAllowRotate) != 0;
            for (size_t pos = 0; pos < block.instrs.size(); ++pos) {
                if ((flags & detail::kStopBeforeTerminator) && pos + 1 == block.instrs.size()) break;
                if (plan.skip_instr[bi][pos]) continue;
                const Instr& instr = block.instrs[pos];

                switch (instr.op) {
            case Op::ConstInt:
            case Op::ConstBool:
                break;   // always an immediate at its uses

            case Op::ConstFloat:
                // A double is not an immediate operand in x86-64; it lives in
                // the constant pool appended after the code and is loaded with
                // one movsd at each use. This is the SSE analogue of the
                // "always an immediate at its uses" above, and it is why
                // ConstFloat must be plannable as a Const temp: otherwise a
                // load would have to be re-materialized at every use, which is
                // what the immediate path avoids for ints.
                if (info_of(instr.result).kind == TempInfo::Kind::Normal) {
                    load_float_const(instr.result, instr.float_imm);
                }
                break;

            case Op::Load: {
                if (info_of(instr.result).kind == TempInfo::Kind::Alias) break;
                require_variable(instr.name, "load of");
                if (is_float_value(value_kinds, instr.result)) {
                    Xmm dst = float_dest(instr.result);
                    load_float_var(dst, instr.name);
                    commit_float_result(instr.result, dst);
                    break;
                }
                Reg dst = compute_dest(instr.result);
                if (alloc.variable_in_register(instr.name)) {
                    emit_mov_reg_reg(code, dst, alloc.variable_reg(instr.name));
                } else {
                    emit_load_rbp_offset(code, dst, alloc.variable_offset(instr.name));
                }
                commit_result(instr.result, dst);
                break;
            }

            case Op::Store: {
                require_variable(instr.name, "store to");
                if (is_float_value(value_kinds, instr.args.at(0))) {
                    Xmm src = read_float(instr.args.at(0));
                    store_float_var(src, instr.name);
                    break;
                }
                if (alloc.variable_in_register(instr.name)) {
                    materialize_into(alloc.variable_reg(instr.name), instr.args.at(0));
                } else {
                    Reg src = read_left(instr.args.at(0));
                    emit_store_rbp_offset(code, src, alloc.variable_offset(instr.name));
                }
                break;
            }

            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod: {
                // The float path is a separate case body rather than extra
                // branches inside the integer one: an int temp and a double
                // temp are different storage, so mixing them in one block
                // would mean every register name in it is conditionally a GP
                // register or an XMM one.
                if (is_float_value(value_kinds, instr.result)) { emit_float_arith(instr); break; }
                        const bool fused = info_of(instr.result).kind == TempInfo::Kind::FusedStore;
                        Reg dst = fused ? alloc.variable_reg(block.instrs[pos + 1].name)
                                        : compute_dest(instr.result);
                        Reg lhs = read_left(instr.args.at(0));
                        int32_t imm = 0;
                        if (instr.op == Op::Mod) {
                            const bool rhs_is_imm = imm32_of(instr.args.at(1), imm);
                            const Reg rhs = rhs_is_imm ? kR : read_right(instr.args.at(1));
                            emit_int_modulo(instr, dst, lhs, rhs, rhs_is_imm, imm);
                            if (!fused) commit_result(instr.result, dst);
                            break;
                        }
                        if (imm32_of(instr.args.at(1), imm)) {
                            if (instr.op == Op::Mul) {
                                emit_imul_reg_reg_imm32(code, dst, lhs, imm);
                            } else {
                                if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                                if (instr.op == Op::Add) emit_add_reg_imm32(code, dst, imm);
                                else emit_sub_reg_imm32(code, dst, imm);
                            }
                        } else {
                            Reg rhs = read_right(instr.args.at(1));
                            if (rhs == dst && lhs != dst) {
                                // dst is about to be overwritten but is also the right operand.
                                if (instr.op == Op::Sub) {
                                    emit_mov_reg_reg(code, kR, lhs);
                                    emit_sub_reg_reg(code, kR, rhs);
                                    emit_mov_reg_reg(code, dst, kR);
                                } else if (instr.op == Op::Add) {
                                    emit_add_reg_reg(code, dst, lhs);
                                } else {
                                    emit_imul_reg_reg(code, dst, lhs);
                                }
                            } else {
                                if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                                if (instr.op == Op::Add) emit_add_reg_reg(code, dst, rhs);
                                else if (instr.op == Op::Sub) emit_sub_reg_reg(code, dst, rhs);
                                else emit_imul_reg_reg(code, dst, rhs);
                            }
                        }
                        if (!fused) commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Lt:
                    case Op::Gt:
                    case Op::Eq: {
                        if (info_of(instr.result).kind == TempInfo::Kind::FusedCmp) break;
                        // A float operand means the whole comparison is a
                        // double comparison -- the guard promotes int+float
                        // -- so it cannot go through the integer cmp/setcc
                        // path, which would read the bit pattern.
                        if (is_float_value(value_kinds, instr.args.at(0)) ||
                            is_float_value(value_kinds, instr.args.at(1))) {
                            emit_float_compare(instr);
                            break;
                        }
                        Reg lhs = read_left(instr.args.at(0));
                        Reg dst = compute_dest(instr.result);
                        int32_t imm = 0;
                        if (imm32_of(instr.args.at(1), imm)) {
                            emit_cmp_reg_imm32(code, lhs, imm);
                        } else {
                            emit_cmp_reg_reg(code, lhs, read_right(instr.args.at(1)));
                        }
                        Cond cond = instr.op == Op::Lt ? Cond::Less
                                  : instr.op == Op::Gt ? Cond::Greater : Cond::Equal;
                        emit_setcc(code, cond, dst);
                        emit_movzx_reg_reg8(code, dst, dst);
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::And: {
                        Reg lhs = read_left(instr.args.at(0));
                        Reg rhs = read_right(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        emit_test_reg_reg(code, lhs);
                        JumpPatch to_use_rhs = emit_jcc_rel32(code, Cond::NotZero);
                        emit_mov_reg_reg(code, dst, lhs);
                        JumpPatch to_end = emit_jmp_rel32(code);
                        resolve_jump_patch(code, to_use_rhs, code.size());
                        emit_mov_reg_reg(code, dst, rhs);
                        resolve_jump_patch(code, to_end, code.size());
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Or: {
                        Reg lhs = read_left(instr.args.at(0));
                        Reg rhs = read_right(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        emit_test_reg_reg(code, lhs);
                        JumpPatch to_use_lhs = emit_jcc_rel32(code, Cond::NotZero);
                        emit_mov_reg_reg(code, dst, rhs);
                        JumpPatch to_end = emit_jmp_rel32(code);
                        resolve_jump_patch(code, to_use_lhs, code.size());
                        emit_mov_reg_reg(code, dst, lhs);
                        resolve_jump_patch(code, to_end, code.size());
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Not: {
                        Reg operand = read_left(instr.args.at(0));
                        Reg dst = compute_dest(instr.result);
                        emit_test_reg_reg(code, operand);
                        emit_setcc(code, Cond::Equal, dst);
                        emit_movzx_reg_reg8(code, dst, dst);
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Branch: {
                        auto targets = branch_targets(instr);
                        if (targets.size() != 2) throw std::runtime_error("compile_module: malformed branch");
                        ValueId cond_id = instr.args.at(0);
                        Cond cond = Cond::NotZero;
                        if (info_of(cond_id).kind == TempInfo::Kind::FusedCmp) {
                            // The compare producing this condition is the previous instruction.
                            const Instr& cmp = block.instrs[pos - 1];
                            Reg lhs = read_left(cmp.args.at(0));
                            int32_t imm = 0;
                            if (imm32_of(cmp.args.at(1), imm)) emit_cmp_reg_imm32(code, lhs, imm);
                            else emit_cmp_reg_reg(code, lhs, read_right(cmp.args.at(1)));
                            cond = cmp.op == Op::Lt ? Cond::Less
                                 : cmp.op == Op::Gt ? Cond::Greater : Cond::Equal;
                        } else {
                            emit_test_reg_reg(code, read_left(cond_id));
                        }
                        if (lb.active()) {
                            // This block was inlined, so its targets are
                            // positions in this straight-line body rather than
                            // IR blocks. Whichever arm is left unset simply
                            // falls through to the next thing emitted.
                            if (lb.on_false != detail::kNoLocal)
                                pending_locals.push_back({emit_jcc_rel32(code, invert(cond)), lb.on_false});
                            if (lb.on_true != detail::kNoLocal)
                                pending_locals.push_back({emit_jcc_rel32(code, cond), lb.on_true});
                            if (!lb.exit_label.empty() && lb.exit_label != next_label)
                                pending_blocks.push_back({emit_jcc_rel32(code, invert(cond)), lb.exit_label});
                        } else if (flags & detail::kExitOnlyBranch) {
                            pending_blocks.push_back({emit_jcc_rel32(code, invert(cond)), targets[1]});
                        } else {
                            emit_branch_to(cond, targets[0], targets[1], next_label);
                        }
                        break;
                    }

                    case Op::Jump: {
                        if (instr.name == next_label) break;   // fall through
                        auto it = block_index.find(instr.name);
                        if (allow_rotate && options.rotate_loops && it != block_index.end() &&
                            it->second <= bi && detail::is_rotatable_header(fn.blocks[it->second])) {
                            // Loop rotation: re-test the loop condition here instead of
                            // jumping back to the header, so each iteration executes one
                            // conditional jump instead of a conditional plus an unconditional.
                            emit_block_body_fn(it->second, next_label, 0, detail::LocalBranch{});
                        } else {
                            pending_blocks.push_back({emit_jmp_rel32(code), instr.name});
                        }
                        break;
                    }

                    case Op::Call: {
                        if (instr.name == "print") {
                            if (instr.args.size() != 1) {
                                throw std::runtime_error(
                                    "compile_module: print() with " +
                                    std::to_string(instr.args.size()) +
                                    " arguments (native supports exactly 1)");
                            }
                            ValueId arg = instr.args.at(0);
                            Kind k = arg < value_kinds.size() ? value_kinds[arg] : Kind::Unknown;

                            if (k == Kind::Float) {
                                // Two calls, because printf has no format that
                                // reproduces CPython's shortest-roundtrip
                                // rendering. First format the double in the host
                                // (see float_runtime.h), then print the
                                // resulting string with "%s".
                                //
                                // The double is a variadic float argument, so it
                                // goes in the first XMM register rather than a GP
                                // one, and AL must be nonzero: SysV reads it as
                                // the count of vector registers used, and the
                                // xor_zero on the int/bool path sets 0, which
                                // would make the callee skip XMM0 outright.
                                Xmm fv = read_float(arg);
                                if (fv != abi::kFloatArgReg) {
                                    emit_movsd_xmm_xmm(code, abi::kFloatArgReg, fv);
                                }
                                emit_mov_reg_imm(code, kR,
                                                 reinterpret_cast<int64_t>(&host_format_double));
                                emit_xor_zero(code, Reg::RAX);
                                if (abi::kShadowSpace) emit_sub_rsp_imm32(code, abi::kShadowSpace);
                                emit_call_reg(code, kR);
                                if (abi::kShadowSpace) emit_add_rsp_imm32(code, abi::kShadowSpace);
                                // Returns a char* in RAX; hand it to printf as
                                // the second argument.
                                emit_mov_reg_reg(code, abi::kArgRegs[1], Reg::RAX);
                                emit_mov_reg_imm(code, abi::kArgRegs[0],
                                                 reinterpret_cast<int64_t>(kFloatPrintFormat));
                            } else if (k == Kind::Bool) {
                                // Two literal strings, no format specifiers: select
                                // which one is printf's sole argument by branching,
                                // rather than formatting a "%s" indirection.
                                Reg v = read_left(arg);
                                emit_test_reg_reg(code, v);
                                JumpPatch to_false = emit_jcc_rel32(code, Cond::Equal);
                                emit_mov_reg_imm(code, abi::kArgRegs[0],
                                    reinterpret_cast<int64_t>(kBoolTrueLiteral));
                                JumpPatch to_call = emit_jmp_rel32(code);
                                resolve_jump_patch(code, to_false, code.size());
                                emit_mov_reg_imm(code, abi::kArgRegs[0],
                                    reinterpret_cast<int64_t>(kBoolFalseLiteral));
                                resolve_jump_patch(code, to_call, code.size());
                            } else if (k == Kind::Int) {
                                materialize_into(abi::kArgRegs[1], arg);
                                emit_mov_reg_imm(code, abi::kArgRegs[0],
                                    reinterpret_cast<int64_t>(kIntPrintFormat));
                            } else {
                                // Unresolved or mixed kind -- not float, which is
                                // handled above. There is no format to fall back to
                                // and no way to tell whether the value is even a
                                // number. A caller with the print_guard in front of
                                // it (tier_runner) never reaches this; a caller
                                // without one (lithon_jit) gets a clear refusal
                                // instead of a silently-wrong integer
                                // reinterpretation of a double.
                                throw std::runtime_error(
                                    "compile_module: print() argument is not provably "
                                    "int, bool or float (kind: " + std::string(kind_name(k)) +
                                    "); native cannot choose a format for it");
                            }
                            emit_mov_reg_imm(code, kR, reinterpret_cast<int64_t>(&std::printf));
                            // The float branch above already made its own call and
                            // left the string pointer in the second argument
                            // register, so AL is 0 here for every kind: printf's
                            // only argument is now a pointer, never a double.
                            emit_xor_zero(code, Reg::RAX);
                            if (abi::kShadowSpace) emit_sub_rsp_imm32(code, abi::kShadowSpace);
                            emit_call_reg(code, kR);
                            if (abi::kShadowSpace) emit_add_rsp_imm32(code, abi::kShadowSpace);
                            break;
                        }
                        if (instr.args.size() > 2) {
                            throw std::runtime_error(
                                "compile_module: calls with more than 2 arguments "
                                "not supported in this slice");
                        }
                        for (size_t i = 0; i < instr.args.size(); ++i) {
                            materialize_into(abi::kArgRegs[i], instr.args[i]);
                        }
                        JumpPatch to_callee = emit_jmp_rel32(code);
                        code[to_callee.rel32_offset - 1] = 0xE8;   // rewrite jmp rel32 -> call rel32
                        pending_calls.push_back({to_callee, instr.name});

                        if (instr.result != kInvalidValue) {
                            // A spilled call result: the callee left its
                            // return value in RAX and nothing has written RAX
                            // since, so store it into the slot directly.
                            // compute_dest() would hand back kL instead and
                            // cost an extra register-to-register copy through
                            // scratch for no reason.
                            const ValueLocation& loc = alloc.temp_location(instr.result);
                            if (loc.in_register) {
                                if (loc.reg != Reg::RAX) emit_mov_reg_reg(code, loc.reg, Reg::RAX);
                            } else {
                                emit_store_rbp_offset(code, Reg::RAX, loc.stack_slot);
                            }
                        }
                        break;
                    }

                    case Op::Return: {
                        if (!instr.args.empty()) materialize_into(Reg::RAX, instr.args.at(0));
                        for (const auto& saved : alloc.callee_saved_slots()) {
                            emit_load_rbp_offset(code, saved.first, saved.second);
                        }
                        emit_epilogue(code);
                        emit_ret(code);
                        break;
                    }

                    default:
                        throw std::runtime_error(
                            "compile_module: opcode not implemented in this slice "
                            "(floats are the main remaining gap)");
                }
            }
        };

        auto emit_block_body = [&](size_t bi, const std::string& next_label, unsigned flags,
                                   detail::LocalBranch lb = detail::LocalBranch{}) {
            emit_block_body_fn(bi, next_label, flags, lb);
        };

        // Aggressive diamond unroll, planned before anything is emitted.
        //
        // The whole loop is emitted as one rotated unit at its header:
        //
        //   rotate: <H test>              jge exit
        //           <copy k: D, A/E, B>   <H test>   jge exit
        //           ...                                    ...
        //           <copy k+U-1: D, A/E, B><H test>   jl rotate / jge exit
        //
        // D, both arms and B are then skipped by the emission loop, because
        // after this inlining they have no predecessor outside the copies and
        // no successor that is not a label we own. Nothing else in the
        // function may reach them -- match_diamond_unroll() proved that by
        // checking sole predecessors -- so skipping them cannot strand a
        // jump. The entry copy is preceded by its own test, so a zero-trip
        // loop still does no work, and every iteration is still tested
        // individually, so any trip count stays exact.
        std::unordered_map<size_t, detail::DiamondUnroll> diamond_at_header;
        std::unordered_set<size_t> inlined_blocks;
        if (options.rotate_loops && options.unroll_factor > 1 && options.unroll_diamonds) {
            for (size_t latch = 1; latch < fn.blocks.size(); ++latch) {
                detail::DiamondUnroll dia;
                if (!detail::match_diamond_unroll(fn, block_index, latch, dia)) continue;
                if (diamond_at_header.count(dia.header)) continue;   // first match wins
                diamond_at_header.emplace(dia.header, dia);
                inlined_blocks.insert(dia.diamond);
                inlined_blocks.insert(dia.arm_then);
                inlined_blocks.insert(dia.arm_else);
                inlined_blocks.insert(latch);
            }
        }

        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            block_offset[fn.blocks[bi].label] = code.size();
            const std::string next_label = bi + 1 < fn.blocks.size() ? fn.blocks[bi + 1].label : "";

            if (inlined_blocks.count(bi)) continue;   // already inside an unrolled copy

            auto dia_it = diamond_at_header.find(bi);
            if (dia_it != diamond_at_header.end()) {
                const detail::DiamondUnroll& dia = dia_it->second;
                const size_t rotate_id = new_local();
                bind_local(rotate_id);
                // Entry test: what the header would have done on the way in.
                {
                    detail::LocalBranch lb;
                    lb.exit_label = dia.exit_label;
                    emit_block_body(dia.header, next_label, 0u, lb);
                }
                for (int k = 0; k < options.unroll_factor; ++k) {
                    // Per-copy join points, so each copy's "skip the else arm"
                    // jump lands on that copy's own latch.
                    const size_t else_id = new_local(), join_id = new_local();
                    // D: the compare it fuses is emitted by its Branch case, so
                    // D is emitted whole. On true the then arm is the
                    // fall-through, which leaves one conditional jump and no
                    // unconditional one on the common path.
                    detail::LocalBranch dlb;
                    dlb.on_false = else_id;
                    emit_block_body(dia.diamond, "", 0u, dlb);
                    emit_block_body(dia.arm_then, "", detail::kStopBeforeTerminator);
                    pending_locals.push_back({emit_jmp_rel32(code), join_id});
                    bind_local(else_id);
                    emit_block_body(dia.arm_else, "", detail::kStopBeforeTerminator);
                    bind_local(join_id);
                    emit_block_body(dia.latch, "", detail::kStopBeforeTerminator);
                    // H: duplicate the test. All but the last exit straight to
                    // the loop exit and fall into the next copy; the last
                    // jumps back to rotate_id, closing the loop.
                    const bool last = (k + 1 == options.unroll_factor);
                    detail::LocalBranch hlb;
                    hlb.exit_label = dia.exit_label;
                    if (last) hlb.on_true = rotate_id;
                    emit_block_body(dia.header, next_label, 0u, hlb);
                }
                continue;
            }

            // Unrolling: header H immediately precedes body B, H's branch
            // enters B or leaves the loop, and B is simple straight-line
            // code ending in `jump H`. Emit U copies of B, each followed
            // by an inlined copy of H's test that EXITS when false and
            // falls into the next copy when true; only the last copy's
            // test jumps back. Every iteration is still individually
            // tested, so any trip count (not just multiples of U) is exact.
            if (options.rotate_loops && options.unroll_factor > 1 && bi >= 1 &&
                detail::is_unrollable_body(fn.blocks[bi])) {
                const Instr& term = fn.blocks[bi].instrs.back();
                auto hit = block_index.find(term.name);
                if (hit != block_index.end() && hit->second == bi - 1 &&
                    detail::is_rotatable_header(fn.blocks[hit->second])) {
                    auto targets = branch_targets(fn.blocks[hit->second].instrs.back());
                    if (targets.size() == 2 && targets[0] == fn.blocks[bi].label &&
                        targets[1] != fn.blocks[bi].label) {
                        for (int k = 0; k < options.unroll_factor; ++k) {
                            emit_block_body(bi, "", detail::kStopBeforeTerminator);
                            const bool last = (k + 1 == options.unroll_factor);
                            emit_block_body(hit->second, next_label,
                                            last ? 0u : detail::kExitOnlyBranch);
                        }
                        continue;
                    }
                }
            }

            emit_block_body(bi, next_label, detail::kAllowRotate);
        }

        for (const auto& p : pending_locals) {
            auto it = local_offsets.find(p.local_id);
            if (it == local_offsets.end()) {
                throw std::runtime_error("compile_module: unbound local join point");
            }
            resolve_jump_patch(code, p.patch, it->second);
        }

        for (const auto& p : pending_blocks) {
            auto it = block_offset.find(p.target_label);
            if (it == block_offset.end()) {
                throw std::runtime_error(
                    "compile_module: jump to unknown block '" + p.target_label + "'");
            }
            resolve_jump_patch(code, p.patch, it->second);
        }
    }

    for (const auto& p : pending_calls) {
        auto it = function_offset.find(p.target_function);
        if (it == function_offset.end()) {
            throw std::runtime_error(
                "compile_module: call to unknown function '" + p.target_function + "'");
        }
        resolve_jump_patch(code, p.patch, it->second);
    }

    // Append the double constant pool to the code and resolve every
    // pending RIP-relative reference to it. Each pending site recorded
    // the offset of its own disp32 and the pool index it wants; now that
    // the pool's base offset in the buffer is known, every one of them
    // becomes a concrete rel32 and the code is position-independent
    // within the mapping.
    for (const auto& site : pending_float_pool_refs) {
        const size_t pool_base = code.size();
        // RIP-relative displacement is measured from the END of the
        // instruction, which is exactly where patch_u32_at expects to
        // write: site.disp_offset is the disp32's own position, and the
        // four bytes after it are the end of the movsd.
        const int64_t rel = static_cast<int64_t>(pool_base + site.pool_index * 8) -
                            static_cast<int64_t>(site.disp_offset + 4);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            throw std::runtime_error(
                "compile_module: float constant pool is out of reach");
        }
        patch_u32_at(code, site.disp_offset, static_cast<uint32_t>(rel));
    }
    for (uint64_t bits : float_pool) {
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
        }
    }

    return CompiledModule{std::move(code), std::move(function_offset), std::move(float_pool)};
}

} // namespace lithon::jit
