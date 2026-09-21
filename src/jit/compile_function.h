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

// The real bridge: walks a typed ir::Function and drives the
// encoder + register allocator to produce genuine, runnable machine
// code -- replacing the hand-written test sequences with real
// codegen driven by real IR.
//
// This slice adds multi-block control flow (Branch/Jump) and
// comparisons (Lt/Gt/Eq), on top of the straight-line arithmetic
// slice already proven working.
//
// Design for comparisons: Lt/Gt/Eq MATERIALIZE an honest 0/1 value
// into a register (cmp + setcc + movzx), matching the IR's literal
// semantics exactly, rather than fusing compare+branch as a real
// compiler eventually would -- correctness first, that fusion
// optimization is a later increment (V1_SPEC Rule 1).
//
// Design for Branch/Jump: every block is emitted once, in fn.blocks
// order, recording each block's starting byte offset. Every jump's
// target patch is recorded but left unresolved until ALL blocks have
// been emitted -- only then can every label's final offset be known,
// which correctly handles both forward jumps (if/else) and backward
// jumps (loop back-edges) with the same two-pass mechanism.
//
// Still out of scope, documented, not silently skipped: function
// Call codegen, floats, And/Or/Not, spilled-temporary loads.

namespace lithon::jit {

inline CodeBuffer compile_function(const lithon::ir::Function& fn) {
    using namespace lithon::ir;

    if (fn.params.size() > 2) {
        throw std::runtime_error(
            "compile_function: only up to 2 parameters supported in this slice "
            "(System V rdi/rsi) -- arg registers 3+ would collide with the "
            "scratch pool used for temporaries, not handled yet");
    }

    RegisterAllocator alloc(fn);

    auto get_temp_reg = [&](ValueId id) -> Reg {
        const ValueLocation& loc = alloc.temp_location(id);
        if (!loc.in_register) {
            throw std::runtime_error(
                "compile_function: value %" + std::to_string(id) + " was spilled to "
                "the stack -- loading a spilled temporary for use in an operation "
                "is not implemented in this slice");
        }
        return loc.reg;
    };

    CodeBuffer code;
    emit_prologue(code, alloc.frame_size());

    static const Reg arg_regs[2] = {Reg::RDI, Reg::RSI};
    for (size_t i = 0; i < fn.params.size(); ++i) {
        int offset = alloc.variable_offset(fn.params[i]);
        emit_store_rbp_offset(code, arg_regs[i], offset);
    }

    // Pending jump patches: (JumpPatch, target block label), resolved
    // once every block's starting offset is known.
    struct PendingPatch {
        JumpPatch patch;
        std::string target_label;
    };
    std::vector<PendingPatch> pending;
    std::unordered_map<std::string, size_t> block_offset;

    for (const auto& block : fn.blocks) {
        block_offset[block.label] = code.size();

        for (const auto& instr : block.instrs) {
            switch (instr.op) {
                case Op::ConstInt: {
                    Reg dst = get_temp_reg(instr.result);
                    emit_mov_reg_imm64(code, dst, instr.int_imm);
                    break;
                }
                case Op::Load: {
                    if (!alloc.has_variable(instr.name)) {
                        throw std::runtime_error(
                            "compile_function: load of undeclared variable '" + instr.name + "'");
                    }
                    Reg dst = get_temp_reg(instr.result);
                    emit_load_rbp_offset(code, dst, alloc.variable_offset(instr.name));
                    break;
                }
                case Op::Store: {
                    Reg src = get_temp_reg(instr.args.at(0));
                    if (!alloc.has_variable(instr.name)) {
                        throw std::runtime_error(
                            "compile_function: store to undeclared variable '" + instr.name + "'");
                    }
                    emit_store_rbp_offset(code, src, alloc.variable_offset(instr.name));
                    break;
                }
                case Op::Add:
                case Op::Sub:
                case Op::Mul: {
                    Reg dst = get_temp_reg(instr.result);
                    Reg lhs = get_temp_reg(instr.args.at(0));
                    Reg rhs = get_temp_reg(instr.args.at(1));
                    emit_mov_reg_reg(code, dst, lhs);
                    if (instr.op == Op::Add) emit_add_reg_reg(code, dst, rhs);
                    else if (instr.op == Op::Sub) emit_sub_reg_reg(code, dst, rhs);
                    else emit_imul_reg_reg(code, dst, rhs);
                    break;
                }
                case Op::Lt:
                case Op::Gt:
                case Op::Eq: {
                    Reg dst = get_temp_reg(instr.result);
                    Reg lhs = get_temp_reg(instr.args.at(0));
                    Reg rhs = get_temp_reg(instr.args.at(1));
                    Cond cond = instr.op == Op::Lt ? Cond::Less
                              : instr.op == Op::Gt ? Cond::Greater
                              : Cond::Equal;
                    emit_cmp_reg_reg(code, lhs, rhs);
                    emit_setcc(code, cond, dst);        // dst's low byte = 0 or 1
                    emit_movzx_reg_reg8(code, dst, dst); // zero-extend to full 64 bits
                    break;
                }
                case Op::Branch: {
                    Reg cond_reg = get_temp_reg(instr.args.at(0));
                    size_t comma = instr.name.find(',');
                    std::string then_label = instr.name.substr(0, comma);
                    std::string else_label = instr.name.substr(comma + 1);

                    emit_test_reg_reg(code, cond_reg);
                    JumpPatch to_then = emit_jcc_rel32(code, Cond::NotZero);
                    pending.push_back({to_then, then_label});

                    JumpPatch to_else = emit_jmp_rel32(code);
                    pending.push_back({to_else, else_label});
                    break;
                }
                case Op::Jump: {
                    JumpPatch to_target = emit_jmp_rel32(code);
                    pending.push_back({to_target, instr.name});
                    break;
                }
                case Op::Return: {
                    if (!instr.args.empty()) {
                        Reg src = get_temp_reg(instr.args.at(0));
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
                        "compile_function: opcode not implemented in this slice "
                        "(boolean ops And/Or/Not, function calls, and floats "
                        "are future increments)");
            }
        }
    }

    for (const auto& p : pending) {
        auto it = block_offset.find(p.target_label);
        if (it == block_offset.end()) {
            throw std::runtime_error(
                "compile_function: jump to unknown block '" + p.target_label + "'");
        }
        resolve_jump_patch(code, p.patch, it->second);
    }

    return code;
}

// Result of compiling a whole ir::Module into one contiguous code
// image. function_offset maps each function's name to its starting
// byte offset within `code`, so a caller can treat
// (base + function_offset.at(name)) as that function's entry point.
struct CompiledModule {
    std::vector<uint8_t> code;
    std::unordered_map<std::string, size_t> function_offset;
};

// Compiles every function in the module into a single shared buffer.
// Each function is compiled independently by compile_function (its
// jumps are self-contained relative patches, so concatenation cannot
// break them), then appended at a 16-byte-aligned offset.
//
// NOTE: cross-function Call is NOT implemented yet -- compile_function
// still rejects Op::Call with an explicit error, so modules containing
// calls will throw rather than silently miscompile.
inline CompiledModule compile_module(const lithon::ir::Module& module) {
    CompiledModule out;

    for (const auto& fn : module.functions) {
        if (out.function_offset.count(fn.name)) {
            throw std::runtime_error(
                "compile_module: duplicate function name '" + fn.name + "'");
        }

        CodeBuffer fn_code = compile_function(fn);

        while (out.code.size() % 16 != 0) {
            out.code.push_back(0xCC); // int3 padding
        }
        out.function_offset[fn.name] = out.code.size();
        out.code.insert(out.code.end(),
                        fn_code.data(), fn_code.data() + fn_code.size());
    }

    return out;
}

} // namespace lithon::jit
