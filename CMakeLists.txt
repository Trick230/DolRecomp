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

# The signatures of the natives below (runtime/sdk/<set>.sigs), built into the
# recompiler: a game's config lists them as natives = ["@<set>", ...].
set(DOLRECOMP_SIGNATURE_SETS mwcc rvl)
set(DOLRECOMP_SIGNATURE_FILES)
foreach(set IN LISTS DOLRECOMP_SIGNATURE_SETS)
    list(APPEND DOLRECOMP_SIGNATURE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk/${set}.sigs")
endforeach()
set(DOLRECOMP_BUILTIN_SIGNATURES "${CMAKE_CURRENT_BINARY_DIR}/builtin_signatures.c")
add_custom_command(
    OUTPUT "${DOLRECOMP_BUILTIN_SIGNATURES}"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${DOLRECOMP_BUILTIN_SIGNATURES}"
        "-DDIR=${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk" "-DSETS=${DOLRECOMP_SIGNATURE_SETS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_signatures.cmake"
    DEPENDS ${DOLRECOMP_SIGNATURE_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_signatures.cmake"
    VERBATIM)

add_library(dr_app STATIC
    src/app/config.c
    src/app/module_link.c
    src/app/natives.c
    src/app/recompile.c
    src/app/rel_link.c
    src/app/rso_link.c
    src/app/setup.c
    "${DOLRECOMP_BUILTIN_SIGNATURES}")
target_include_directories(dr_app PUBLIC ${DOLRECOMP_SRC})
target_link_libraries(dr_app PUBLIC dr_backend tomlc99)

add_executable(dolrecomp src/app/main.c)
target_link_libraries(dolrecomp PRIVATE dr_app)

# The guest CPU the generated code runs on: CPUState, memory access, the
# bit-exact Broadway floating point and paired singles, and an interpreter
# for code the recompiler does not cover.
add_library(dr_runtime STATIC
    runtime/core/cpu.c
    runtime/core/cpu_exception.c
    runtime/core/cpu_interpreter.c
    runtime/core/cpu_interpreter_float.c
    runtime/core/cpu_interpreter_integer.c
    runtime/core/cpu_interpreter_table.c)
target_include_directories(dr_runtime PUBLIC runtime)
if(NOT WIN32)
    target_link_libraries(dr_runtime PUBLIC m)
endif()

# Sources a game's recompiled library compiles next to its generated code,
# with its flags (so they inline across it under LTO): the glue to the
# generated tables (dr_guest) and host implementations of SDK functions any
# game can list in its natives (dr_natives_*). They include ppc_recomp.h and
# ppc_natives.h, so the game's library puts the output directory on its
# include path, and link them PRIVATE.
add_library(dr_guest INTERFACE)
target_sources(dr_guest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/runtime/core/guest_module.c)
target_link_libraries(dr_guest INTERFACE dr_runtime)

# The Metrowerks runtime and MSL: register save/restore helpers, memcpy.
add_library(dr_natives_mwcc INTERFACE)
target_sources(dr_natives_mwcc INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk/msl_string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk/mwcc_runtime.c)
target_link_libraries(dr_natives_mwcc INTERFACE dr_runtime)

# The Revolution SDK's OS (context, interrupts, mutexes), cache and matrix
# functions; their defines are listed beside each source's natives.
add_library(dr_natives_rvl INTERFACE)
target_sources(dr_natives_rvl INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk/rvl_cache.c
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk/rvl_mtx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk/rvl_os.c)
target_link_libraries(dr_natives_rvl INTERFACE dr_runtime)

# The host runtime's CPU layer (C++20), compiled into the host executable with
# its flags: the dispatcher over the generated functions with the interpreter
# beside it, guest calls, guest threads on fibers, the event scheduler, guest
# memory and the savestate stream. The host derives its machine from
# dolrecomp::CpuHost (runtime/host/cpu.hpp) and defines its members. A
# GameCube host defines DOLRECOMP_GAMECUBE (no MEM2, Gekko's clock).
add_library(dr_host INTERFACE)
target_sources(dr_host INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/host/cpu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/host/interpreter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/host/memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/host/scheduler.cpp)
target_link_libraries(dr_host INTERFACE dr_runtime)

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
    foreach(test c_cfg)
        add_executable(test_${test} tests/test_${test}.c)
        target_link_libraries(test_${test} PRIVATE dr_backend)
        add_test(NAME ${test} COMMAND test_${test})
    endforeach()

    # The CPU layer over a hand-made module: picking among overlapping groups.
    enable_language(CXX)
    add_executable(test_overlay_dispatch tests/test_overlay_dispatch.cpp)
    target_link_libraries(test_overlay_dispatch PRIVATE dr_host)
    target_compile_features(test_overlay_dispatch PRIVATE cxx_std_20)
    add_test(NAME overlay_dispatch COMMAND test_overlay_dispatch)

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
