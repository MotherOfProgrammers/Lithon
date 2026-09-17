#include "text_parser.h"

#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace lithon::ir {

namespace {

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, delim)) {
        out.push_back(trim(cur));
    }
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

ValueId parse_value_ref(const std::string& tok) {
    if (tok.empty() || tok[0] != '%') {
        throw std::runtime_error("expected value reference starting with '%', got: " + tok);
    }
    return static_cast<ValueId>(std::stoul(tok.substr(1)));
}

struct OpAndArgs {
    std::string op_name;
    std::vector<std::string> raw_args;
};

OpAndArgs split_op_and_args(const std::string& rhs) {
    size_t space_pos = rhs.find(' ');
    OpAndArgs result;
    if (space_pos == std::string::npos) {
        result.op_name = rhs;
        return result;
    }
    result.op_name = rhs.substr(0, space_pos);
    std::string arg_str = trim(rhs.substr(space_pos + 1));
    if (!arg_str.empty()) {
        result.raw_args = split(arg_str, ',');
    }
    return result;
}

} // namespace

Module parse_ir_text(const std::string& text) {
    Module module;
    Function* current_fn = nullptr;
    BasicBlock* current_block = nullptr;

    std::istringstream stream(text);
    std::string raw_line;

    while (std::getline(stream, raw_line)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;

        if (starts_with(line, "function ")) {
            std::string rest = line.substr(std::string("function ").size());
            if (!rest.empty() && rest.back() == ':') rest.pop_back();

            size_t paren_open = rest.find('(');
            size_t paren_close = rest.find(')');
            std::string name = rest.substr(0, paren_open);
            std::vector<std::string> params;
            if (paren_open != std::string::npos && paren_close != std::string::npos) {
                std::string param_str = rest.substr(paren_open + 1, paren_close - paren_open - 1);
                param_str = trim(param_str);
                if (!param_str.empty()) {
                    params = split(param_str, ',');
                }
            }

            module.functions.push_back(Function{name, params, {}});
            current_fn = &module.functions.back();
            current_block = nullptr;
            continue;
        }

        if (line.back() == ':' && line.find('=') == std::string::npos) {
            if (!current_fn) {
                throw std::runtime_error("block label outside of any function: " + line);
            }
            std::string label = line.substr(0, line.size() - 1);
            current_fn->blocks.push_back(BasicBlock{label, {}});
            current_block = &current_fn->blocks.back();
            continue;
        }

        if (!current_block) {
            throw std::runtime_error("instruction outside of any block: " + line);
        }

        Instr instr;
        instr.result = kInvalidValue;

        size_t eq_pos = line.find('=');
        std::string rhs;
        if (eq_pos != std::string::npos) {
            std::string lhs = trim(line.substr(0, eq_pos));
            instr.result = parse_value_ref(lhs);
            rhs = trim(line.substr(eq_pos + 1));
        } else {
            rhs = line;
        }

        OpAndArgs oa = split_op_and_args(rhs);

        if (oa.op_name == "const_i64") {
            instr.op = Op::ConstInt;
            instr.int_imm = std::stoll(oa.raw_args.at(0));
        } else if (oa.op_name == "const_f64") {
            instr.op = Op::ConstFloat;
            instr.float_imm = std::stod(oa.raw_args.at(0));
        } else if (oa.op_name == "const_bool") {
            instr.op = Op::ConstBool;
            instr.int_imm = std::stoll(oa.raw_args.at(0));
        } else if (oa.op_name == "load") {
            instr.op = Op::Load;
            instr.name = oa.raw_args.at(0);
        } else if (oa.op_name == "store") {
            instr.op = Op::Store;
            instr.name = oa.raw_args.at(0);
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "add") {
            instr.op = Op::Add;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "sub") {
            instr.op = Op::Sub;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "mul") {
            instr.op = Op::Mul;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "div") {
            instr.op = Op::Div;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "lt") {
            instr.op = Op::Lt;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "gt") {
            instr.op = Op::Gt;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "eq") {
            instr.op = Op::Eq;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "and") {
            instr.op = Op::And;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "or") {
            instr.op = Op::Or;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.args.push_back(parse_value_ref(oa.raw_args.at(1)));
        } else if (oa.op_name == "not") {
            instr.op = Op::Not;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
        } else if (oa.op_name == "branch") {
            instr.op = Op::Branch;
            instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            instr.name = oa.raw_args.at(1) + "," + oa.raw_args.at(2);
        } else if (oa.op_name == "jump") {
            instr.op = Op::Jump;
            instr.name = oa.raw_args.at(0);
        } else if (oa.op_name == "call") {
            instr.op = Op::Call;
            instr.name = oa.raw_args.at(0);
            for (size_t i = 1; i < oa.raw_args.size(); ++i) {
                instr.args.push_back(parse_value_ref(oa.raw_args[i]));
            }
        } else if (oa.op_name == "return") {
            instr.op = Op::Return;
            if (!oa.raw_args.empty()) {
                instr.args.push_back(parse_value_ref(oa.raw_args.at(0)));
            }
        } else {
            throw std::runtime_error("unrecognized IR opcode: " + oa.op_name);
        }

        current_block->instrs.push_back(instr);
    }

    return module;
}

} // namespace lithon::ir
