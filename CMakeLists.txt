cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(GXX NAMES g++-14 g++ REQUIRED)
  set(CMAKE_CXX_COMPILER "${GXX}" CACHE STRING "" FORCE)
endif()

project(rye CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

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

add_executable(rye
    src/main.cpp
    src/test_runner.cpp
    src/frontend/project.cpp
    src/lsp/json.cpp
    src/lsp/ide.cpp
    src/lsp/format.cpp
    src/lsp/server.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/parser/comptime_parse.cpp
    src/parser/for_in_desugar.cpp
    src/checker/checker.cpp
    src/checker/checker_comptime.cpp
    src/ir/ir.cpp
    src/ir/irgen.cpp
    src/codegen/regalloc.cpp
    src/codegen/codegen.cpp
    src/codegen/emitters/aarch64_macos_gas.cpp
    src/codegen/emitters/aarch64_linux_gas.cpp
    src/codegen/emitters/x86_64_linux_gas.cpp)

target_include_directories(rye PRIVATE src)
target_link_libraries(rye PRIVATE magic_enum::magic_enum)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  if(APPLE)
    target_compile_options(rye PRIVATE -stdlib=libc++)
    target_link_options(rye PRIVATE -stdlib=libc++)
  else()
    target_compile_options(rye PRIVATE -stdlib=libstdc++)
    target_link_options(rye PRIVATE -stdlib=libstdc++)
  endif()
endif()
