#pragma once

#include <cstdint>
#include <cstring>

namespace lithon {

enum class ValueTag : uint8_t {
    Int,
    Float,
    Bool,
    None,
    Object
};

struct ObjHeader {
    uint32_t refcount;
    uint16_t type_tag;
    uint16_t flags;
};

// All access must go through this API. No call site outside this
// file may assume the physical layout of LithonValue — this keeps
// the representation swappable (e.g. to NaN-boxing) later without
// touching the interpreter, IR, or JIT.
struct LithonValue {
    uint64_t payload;
    ValueTag tag;

    static LithonValue make_int(int64_t v) {
        LithonValue val;
        val.tag = ValueTag::Int;
        std::memcpy(&val.payload, &v, sizeof(v));
        return val;
    }

    static LithonValue make_float(double v) {
        LithonValue val;
        val.tag = ValueTag::Float;
        std::memcpy(&val.payload, &v, sizeof(v));
        return val;
    }

    static LithonValue make_bool(bool v) {
        LithonValue val;
        val.tag = ValueTag::Bool;
        val.payload = v ? 1 : 0;
        return val;
    }

    static LithonValue make_none() {
        LithonValue val;
        val.tag = ValueTag::None;
        val.payload = 0;
        return val;
    }

    static LithonValue make_object(ObjHeader* ptr) {
        LithonValue val;
        val.tag = ValueTag::Object;
        std::memcpy(&val.payload, &ptr, sizeof(ptr));
        return val;
    }

    bool is_int() const   { return tag == ValueTag::Int; }
    bool is_float() const { return tag == ValueTag::Float; }
    bool is_bool() const  { return tag == ValueTag::Bool; }
    bool is_none() const  { return tag == ValueTag::None; }
    bool is_object() const{ return tag == ValueTag::Object; }

    int64_t as_int() const {
        int64_t v;
        std::memcpy(&v, &payload, sizeof(v));
        return v;
    }

    double as_float() const {
        double v;
        std::memcpy(&v, &payload, sizeof(v));
        return v;
    }

    bool as_bool() const { return payload != 0; }

    ObjHeader* as_object() const {
        ObjHeader* p;
        std::memcpy(&p, &payload, sizeof(p));
        return p;
    }

    // Truthiness per V1_SPEC 0.2: False, 0, 0.0, None are false.
    bool is_truthy() const {
        switch (tag) {
            case ValueTag::Bool:  return payload != 0;
            case ValueTag::Int:   return as_int() != 0;
            case ValueTag::Float: return as_float() != 0.0;
            case ValueTag::None:  return false;
            case ValueTag::Object:return true;
        }
        return true;
    }
};

static_assert(sizeof(ObjHeader) == 8, "ObjHeader must stay 8 bytes per V1_SPEC 0.3");

} // namespace lithon
