#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

// Basic-block IR with PHI nodes. Do not add instructions beyond
// M1's required set without updating docs/V1_SPEC.md and
// docs/ROADMAP.md first.

namespace lithon::ir {

enum class Op : uint8_t {
    ConstInt,
    ConstFloat,
    ConstBool,
    Load,
    Store,

    Add,
    Sub,
    Mul,
    Div,

    Lt,
    Gt,
    Eq,

    And,
    Or,
    Not,

    Call,
    Return,

    Branch,
    Jump,

    Phi
};

using ValueId = uint32_t;
constexpr ValueId kInvalidValue = 0xFFFFFFFF;

struct Instr {
    Op op;
    ValueId result;
    std::vector<ValueId> args;

    int64_t int_imm = 0;
    double float_imm = 0.0;
    std::string name;
    std::string type_kind;   // "int" | "float" | "str" | "bool" | "" (none)
    int type_width = -1;      // bit width/byte capacity; -1 for bool or "none"
};

struct BasicBlock {
    std::string label;
    std::vector<Instr> instrs;
};

struct Function {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> param_type_kinds;   // parallel to params; "" if untyped
    std::vector<int> param_type_widths;           // parallel to params; -1 if untyped/bool
    std::string return_type_kind;                 // "" if untyped
    int return_type_width = -1;
    std::vector<BasicBlock> blocks;
};

struct Module {
    std::vector<Function> functions;
};

} // namespace lithon::ir
