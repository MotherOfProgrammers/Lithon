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

    // Integer/float remainder. Typed like Mul (int iff both operands are
    // int, else float), NOT like Div: true division has to widen because a
    // quotient generally is not an integer, but a remainder never leaves the
    // domain. Semantics are C's, i.e. the sign follows the dividend and the
    // result truncates toward zero -- NOT Python's floored `%`, where
    // -7 % 3 is 2. See interpreter.cpp's apply_binop for why.
    Mod,

    Lt,
    Gt,
    Eq,

    And,
    Or,
    Not,

    // Bitwise, and integer-only. These are deliberately separate from the
    // And/Or above rather than overloading them: those are logical ops with
    // value semantics (the result IS one of the operands, so `x and y` is y
    // whenever x is truthy), which is not a bit operation at all. A single
    // op that meant both would make `&` and `and` differ in result TYPE, not
    // just in result value, and the print guard's int/bool tracking -- which
    // is the whole reason these are separate -- would have nothing to key on.
    //
    // Shl/Shr are 64-bit two's-complement. Shr is an ARITHMETIC shift: it
    // replicates the sign bit, matching Python's >> and the interpreter's
    // int64_t >>, so -1 >> 1 is -1 here too. Shift counts are range-checked
    // to 0..63 at compile time when literal, and at runtime otherwise,
    // because x86 masks the count to 6 bits -- a count of 64 would silently
    // execute as 0 and quietly produce the wrong answer instead of trapping.
    Shl,
    Shr,
    BitAnd,
    BitOr,
    BitXor,

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
