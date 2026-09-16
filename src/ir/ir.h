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
    ValueId result;              // kInvalidValue if the instr has no result
    std::vector<ValueId> args;   // operand value ids

    // Payload for leaf/const instructions and named refs (Load/Store/Call target).
    int64_t int_imm = 0;
    double float_imm = 0.0;
    std::string name;
};

struct BasicBlock {
    std::string label;
    std::vector<Instr> instrs;
};

struct Function {
    std::string name;
    std::vector<std::string> params;   // positional args, per V1_SPEC
    std::vector<BasicBlock> blocks;
};

struct Module {
    std::vector<Function> functions;
};

} // namespace lithon::ir
