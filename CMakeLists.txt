cmake_minimum_required(VERSION 3.28)
project(rye CXX)

# Set default build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Enable debug symbols (optional extra flags)
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_C_FLAGS_DEBUG "-g")

include(cmake/CPM.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

CPMAddPackage(
    NAME magic_enum
    GITHUB_REPOSITORY Neargye/magic_enum
    GIT_TAG v0.9.7
)

include_directories(libs/magic_enum/include)

include_directories(src)

add_executable(rye 
    src/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/checker/checker.cpp
    src/ir/ir.cpp
    src/ir/irgen.cpp
    src/codegen/regalloc.cpp
    src/codegen/codegen.cpp
    src/codegen/emitters/aarch64_macos_gas.cpp)
