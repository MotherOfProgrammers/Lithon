// lithon_jit: run a Lithon IR file entirely in-process.
//
//   IR text -> parse -> (typecheck if annotated) -> optimise -> x86-64
//   machine code -> executable page -> call main().
//
// No temp files, no subprocess, no interpreter on the hot path.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ir/ir.h"
#include "ir/text_parser.h"
#include "typecheck/typecheck.h"
#include "jit/compile_function.h"
#include "jit/exec_memory.h"

using namespace lithon;

static bool module_has_any_typing(const ir::Module& module) {
    for (const auto& fn : module.functions) {
        if (!fn.return_type_kind.empty()) return true;
        for (const auto& k : fn.param_type_kinds) if (!k.empty()) return true;
        for (const auto& block : fn.blocks)
            for (const auto& instr : block.instrs)
                if (instr.op == ir::Op::Store && !instr.type_kind.empty()) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    std::string path;
    bool dump_hex = false, stats = false;
    jit::CompileOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump-hex")) dump_hex = true;
        else if (!std::strcmp(argv[i], "--stats")) stats = true;
        else if (!std::strcmp(argv[i], "--no-opt")) options.optimize = false;
        else if (!std::strcmp(argv[i], "--no-promote")) options.promote_registers = false;
        else if (!std::strcmp(argv[i], "--no-rotate")) options.rotate_loops = false;
        else if (!std::strncmp(argv[i], "--unroll=", 9)) options.unroll_factor = std::atoi(argv[i] + 9);
        else path = argv[i];
    }
    if (path.empty()) {
        std::cerr << "usage: lithon_jit <ir_file> [--dump-hex] [--stats]\n"
                     "                  [--no-opt] [--no-promote] [--no-rotate] [--unroll=N]\n";
        return 1;
    }

    std::ifstream file(path);
    if (!file) { std::cerr << "error: cannot open " << path << "\n"; return 1; }
    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        ir::Module module = ir::parse_ir_text(buffer.str());

        if (module_has_any_typing(module)) {
            auto errors = typecheck::check_module(module);
            if (!errors.empty()) {
                for (const auto& e : errors) std::cerr << "RCR error: " << e.message << "\n";
                return 1;
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        jit::CompiledModule compiled = jit::compile_module(module, options);
        jit::ExecutableBuffer exec(compiled.code);
        auto t1 = std::chrono::steady_clock::now();

        auto it = compiled.function_offset.find("__main__");
        if (it == compiled.function_offset.end()) it = compiled.function_offset.find("main");
        if (it == compiled.function_offset.end()) {
            std::cerr << "error: no '__main__' or 'main' function in module\n";
            return 1;
        }

        if (dump_hex) {
            std::fprintf(stderr, "[+] %zu bytes x64 @ %p\n[+] ", compiled.code.size(),
                         static_cast<void*>(exec.data()));
            for (size_t i = 0; i < compiled.code.size() && i < 64; ++i)
                std::fprintf(stderr, "%02x ", compiled.code[i]);
            std::fprintf(stderr, "%s\n", compiled.code.size() > 64 ? "..." : "");
        }

        auto main_fn = exec.entry<int64_t (*)()>(it->second);
        auto t2 = std::chrono::steady_clock::now();
        main_fn();
        std::fflush(stdout);
        auto t3 = std::chrono::steady_clock::now();

        if (stats) {
            using ms = std::chrono::duration<double, std::milli>;
            std::fprintf(stderr, "[+] compile %.3f ms, run %.3f ms\n",
                         ms(t1 - t0).count(), ms(t3 - t2).count());
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
