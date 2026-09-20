#pragma once
#include <stdexcept>
#include <unordered_map>
#include "ir/ir.h"
#include "x86_encoder.h"
#include "register_alloc.h"

namespace lithon::jit {

inline CodeBuffer compile_function(const lithon::ir::Function& fn) {
    using namespace lithon::ir;

    if (fn.blocks.size() != 1) {
        throw std::runtime_error(
            "compile_function: only single-block (straight-line) functions "
            "are supported in this slice, Branch/Jump codegen not implemented yet");
    }
    if (fn.params.size() > 2) {
        throw std::runtime_error(
            "compile_function: only up to 2 parameters supported in this slice "
            "(System V rdi/rsi), arg registers 3+ would collide with the "
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
    // Move incoming arguments (rdi, rsi) into their variable stack slots.
    static const Reg arg_regs[2] = {Reg::RDI, Reg::RSI};
    for (size_t i = 0; i < fn.params.size(); ++i) {
        int offset = alloc.variable_offset(fn.params[i]);
        emit_store_rbp_offset(code, arg_regs[i], offset);
    }

    const BasicBlock& block = fn.blocks.front();

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
                    "(comparisons, boolean ops, branches, calls, and floats "
                    "are future increments)");
        }
    }

    return code;
}

} // namespace lithon::jit
