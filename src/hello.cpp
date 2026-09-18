#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#include "runtime/value.h"
#include "ir/ir.h"
#include "ir/text_parser.h"
#include "interpreter/interpreter.h"
#include "typecheck/typecheck.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: hello [--typecheck] <ir_file>\n";
        return 1;
    }

    bool do_typecheck = false;
    std::string ir_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--typecheck") {
            do_typecheck = true;
        } else {
            ir_path = arg;
        }
    }

    if (ir_path.empty()) {
        std::cerr << "usage: hello [--typecheck] <ir_file>\n";
        return 1;
    }

    std::ifstream file(ir_path);
    if (!file) {
        std::cerr << "error: cannot open " << ir_path << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        lithon::ir::Module module = lithon::ir::parse_ir_text(buffer.str());

        std::cout << "functions parsed: " << module.functions.size() << "\n";
        if (!module.functions.empty()) {
            std::cout << "function name: " << module.functions[0].name << "\n";
            std::cout << "blocks in it: " << module.functions[0].blocks.size() << "\n";
            if (!module.functions[0].blocks.empty()) {
                std::cout << "instrs in block0: "
                          << module.functions[0].blocks[0].instrs.size() << "\n";
            }
        }

        if (do_typecheck) {
            std::cout << "--- type-checking ---\n";
            auto errors = lithon::typecheck::check_module(module);
            if (!errors.empty()) {
                for (const auto& e : errors) {
                    std::cerr << "RCR error: " << e.message << "\n";
                }
                return 1;
            }
            std::cout << "type-check: OK\n";
        }

        std::cout << "--- running interpreter ---\n";
        lithon::interp::run_main(module);

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
