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

    # These go through compile_module(), which is ABI-aware
    # (System V / Microsoft x64), so they run on every platform.
    set(LITHON_JIT_TESTS
        liveness_test
        regalloc_test
        compile_function_test
        compile_function_branch_test
        compile_function_loop_test
        compile_function_diamond_test
        compile_module_call_test
        compile_module_fib_test
        compile_module_bool_print_test
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

    # NOTE: tools/verify_all.sh also builds typecheck_entry_test and
    # interpreter_entry_test, but neither src/typecheck/typecheck_entry_test.cpp
    # nor src/interpreter/interpreter_entry_test.cpp exists in this tree yet --
    # that script will fail on that step until those files are added. Not
    # wired here since there's nothing to build.
endif()
