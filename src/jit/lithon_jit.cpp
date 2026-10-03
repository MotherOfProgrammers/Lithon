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
#include "jit/cpu_features.h"
#include "jit/exec_memory.h"
#include "jit/loop_info.h"
#include "jit/ssa.h"

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
    // Detect CPU target features once, before any code generation can ask
    // (1.2). Today nothing emits AVX; this makes the query surface live and
    // costs one CPUID at startup.
    jit::cpu_features::init();

    std::string path, dump_code_path;
    bool dump_hex = false, stats = false, dump_loops = false, dump_phi = false;
    bool run_mem2reg = false, canonicalize = false;
    jit::CompileOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump-hex")) dump_hex = true;
        else if (!std::strcmp(argv[i], "--stats")) stats = true;
        else if (!std::strcmp(argv[i], "--dump-loops")) dump_loops = true;
        else if (!std::strcmp(argv[i], "--dump-phi")) dump_phi = true;
        else if (!std::strcmp(argv[i], "--mem2reg")) { run_mem2reg = true; options.ssa_pipeline = true; }
        else if (!std::strcmp(argv[i], "--ssa")) options.ssa_pipeline = true;
        else if (!std::strcmp(argv[i], "--canonicalize-loops")) canonicalize = true;
        else if (!std::strcmp(argv[i], "--dump-code") && i + 1 < argc) dump_code_path = argv[++i];
        else if (!std::strcmp(argv[i], "--no-opt")) options.optimize = false;
        else if (!std::strcmp(argv[i], "--no-promote")) options.promote_registers = false;
        else if (!std::strcmp(argv[i], "--no-rotate")) options.rotate_loops = false;
        else if (!std::strncmp(argv[i], "--unroll=", 9)) options.unroll_factor = std::atoi(argv[i] + 9);
        else if (!std::strcmp(argv[i], "--unroll-diamonds")) options.unroll_diamonds = true;
        else if (!std::strcmp(argv[i], "--no-lsr")) options.strength_reduce = false;
        else if (!std::strcmp(argv[i], "--accum-unroll")) options.accum_unroll = 4;
        else if (!std::strcmp(argv[i], "--no-accum-unroll")) options.accum_unroll = 1;
        else path = argv[i];
    }
    if (path.empty()) {
        std::cerr << "usage: lithon_jit <ir_file> [--dump-hex] [--stats]\n"
                     "                  [--no-opt] [--no-promote] [--no-rotate] [--unroll=N]\n"
                     "                  [--no-lsr] [--accum-unroll] [--unroll-diamonds]\n"
                     "       --accum-unroll splits a counted reduction's accumulator into 4 partials.\n"
                     "       Off by default; the win is on long-latency float accumulators (~1.6x).\n"
                     "       For a FLOAT accumulator it reassociates the adds, so the value is not\n"
                     "       bit-identical to the interpreter's.\n"
                     "       --unroll-diamonds unrolls if/else diamonds (off: slower on Sandy Bridge).\n"
                     "                  [--dump-code <path>] [--dump-loops] [--dump-phi] [--mem2reg]\n"
                     "                  [--canonicalize-loops] [--ssa]\n"
                     "       --canonicalize-loops gives every loop one preheader and one latch\n"
                     "       before any other phase runs.\n"
                     "       --dump-loops prints the natural loops (header, preheader, latches,\n"
                     "       blocks, exits, nesting) of every function and exits.\n"
                     "       --dump-phi prints the blocks where Phi nodes belong for each\n"
                     "       variable (dominance-frontier placement) and exits.\n"
                     "       --ssa runs the full SSA pipeline in compile_module: canonicalize\n"
                     "       loops, promote locals to SSA, run copy propagation + dead store\n"
                     "       elimination on the SSA form, resolve the phis into edge copies,\n"
                     "       then run. Correctness-first; not a speed win.\n"
                     "       --mem2reg is --ssa plus a per-function report of what it promoted.\n"
                     "       --dump-code writes the raw emitted machine code (the whole buffer,\n"
                     "       unlike --dump-hex's 64-byte preview) to <path> for external tools --\n"
                     "       e.g. objdump -D -b binary -mi386:x86-64 -M intel <path>, which is\n"
                     "       what tools/check_stack_alignment.py does. Compiles but does not run.\n";
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

        if (canonicalize) {
            for (auto& fn : module.functions) {
                jit::CanonStats cs = jit::canonicalize_loops(fn);
                std::fprintf(stderr,
                             "[+] canonicalize %s: +%d preheader(s), %d latch merge(s), "
                             "%d fallthrough(s), %d block(s) added\n",
                             fn.name.c_str(), cs.preheaders, cs.latches_merged,
                             cs.fallthroughs, cs.blocks_added);
            }
        }

        if (dump_loops) {
            // Analysis-only view of the natural loops the SSA passes will
            // build on. No code is generated or run.
            for (const auto& fn : module.functions) {
                jit::LoopInfo info = jit::compute_loop_info(fn);
                std::printf("function %s: %zu loop(s)\n", fn.name.c_str(), info.loops.size());
                auto label = [&](size_t b) {
                    return b == jit::kNoBlock ? std::string("<none>") : fn.blocks[b].label;
                };
                for (const auto& loop : info.loops) {
                    std::printf("  loop header=%s preheader=%s\n",
                                label(loop.header).c_str(), label(loop.preheader).c_str());
                    std::printf("    latches=");
                    for (size_t i = 0; i < loop.latches.size(); ++i)
                        std::printf("%s%s", i ? "," : "", label(loop.latches[i]).c_str());
                    std::printf(" blocks=");
                    for (size_t i = 0; i < loop.blocks.size(); ++i)
                        std::printf("%s%s", i ? "," : "", label(loop.blocks[i]).c_str());
                    std::printf(" exits=");
                    for (size_t i = 0; i < loop.exit_blocks.size(); ++i)
                        std::printf("%s%s", i ? "," : "", label(loop.exit_blocks[i]).c_str());
                    std::printf(" parent=%s\n",
                                loop.parent == jit::kNoBlock ? "<none>"
                                                             : label(info.loops[loop.parent].header).c_str());
                }
            }
            return 0;
        }

        if (dump_phi) {
            // Where Phi nodes would be placed (dominance-frontier analysis).
            // Analysis-only: Op::Phi has no codegen path yet.
            for (const auto& fn : module.functions) {
                jit::PhiPlacement phi = jit::compute_phi_placement(fn);
                std::printf("function %s: %zu phi(s)\n", fn.name.c_str(), phi.phi_count());
                for (size_t b = 0; b < phi.block_phis.size(); ++b) {
                    if (phi.block_phis[b].empty()) continue;
                    std::printf("  block %s:", fn.blocks[b].label.c_str());
                    for (const auto& var : phi.block_phis[b])
                        std::printf(" phi(%s)", var.c_str());
                    std::printf("\n");
                }
            }
            return 0;
        }

        if (run_mem2reg) {
            // Stats-only view of the SSA pipeline: run it on a scratch copy so
            // the numbers can be printed without disturbing the module, which
            // compile_module then pipelines through CompileOptions::ssa_pipeline.
            for (const auto& fn : module.functions) {
                ir::Function scratch = fn;
                jit::SsaStats s = jit::mem2reg(scratch);
                std::string err;
                if (!jit::validate_ssa(scratch, &err)) {
                    std::cerr << "error: invalid SSA for " << fn.name << ": " << err << "\n";
                    return 1;
                }
                jit::OptimizeStats os;
                jit::copy_propagate(scratch, os);
                jit::dead_store_elimination(scratch, os);
                jit::resolve_phis(scratch);
                std::fprintf(stderr,
                             "[+] mem2reg %s: %zu var(s) promoted, %zu declined, %zu phi(s), "
                             "-%zu load(s), -%zu store(s), copies=%d, dead=%d\n",
                             fn.name.c_str(), s.vars_promoted, s.vars_declined,
                             s.phis_materialized, s.loads_removed, s.stores_removed,
                             os.copies_propagated, os.dead_removed);
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        jit::CompiledModule compiled = jit::compile_module(module, options);

        if (!dump_code_path.empty()) {
            std::ofstream out(dump_code_path, std::ios::binary);
            if (!out) { std::cerr << "error: cannot write " << dump_code_path << "\n"; return 1; }
            out.write(reinterpret_cast<const char*>(compiled.code.data()),
                      static_cast<std::streamsize>(compiled.code.size()));
            // Exact function boundaries, so an external tool (e.g.
            // tools/check_stack_alignment.py) can seed rsp=0 at each
            // function's first byte instead of pattern-matching prologues.
            for (const auto& kv : compiled.function_offset) {
                std::cout << kv.first << " " << kv.second << "\n";
            }
            return 0;   // dump-only: compiled code is never executed
        }

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
            // 2.5. Every phi copy is a register move unless the callee-saved
            // pool ran out or the merge carried a double, so the shortfall is
            // what is still going through memory.
            if (compiled.phis_forwarded)
                std::fprintf(stderr, "[+] trivial merges folded away: %zu\n",
                             compiled.phis_forwarded);
            if (compiled.phi_copies_coalesced)
                std::fprintf(stderr, "[+] phi copies deleted by coalescing: %zu\n",
                             compiled.phi_copies_coalesced);
            std::fprintf(stderr, "[+] phi copies: %zu of %zu in registers%s\n",
                         compiled.phi_copies_in_registers, compiled.phi_copies_total,
                         compiled.phi_copies_in_registers == compiled.phi_copies_total
                             ? "" : "  (rest via memory)");
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
