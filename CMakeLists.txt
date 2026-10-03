cmake_minimum_required(VERSION 3.20)
project(lithon CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hello
    src/hello.cpp
    src/ir/text_parser.cpp
    src/interpreter/interpreter.cpp
    src/typecheck/typecheck.cpp
)
target_include_directories(hello PUBLIC src)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|X86_64)$")
    # In-process JIT runner: IR file -> machine code in an executable page
    # -> call main(). No temp files, no subprocess.
    add_executable(lithon_jit
        src/jit/lithon_jit.cpp
        src/ir/text_parser.cpp
        src/typecheck/typecheck.cpp
    )
    target_include_directories(lithon_jit PUBLIC src)

    add_executable(benchmark_fib src/jit/benchmark_fib.cpp src/interpreter/interpreter.cpp)
    target_include_directories(benchmark_fib PUBLIC src)

    # tier_runner: what the installed `lithon` command (cli.py -> init.py)
    # actually calls. Portable (exec_memory.h) and ABI-aware, so -- unlike
    # the encoder/stack/branch tests below -- not restricted to POSIX.
    add_executable(tier_runner
        src/jit/tier_runner.cpp
        src/ir/text_parser.cpp
        src/interpreter/interpreter.cpp
        src/typecheck/typecheck.cpp
    )
    target_include_directories(tier_runner PUBLIC src src/jit)

    enable_testing()

    # typecheck_test: pins the static rules on their own, with no JIT and no
    # interpreter in the way. Portable -- it is pure IR plus check_module().
    add_executable(typecheck_test
        src/typecheck/typecheck_test.cpp
        src/typecheck/typecheck.cpp
        src/ir/text_parser.cpp
    )
    target_include_directories(typecheck_test PUBLIC src src/jit)
    add_test(NAME typecheck_test COMMAND typecheck_test)

    # These go through compile_module(), which is ABI-aware
    # (System V / Microsoft x64), so they run on every platform.
    set(LITHON_JIT_TESTS
        liveness_test
        loop_info_test
        ssa_test
        regalloc_test
        compile_function_test
        compile_function_branch_test
        compile_function_loop_test
        compile_function_diamond_test
        compile_function_unroll_shift_test
        cpu_features_test
        compile_module_call_test
        compile_module_fib_test
        compile_module_bool_print_test
        compile_module_accum_float_test
        print_guard_entry_test
    )
    # These hand-assemble byte sequences that hard-code System V
    # argument registers (rdi/rsi), so they are POSIX-only by design.
    if(NOT WIN32)
        list(APPEND LITHON_JIT_TESTS encoder_test stack_test branch_test)
    endif()

    foreach(t ${LITHON_JIT_TESTS})
        add_executable(${t} src/jit/${t}.cpp)
        target_include_directories(${t} PUBLIC src)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    # Parses IR text (unlike the hand-built-Instr tests above), so it also
    # needs text_parser.cpp.
    add_executable(print_guard_test src/jit/print_guard_test.cpp src/ir/text_parser.cpp)
    target_include_directories(print_guard_test PUBLIC src src/jit)
    add_test(NAME print_guard_test COMMAND print_guard_test)

    add_executable(optimize_lsr_test src/jit/optimize_lsr_test.cpp src/ir/text_parser.cpp)
    target_include_directories(optimize_lsr_test PUBLIC src src/jit)
    add_test(NAME optimize_lsr_test COMMAND optimize_lsr_test)

    add_executable(optimize_accum_test src/jit/optimize_accum_test.cpp src/ir/text_parser.cpp)
    target_include_directories(optimize_accum_test PUBLIC src src/jit)
    add_test(NAME optimize_accum_test COMMAND optimize_accum_test)

    # host_format_double is the single function both the JIT (called by
    # emitted code) and the interpreter use to render a double, so it is the
    # one place where CPython's repr rules can be pinned directly. The
    # expectations below are CPython's, transcribed by hand -- not generated
    # from this code, which is the whole point of the test.
    add_executable(float_format_test src/jit/float_format_test.cpp)
    target_include_directories(float_format_test PUBLIC src src/jit)
    add_test(NAME float_format_test COMMAND float_format_test)

    # Needs real machine code to be executed, so this is POSIX-only (fork +
    # waitpid) and does not build on Windows. The div-by-zero trap calls
    # exit(1), which is why one case runs in a child process.
    if(NOT WIN32)
        add_executable(compile_module_float_test src/jit/compile_module_float_test.cpp)
        target_include_directories(compile_module_float_test PUBLIC src src/jit)
        add_test(NAME compile_module_float_test COMMAND compile_module_float_test)

        # mem2reg round-trip: executes JIT'd code, parses IR text.
        add_executable(mem2reg_test src/jit/mem2reg_test.cpp src/ir/text_parser.cpp)
        target_include_directories(mem2reg_test PUBLIC src src/jit)
        add_test(NAME mem2reg_test COMMAND mem2reg_test)

        # 2.5 Phi copies as register moves: asserts the allocation, not just
        # the answer -- a value-only test would pass with the phase disabled.
        add_executable(phi_register_test src/jit/phi_register_test.cpp src/ir/text_parser.cpp)
        target_include_directories(phi_register_test PUBLIC src src/jit)
        add_test(NAME phi_register_test COMMAND phi_register_test)

        # 2.4 DSE + copy propagation on SSA: executes JIT'd code, parses IR text.
        add_executable(optimize_ssa_test src/jit/optimize_ssa_test.cpp src/ir/text_parser.cpp)
        target_include_directories(optimize_ssa_test PUBLIC src src/jit)
        add_test(NAME optimize_ssa_test COMMAND optimize_ssa_test)

        # 2.1 loop canonicalization: executes JIT'd code, parses IR text.
        add_executable(loop_canon_test src/jit/loop_canon_test.cpp src/ir/text_parser.cpp)
        target_include_directories(loop_canon_test PUBLIC src src/jit)
        add_test(NAME loop_canon_test COMMAND loop_canon_test)
    endif()

    # NOTE: tools/verify_all.sh also builds typecheck_entry_test and
    # interpreter_entry_test, but neither src/typecheck/typecheck_entry_test.cpp
    # nor src/interpreter/interpreter_entry_test.cpp exists in this tree yet --
    # that script will fail on that step until those files are added. Not
    # wired here since there's nothing to build.
endif()
