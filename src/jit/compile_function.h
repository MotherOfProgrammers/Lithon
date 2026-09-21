#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <string>
#include "ir/ir.h"
#include "x86_encoder.h"
#include "register_alloc.h"

// The real bridge: walks typed ir::Functions and drives the encoder
// + register allocator to produce genuine, runnable machine code.
//
// This slice adds function CALLS between compiled functions (needed
// for recursion, e.g. fib), on top of straight-line arithmetic,
// control flow, and comparisons already proven working.
//
// Because a Call can target ANY function in the module -- including
// itself, for recursion, or one compiled later in the module -- all
// of a module's functions are compiled into ONE shared CodeBuffer,
// with a two-pass patch scheme covering both intra-function jumps
// (resolved once that function's blocks are done) and inter-function
// calls (resolved only once EVERY function has been compiled, since
// a call may target a function that hasn't been emitted yet).
//
// Spilled temporaries: RBX is reserved, never assigned to a %N value
// (see register_alloc.h) -- it is used purely as transient scratch
// within a single instruction's codegen to read or write a spilled
// value's stack slot. This is necessary correctness, not an
// optimization gap: RAX/RCX/RDX are caller-saved, so any temporary
// whose live range spans a Call is forced to a stack slot by the
// allocator, and RBX is how codegen touches that slot's value for
// one instruction without needing a permanent register of its own.
//
// Still out of scope, documented: floats, And/Or/Not, more than 2
// parameters (System V rdi/rsi only), print() in native codegen.

namespace lithon::jit {

struct CompiledModule {
    std::vector<uint8_t> code;
    std::unordered_map<std::string, size_t> function_offset;
};

inline CompiledModule compile_module(const lithon::ir::Module& module) {
    using namespace lithon::ir;

    CodeBuffer code;
    std::unordered_map<std::string, size_t> function_offset;

    struct PendingCallPatch {
        JumpPatch patch;
        std::string target_function;
    };
    std::vector<PendingCallPatch> pending_calls;

    for (const auto& fn : module.functions) {
        if (function_offset.count(fn.name)) {
            throw std::runtime_error(
                "compile_module: duplicate function name '" + fn.name + "'");
        }
        function_offset[fn.name] = code.size();

        if (fn.params.size() > 2) {
            throw std::runtime_error(
                "compile_module: function '" + fn.name + "' has more than 2 "
                "parameters -- only System V rdi/rsi are supported in this slice");
        }

        RegisterAllocator alloc(fn);

        auto read_value = [&](ValueId id) -> Reg {
            const ValueLocation& loc = alloc.temp_location(id);
            if (loc.in_register) return loc.reg;
            emit_load_rbp_offset(code, Reg::RBX, loc.stack_slot);
            return Reg::RBX;
        };

        auto compute_dest = [&](ValueId id) -> Reg {
            const ValueLocation& loc = alloc.temp_location(id);
            return loc.in_register ? loc.reg : Reg::RBX;
        };

        auto commit_result = [&](ValueId id, Reg computed_in) {
            const ValueLocation& loc = alloc.temp_location(id);
            if (!loc.in_register) {
                emit_store_rbp_offset(code, computed_in, loc.stack_slot);
            }
        };

        emit_prologue(code, alloc.frame_size());

        static const Reg arg_regs[2] = {Reg::RDI, Reg::RSI};
        for (size_t i = 0; i < fn.params.size(); ++i) {
            int offset = alloc.variable_offset(fn.params[i]);
            emit_store_rbp_offset(code, arg_regs[i], offset);
        }

        struct PendingBlockPatch {
            JumpPatch patch;
            std::string target_label;
        };
        std::vector<PendingBlockPatch> pending_blocks;
        std::unordered_map<std::string, size_t> block_offset;

        for (const auto& block : fn.blocks) {
            block_offset[block.label] = code.size();

            for (const auto& instr : block.instrs) {
                switch (instr.op) {
                    case Op::ConstInt: {
                        Reg dst = compute_dest(instr.result);
                        emit_mov_reg_imm64(code, dst, instr.int_imm);
                        commit_result(instr.result, dst);
                        break;
                    }
                    case Op::Load: {
                        if (!alloc.has_variable(instr.name)) {
                            throw std::runtime_error(
                                "compile_module: load of undeclared variable '" + instr.name + "'");
                        }
                        Reg dst = compute_dest(instr.result);
                        emit_load_rbp_offset(code, dst, alloc.variable_offset(instr.name));
                        commit_result(instr.result, dst);
                        break;
                    }
                    case Op::Store: {
                        Reg src = read_value(instr.args.at(0));
                        if (!alloc.has_variable(instr.name)) {
                            throw std::runtime_error(
                                "compile_module: store to undeclared variable '" + instr.name + "'");
                        }
                        emit_store_rbp_offset(code, src, alloc.variable_offset(instr.name));
                        break;
                    }
                    case Op::Add:
                    case Op::Sub:
                    case Op::Mul: {
                        Reg lhs = read_value(instr.args.at(0));
                        Reg rhs = read_value(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        emit_mov_reg_reg(code, dst, lhs);
                        if (instr.op == Op::Add) emit_add_reg_reg(code, dst, rhs);
                        else if (instr.op == Op::Sub) emit_sub_reg_reg(code, dst, rhs);
                        else emit_imul_reg_reg(code, dst, rhs);
                        commit_result(instr.result, dst);
                        break;
                    }
                    case Op::Lt:
                    case Op::Gt:
                    case Op::Eq: {
                        Reg lhs = read_value(instr.args.at(0));
                        Reg rhs = read_value(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        Cond cond = instr.op == Op::Lt ? Cond::Less
                                  : instr.op == Op::Gt ? Cond::Greater
                                  : Cond::Equal;
                        emit_cmp_reg_reg(code, lhs, rhs);
                        emit_setcc(code, cond, dst);
                        emit_movzx_reg_reg8(code, dst, dst);
                        commit_result(instr.result, dst);
                        break;
                    }
                    case Op::Branch: {
                        Reg cond_reg = read_value(instr.args.at(0));
                        size_t comma = instr.name.find(',');
                        std::string then_label = instr.name.substr(0, comma);
                        std::string else_label = instr.name.substr(comma + 1);

                        emit_test_reg_reg(code, cond_reg);
                        JumpPatch to_then = emit_jcc_rel32(code, Cond::NotZero);
                        pending_blocks.push_back({to_then, then_label});

                        JumpPatch to_else = emit_jmp_rel32(code);
                        pending_blocks.push_back({to_else, else_label});
                        break;
                    }
                    case Op::Jump: {
                        JumpPatch to_target = emit_jmp_rel32(code);
                        pending_blocks.push_back({to_target, instr.name});
                        break;
                    }
                    case Op::Call: {
                        if (instr.name == "print") {
                            throw std::runtime_error(
                                "compile_module: print() calls not implemented in "
                                "native codegen yet -- interpreter-only for now");
                        }
                        if (instr.args.size() > 2) {
                            throw std::runtime_error(
                                "compile_module: calls with more than 2 arguments "
                                "not supported in this slice");
                        }
                        for (size_t i = 0; i < instr.args.size(); ++i) {
                            Reg src = read_value(instr.args[i]);
                            emit_mov_reg_reg(code, arg_regs[i], src);
                        }
                        JumpPatch to_callee = emit_jmp_rel32(code);
                        code[to_callee.rel32_offset - 1] = 0xE8; // jmp -> call
                        pending_calls.push_back({to_callee, instr.name});

                        if (instr.result != kInvalidValue) {
                            Reg dst = compute_dest(instr.result);
                            if (dst != Reg::RAX) {
                                emit_mov_reg_reg(code, dst, Reg::RAX);
                            }
                            commit_result(instr.result, dst);
                        }
                        break;
                    }
                    case Op::Return: {
                        if (!instr.args.empty()) {
                            Reg src = read_value(instr.args.at(0));
                            if (src != Reg::RAX) {
                                emit_mov_reg_reg(code, Reg::RAX, src);
                            }
                        }
                        emit_epilogue(code);
                        emit_ret(code);
                        break;
                    }
                    default:
                        throw std::runtime_error(
                            "compile_module: opcode not implemented in this slice "
                            "(boolean ops And/Or/Not and floats are future increments)");
                }
            }
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

    return CompiledModule{std::move(code), std::move(function_offset)};
}

} // namespace lithon::jit
