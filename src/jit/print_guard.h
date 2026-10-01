#pragma once

// Native-tier eligibility guard for print().
//
// The JIT's print() always formats its argument as a signed 64-bit
// integer. The interpreter prints True/False for bools and a decimal
// for floats. If a program prints anything that is not *provably* an
// int, native output can silently differ from the interpreter.
//
// This header answers one question, conservatively:
//
//     "Is every print() argument in this module provably an int or a bool?"
//
// (The JIT prints a provably-bool value as True/False and a provably-int
// value as a decimal; see compile_function.h.) If yes, native compilation
// is output-safe. If no, the caller must
// use the Tier-0 interpreter. "Provably" means the abstract kind of the
// value is exactly Int after a whole-module fixpoint. Anything unseen,
// mixed, or unknown counts as unsafe.
//
// Kinds form a flat lattice:
//
//   Unknown  (top: could be anything)
//      ^
//      |  Int, Bool and Float are incomparable siblings
//      |
//   Unseen   (bottom: no information yet)
//
// The analysis is whole-module and flow-insensitive:
//   * a variable's kind is the join of everything stored to it
//   * an untyped parameter's kind is the join of the arguments at
//     every call site (main is the only external entry point)
//   * a function's return kind is the join of its returned values
// Instructions after the first terminator of a block are dead and are
// ignored (the frontend emits a trailing `return` after `return x`).

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "ir/ir.h"

namespace lithon::jit {

enum class Kind : uint8_t { Unseen, Int, Bool, Float, Unknown };

inline Kind join(Kind a, Kind b) {
    if (a == b) return a;
    if (a == Kind::Unseen) return b;
    if (b == Kind::Unseen) return a;
    return Kind::Unknown;
}

inline const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Int:     return "int";
        case Kind::Bool:    return "bool";
        case Kind::Float:   return "float";
        case Kind::Unknown: return "not provably int";
        case Kind::Unseen:  return "unresolved";
    }
    return "?";
}

struct GuardVerdict {
    bool native_safe = true;
    std::vector<std::string> reasons;   // one entry per offending print()
};

namespace detail {

inline Kind declared_kind(const std::string& type_kind) {
    if (type_kind.empty()) return Kind::Unseen;   // "no declaration"
    if (type_kind == "int")   return Kind::Int;
    if (type_kind == "bool")  return Kind::Bool;
    if (type_kind == "float") return Kind::Float;
    return Kind::Unknown;                          // str, anything else
}

inline bool is_terminator(lithon::ir::Op op) {
    using lithon::ir::Op;
    return op == Op::Return || op == Op::Jump || op == Op::Branch;
}

struct FnState {
    const lithon::ir::Function* fn = nullptr;
    std::vector<Kind> vals;                          // by ValueId
    std::unordered_map<std::string, Kind> vars;      // variables and params
    std::vector<Kind> param_kind;                    // inferred from call sites
    Kind ret = Kind::Unseen;
    bool called = false;
};

struct Analysis {
    std::vector<FnState> fns;
    std::unordered_map<std::string, size_t> index;
    bool changed = false;

    template <typename T>
    void raise(T& slot, Kind k) {
        Kind j = join(slot, k);
        if (j != slot) { slot = j; changed = true; }
    }

    Kind val(FnState& st, lithon::ir::ValueId id) {
        return id < st.vals.size() ? st.vals[id] : Kind::Unknown;
    }

    static Kind arith(Kind a, Kind b) {
        if (a == Kind::Unseen || b == Kind::Unseen) return Kind::Unseen;
        if (a == Kind::Int && b == Kind::Int) return Kind::Int;
        bool numeric_a = (a == Kind::Int || a == Kind::Float);
        bool numeric_b = (b == Kind::Int || b == Kind::Float);
        if (numeric_a && numeric_b) return Kind::Float;
        return Kind::Unknown;   // bool operands: interpreter rejects them
    }

    void init(const lithon::ir::Module& m) {
        fns.resize(m.functions.size());
        for (size_t i = 0; i < m.functions.size(); ++i) {
            const auto& f = m.functions[i];
            FnState& st = fns[i];
            st.fn = &f;
            index[f.name] = i;

            lithon::ir::ValueId max_id = 0;
            for (const auto& b : f.blocks)
                for (const auto& in : b.instrs)
                    if (in.result != lithon::ir::kInvalidValue)
                        max_id = std::max(max_id, in.result);
            st.vals.assign(static_cast<size_t>(max_id) + 1, Kind::Unseen);

            st.param_kind.assign(f.params.size(), Kind::Unseen);
            for (size_t p = 0; p < f.params.size(); ++p) {
                Kind decl = p < f.param_type_kinds.size()
                                ? declared_kind(f.param_type_kinds[p])
                                : Kind::Unseen;
                if (decl != Kind::Unseen) st.param_kind[p] = decl;
            }
            if (!f.return_type_kind.empty())
                st.ret = declared_kind(f.return_type_kind);
        }
        // Which functions are entered from IR call sites? Computed once,
        // syntactically (a superset of the truly reachable calls), so
        // the answer never changes during the fixpoint. main is entered
        // by the runtime.
        for (const auto& f : m.functions)
            for (const auto& b : f.blocks)
                for (const auto& in : b.instrs)
                    if (in.op == lithon::ir::Op::Call) {
                        auto it = index.find(in.name);
                        if (it != index.end()) fns[it->second].called = true;
                    }
        // The runtime enters __main__ (the frontend's implicit top-level
        // function) or, for hand-written IR, a plain main.
        for (const char* entry : {"__main__", "main"}) {
            auto mi = index.find(entry);
            if (mi != index.end()) fns[mi->second].called = true;
        }

        // A function nothing calls could be entered from outside with any
        // arguments, so its untyped parameters are unknown.
        for (auto& st : fns) {
            if (st.called) continue;
            for (size_t p = 0; p < st.param_kind.size(); ++p) {
                bool typed = p < st.fn->param_type_kinds.size() &&
                             !st.fn->param_type_kinds[p].empty();
                if (!typed) st.param_kind[p] = Kind::Unknown;
            }
        }
    }

    void run_function(FnState& st, bool final_pass, GuardVerdict* verdict) {
        using lithon::ir::Op;
        const auto& f = *st.fn;
        const bool ret_declared = !f.return_type_kind.empty();

        // parameters seed the variable table
        for (size_t p = 0; p < f.params.size(); ++p)
            raise(st.vars[f.params[p]], st.param_kind[p]);

        for (const auto& block : f.blocks) {
            bool terminated = false;
            for (const auto& in : block.instrs) {
                if (terminated) break;   // dead code after a terminator
                switch (in.op) {
                    case Op::ConstInt:   raise(st.vals[in.result], Kind::Int);   break;
                    case Op::ConstFloat: raise(st.vals[in.result], Kind::Float); break;
                    case Op::ConstBool:  raise(st.vals[in.result], Kind::Bool);  break;

                    case Op::Load: {
                        auto it = st.vars.find(in.name);
                        raise(st.vals[in.result],
                              it == st.vars.end() ? Kind::Unseen : it->second);
                        break;
                    }
                    case Op::Store:
                        if (!in.args.empty())
                            raise(st.vars[in.name], val(st, in.args[0]));
                        break;

                    case Op::Add: case Op::Sub: case Op::Mul:
                    // Mod is typed like Mul, NOT like Div: int % int stays int,
                    // so it must go through arith() unmodified. Routing it
                    // through the Div case would force every modulo result to
                    // Float and the native path would then take the double
                    // branch for `7 % 2`, which is 1 and not 1.0.
                    case Op::Mod:
                        raise(st.vals[in.result],
                              arith(val(st, in.args.at(0)), val(st, in.args.at(1))));
                        if (final_pass) check_arith_operands(st, block, in, verdict);
                        break;
                    case Op::Div: {
                        Kind k = arith(val(st, in.args.at(0)), val(st, in.args.at(1)));
                        raise(st.vals[in.result],
                              k == Kind::Unseen ? k
                              : (k == Kind::Unknown ? k : Kind::Float));
                        if (final_pass) check_arith_operands(st, block, in, verdict);
                        break;
                    }

                    case Op::Lt: case Op::Gt: case Op::Eq: case Op::Not:
                        raise(st.vals[in.result], Kind::Bool);
                        if (final_pass) check_arith_operands(st, block, in, verdict);
                        break;

                    case Op::And: case Op::Or:
                        // value semantics: the result is one of the operands
                        raise(st.vals[in.result],
                              join(val(st, in.args.at(0)), val(st, in.args.at(1))));
                        break;

                    case Op::Phi: {
                        Kind k = Kind::Unseen;
                        for (auto a : in.args) k = join(k, val(st, a));
                        raise(st.vals[in.result], k);
                        break;
                    }

                    case Op::Call: {
                        if (in.name == "print") {
                            if (final_pass) check_print(st, block, in, verdict);
                            break;
                        }
                        auto it = index.find(in.name);
                        if (it == index.end()) {
                            if (in.result != lithon::ir::kInvalidValue)
                                raise(st.vals[in.result], Kind::Unknown);
                            break;
                        }
                        FnState& callee = fns[it->second];
                        for (size_t p = 0; p < in.args.size(); ++p) {
                            if (p < callee.param_kind.size() &&
                                (p >= callee.fn->param_type_kinds.size() ||
                                 callee.fn->param_type_kinds[p].empty()))
                                raise(callee.param_kind[p], val(st, in.args[p]));
                        }
                        if (in.result != lithon::ir::kInvalidValue)
                            raise(st.vals[in.result], callee.ret);
                        break;
                    }

                    case Op::Return:
                        if (!ret_declared) {
                            raise(st.ret, in.args.empty() ? Kind::Unknown
                                                          : val(st, in.args[0]));
                        }
                        break;

                    default: break;
                }
                if (is_terminator(in.op)) terminated = true;
            }
            if (!terminated && !ret_declared) raise(st.ret, Kind::Unknown);
        }
    }

    // A value whose kind is Unknown cannot be given a storage class by
    // codegen, which only ever asks `is_float_value` -- and that predicate is
    // false for Unknown, so an Unknown operand is silently lowered as an
    // *integer*. That is correct only if it really is an integer.
    //
    // It is not, in general. `r = 0.0` then `r = 5` gives r the join
    // Unknown; a later `s * r` where s is a float is arith(Float, Unknown),
    // which is also Unknown, so the multiply is emitted as an integer
    // multiply over a double's raw bit pattern and the result is garbage.
    // Printing a bool from that comparison hides the problem, because a
    // comparison always produces Bool and so always passes the print check.
    //
    // So the guard has to reject the *producer* of every Unknown value that
    // feeds arithmetic, not just Unknown values that are printed. This is
    // the conservative direction: the alternative is for codegen to treat
    // Unknown as Float, which would break the long-standing behaviour where
    // an unresolvable value keeps the integer path.
    //
    // Only arithmetic and comparison consume a value in a way that needs to
    // know int-vs-float. load/store/print of an Unknown are already covered
    // (store of an Unknown is itself a source of Unknown, and print is
    // checked separately).
    void check_arith_operands(FnState& st, const lithon::ir::BasicBlock& block,
                              const lithon::ir::Instr& in, GuardVerdict* verdict) {
        using lithon::ir::Op;
        if (in.op != Op::Add && in.op != Op::Sub && in.op != Op::Mul &&
            in.op != Op::Div && in.op != Op::Mod && in.op != Op::Lt &&
            in.op != Op::Gt && in.op != Op::Eq) {
            return;
        }
        std::string where = st.fn->name + "/" + block.label;
        for (size_t i = 0; i < in.args.size(); ++i) {
            Kind k = val(st, in.args[i]);
            if (k == Kind::Unknown) {
                verdict->native_safe = false;
                verdict->reasons.push_back(
                    where + ": operand %" + std::to_string(in.args[i]) +
                    " of " + (in.op == Op::Div ? "div" :
                              in.op == Op::Mod ? "mod" :
                              in.op == Op::Add ? "add" :
                              in.op == Op::Sub ? "sub" :
                              in.op == Op::Mul ? "mul" :
                              in.op == Op::Lt ? "lt" :
                              in.op == Op::Gt ? "gt" : "eq") +
                    " is " + kind_name(k) +
                    "; native cannot choose between the integer and double "
                    "path for it (a value stored both an int and a float "
                    "reaches here)");
            }
        }
    }

    void check_print(FnState& st, const lithon::ir::BasicBlock& block,
                     const lithon::ir::Instr& in, GuardVerdict* verdict) {
        std::string where = st.fn->name + "/" + block.label;
        if (in.args.size() != 1) {
            verdict->native_safe = false;
            verdict->reasons.push_back(where + ": print() with " +
                std::to_string(in.args.size()) + " arguments (native supports exactly 1)");
            return;
        }
        Kind k = val(st, in.args[0]);
        // Int and Bool are native-safe, and so is Float: the JIT formats a
        // provably-float value with host_format_double(), which reproduces
        // CPython's shortest-roundtrip repr, so the bytes match what the
        // interpreter prints. A join of incomparable kinds is still refused.
        if (k != Kind::Int && k != Kind::Bool && k != Kind::Float) {
            verdict->native_safe = false;
            verdict->reasons.push_back(where + ": print argument %" +
                std::to_string(in.args[0]) + " is " + kind_name(k) +
                "; native print() cannot format it (only int, bool and float are supported)");
        }
    }
};

}  // namespace detail

// Runs the whole-module analysis to a fixpoint, then checks every reachable
// print() and every arithmetic/comparison operand. Returns native_safe=false
// with reasons if anything the emitted code would have to pick an int-vs-float
// lowering for is not provably one of them.
//
// Note that this guards the *whole module*, so it is what tier_runner uses to
// decide whether a program may fall back to the interpreter. lithon_jit
// compiles a single function directly and therefore relies on infer_value_kinds
// alone; a program that check_print_safety refuses is still compilable, it just
// is not trusted to produce the same bytes as the interpreter.
inline GuardVerdict check_print_safety(const lithon::ir::Module& module) {
    detail::Analysis an;
    an.init(module);

    // Lattice height is 3 and state is finite, so this terminates; the
    // cap is a backstop against a bug, and failing it is treated as unsafe.
    bool converged = false;
    for (int iter = 0; iter < 1000; ++iter) {
        an.changed = false;
        for (auto& st : an.fns) an.run_function(st, false, nullptr);
        if (!an.changed) { converged = true; break; }
    }

    GuardVerdict verdict;
    if (!converged) {
        verdict.native_safe = false;
        verdict.reasons.push_back("kind analysis did not converge");
        return verdict;
    }
    for (auto& st : an.fns) an.run_function(st, true, &verdict);
    return verdict;
}

// The same whole-module analysis, exposed per-value instead of collapsed
// into one verdict: kinds[i][id] is the inferred Kind of ValueId `id` in
// module.functions[i] (index-parallel to the module, since both this and
// check_print_safety iterate m.functions in the same order). The code
// generator uses this to decide print() formatting -- Bool vs Int -- for
// modules it compiles directly (e.g. lithon_jit), independently of whether
// the *whole module* would pass the stricter tier_runner safety gate.
inline std::vector<std::vector<Kind>> infer_value_kinds(const lithon::ir::Module& module) {
    detail::Analysis an;
    an.init(module);
    for (int iter = 0; iter < 1000; ++iter) {
        an.changed = false;
        for (auto& st : an.fns) an.run_function(st, false, nullptr);
        if (!an.changed) break;
    }
    std::vector<std::vector<Kind>> out;
    out.reserve(an.fns.size());
    for (auto& st : an.fns) out.push_back(st.vals);
    return out;
}

}  // namespace lithon::jit
