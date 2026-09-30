// tier_runner: run an IR file on the interpreter, the native JIT, or
// "auto" (native only when provably output-safe, else interpreter).
//
//   tier_runner <file.ir> [--interp | --native | --auto | --strict]   (default: --auto)
//
//   --interp  interpreter only (the correctness oracle)
//   --auto    native when print-safe, else interpreter fallback
//   --strict  native only. If the guard or the JIT refuses, exit 3 and run
//             nothing: no interpreter fallback ("slow paths never exist").
//   --native  force the JIT with the guard OFF. Unsafe: for testing only.
//
// Linux/x86-64 only (uses mmap/mprotect).
//
// Which tier actually ran is reported on stderr as "[tier0]" (interpreter)
// or "[tier1]" (native), so a test harness can compare stdout between
// modes and still know what executed. Program output goes to stdout only.
//
// Build:
//   g++ -std=c++20 -O2 -Isrc -Isrc/jit -o build/tier_runner
//       src/jit/tier_runner.cpp src/ir/text_parser.cpp
//       src/interpreter/interpreter.cpp src/typecheck/typecheck.cpp

#include <sys/mman.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "compile_function.h"
#include "interpreter/interpreter.h"
#include "ir/text_parser.h"
#include "print_guard.h"
#include "typecheck/typecheck.h"

namespace {

bool has_type_annotations(const lithon::ir::Module& m) {
    for (const auto& fn : m.functions) {
        if (!fn.return_type_kind.empty()) return true;
        for (const auto& k : fn.param_type_kinds)
            if (!k.empty()) return true;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.instrs)
                if (in.op == lithon::ir::Op::Store && !in.type_kind.empty()) return true;
    }
    return false;
}

void run_interpreter(const lithon::ir::Module& m) {
    std::fputs("[tier0] interpreter\n", stderr);
    lithon::interp::run_main(m);
    std::cout.flush();
}

// Returns false (and prints why) if native compilation failed, so the
// caller can fall back. Nothing has executed when this returns false.
bool run_native(const lithon::ir::Module& m, lithon::jit::CompileOptions options = {}) {
    lithon::jit::CompiledModule compiled;
    try {
        compiled = lithon::jit::compile_module(m, options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[tier0] native compile refused: %s\n", e.what());
        return false;
    }

    auto main_it = compiled.function_offset.find("__main__");
    if (main_it == compiled.function_offset.end())
        main_it = compiled.function_offset.find("main");
    if (main_it == compiled.function_offset.end()) {
        std::fputs("[tier0] native compile refused: no __main__/main entry\n", stderr);
        return false;
    }

    void* mem = mmap(nullptr, compiled.code.size(), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { std::perror("mmap"); return false; }
    std::memcpy(mem, compiled.code.data(), compiled.code.size());
    if (mprotect(mem, compiled.code.size(), PROT_READ | PROT_EXEC) != 0) {
        std::perror("mprotect");
        munmap(mem, compiled.code.size());
        return false;
    }

    std::fputs("[tier1] native\n", stderr);
    using Fn = void (*)();
    auto entry = reinterpret_cast<Fn>(static_cast<uint8_t*>(mem) + main_it->second);
    entry();
    std::fflush(stdout);   // native print() goes through libc stdio
    munmap(mem, compiled.code.size());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: tier_runner <file.ir> [--interp|--native|--auto|--strict]\n"
                     "                  [--no-lsr] [--unroll-diamonds]\n";
        return 2;
    }
    const std::string path = argv[1];
    std::string mode = "--auto";
    lithon::jit::CompileOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--interp" || a == "--native" || a == "--auto" || a == "--strict") {
            mode = a;
        } else if (a == "--no-lsr") {
            options.strength_reduce = false;
        } else if (a == "--unroll-diamonds") {
            options.unroll_diamonds = true;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return 2;
        }
    }

    std::ifstream file(path);
    if (!file) { std::cerr << "error: cannot open " << path << "\n"; return 2; }
    std::stringstream buf;
    buf << file.rdbuf();

    try {
        auto module = lithon::ir::parse_ir_text(buf.str());

        if (has_type_annotations(module)) {
            auto errors = lithon::typecheck::check_module(module);
            if (!errors.empty()) {
                for (const auto& e : errors) std::cerr << "RCR error: " << e.message << "\n";
                return 1;
            }
        }

        if (mode == "--interp") { run_interpreter(module); return 0; }

        if (mode == "--auto" || mode == "--strict") {
            auto verdict = lithon::jit::check_print_safety(module);
            if (!verdict.native_safe) {
                for (const auto& r : verdict.reasons)
                    std::fprintf(stderr, "[guard] %s\n", r.c_str());
                if (mode == "--strict") {
                    std::fputs("[strict] refused: cannot prove native output is safe\n",
                               stderr);
                    return 3;
                }
                run_interpreter(module);
                return 0;
            }
        }

        // --native forces the JIT with no guard (used to demonstrate the divergence)
        if (!run_native(module, options)) {
            if (mode == "--strict") {
                std::fputs("[strict] refused: native compilation failed\n", stderr);
                return 3;
            }
            if (mode == "--native") return 1;
            run_interpreter(module);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
