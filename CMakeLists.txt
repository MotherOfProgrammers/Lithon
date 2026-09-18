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
