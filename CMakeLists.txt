cmake_minimum_required(VERSION 3.20)
project(DolRecomp C)

# PowerPC (Broadway) to C static recompiler. DualizeRecompLib runs it on the
# game's DOL and RSO modules; see src/app/config.h for the config it reads.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(DOLRECOMP_BUILD_TESTS "Build DolRecomp's unit tests" OFF)

if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

find_package(ZLIB QUIET)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

set(DOLRECOMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(dr_platform STATIC
    src/platform/fs.c
    src/platform/strutil.c
    src/platform/pathlist.c
    src/platform/process.c)
target_include_directories(dr_platform PUBLIC ${DOLRECOMP_SRC})

add_library(dr_frontend STATIC
    src/frontend/decoder.c
    src/frontend/container/dol.c
    src/frontend/container/rel.c
    src/frontend/container/disc_extract.c)
target_include_directories(dr_frontend PUBLIC ${DOLRECOMP_SRC})
if(ZLIB_FOUND)
    target_compile_definitions(dr_frontend PUBLIC DOLRECOMP_HAVE_ZLIB)
    target_link_libraries(dr_frontend PUBLIC ZLIB::ZLIB)
endif()

add_library(dr_analysis STATIC
    src/analysis/embedded_data.c
    src/analysis/functions.c
    src/analysis/symbol_map.c)
target_include_directories(dr_analysis PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_analysis PUBLIC dr_frontend)

add_library(dr_backend STATIC
    src/backend/c_cfg.c
    src/backend/emitter.c
    src/backend/codegen.c
    src/backend/symbols.c)
target_include_directories(dr_backend PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_backend PUBLIC dr_frontend dr_analysis dr_platform)
if(NOT WIN32)
    target_link_libraries(dr_backend PUBLIC Threads::Threads)
endif()

add_library(tomlc99 STATIC thirdparty/tomlc99/toml.c)
target_include_directories(tomlc99 PUBLIC thirdparty/tomlc99)

add_library(dr_app STATIC
    src/app/config.c
    src/app/module_link.c
    src/app/natives.c
    src/app/recompile.c
    src/app/rel_link.c
    src/app/rso_link.c
    src/app/setup.c)
target_include_directories(dr_app PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_app PUBLIC dr_backend tomlc99)

add_executable(dolrecomp src/app/main.c)
target_link_libraries(dolrecomp PRIVATE dr_app)

# What the generated code runs on, the recompiler's side of a game's runtime:
# CPUState and its memory access, the bit-exact Broadway floating point and
# paired singles, exceptions, and the instructions it calls out for. The game
# links it and supplies the rest: memory, the dispatcher behind
# dolrecomp_lookup and dolrecomp_safepoint, and the hooks in CPUState.
add_library(dr_core STATIC
    core/cpu.c
    core/cpu_exception.c
    core/cpu_interpreter.c
    core/cpu_interpreter_float.c
    core/cpu_interpreter_integer.c
    core/cpu_interpreter_table.c)
target_include_directories(dr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
    target_link_libraries(dr_core PUBLIC m)
endif()

# The glue to the generated tables (core/guest_module.h), which the game's
# recompiled library compiles next to its generated code with its flags: it
# includes ppc_recomp.h, so that library puts the output directory on its
# include path and links this PRIVATE.
add_library(dr_guest INTERFACE)
target_sources(dr_guest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/guest_module.c)
target_link_libraries(dr_guest INTERFACE dr_core)

if(DOLRECOMP_BUILD_TESTS)
    enable_testing()
    foreach(test natives rel_link rso_link)
        add_executable(test_${test} tests/test_${test}.c)
        target_link_libraries(test_${test} PRIVATE dr_app)
        add_test(NAME ${test} COMMAND test_${test})
    endforeach()
    foreach(test opcodes cross_check dol rel)
        add_executable(test_${test} tests/test_${test}.c)
        target_link_libraries(test_${test} PRIVATE dr_frontend)
        add_test(NAME ${test} COMMAND test_${test})
    endforeach()
    foreach(test fprf)
        add_executable(test_${test} tests/test_${test}.c)
        target_link_libraries(test_${test} PRIVATE dr_core)
        add_test(NAME ${test} COMMAND test_${test})
    endforeach()
    foreach(test c_cfg)
        add_executable(test_${test} tests/test_${test}.c)
        target_link_libraries(test_${test} PRIVATE dr_backend)
        add_test(NAME ${test} COMMAND test_${test})
    endforeach()

    add_executable(test_symbols tests/test_symbols.c)
    target_link_libraries(test_symbols PRIVATE dr_backend)
    add_test(NAME symbols COMMAND test_symbols
        $<SHELL_PATH:${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codewarrior.map>)

    add_executable(test_disc_extract tests/test_disc_extract.c)
    target_include_directories(test_disc_extract PRIVATE ${DOLRECOMP_SRC})
    add_test(NAME disc_extract
        COMMAND test_disc_extract
            $<SHELL_PATH:$<TARGET_FILE:dolrecomp>>
            $<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/disc_extract_test>)
endif()
