#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#include "runtime/value.h"
#include "ir/ir.h"
#include "ir/text_parser.h"
#include "interpreter/interpreter.h"
#include "typecheck/typecheck.h"

namespace {

// Detects whether this module carries ANY type annotation at all --
// if so, Lithon's mandatory typing discipline applies and the
// type-checker runs automatically. A fully untyped module (no
// annotations anywhere) is treated as the original, pre-0.6
// execution-only path, unchanged -- this is what keeps the original
// 11-program regression suite working without modification.
bool module_has_any_typing(const lithon::ir::Module& module) {
    for (const auto& fn : module.functions) {
        if (!fn.return_type_kind.empty()) return true;
        for (const auto& k : fn.param_type_kinds) {
            if (!k.empty()) return true;
        }
        for (const auto& block : fn.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op == lithon::ir::Op::Store && !instr.type_kind.empty()) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: hello <ir_file>\n";
        return 1;
    }

    std::string ir_path = argv[1];

    std::ifstream file(ir_path);
    if (!file) {
        std::cerr << "error: cannot open " << ir_path << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        lithon::ir::Module module = lithon::ir::parse_ir_text(buffer.str());

        if (module_has_any_typing(module)) {
            auto errors = lithon::typecheck::check_module(module);
            if (!errors.empty()) {
                for (const auto& e : errors) {
                    std::cerr << "RCR error: " << e.message << "\n";
                }
                return 1;
            }
        }

        lithon::interp::run_main(module);

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
