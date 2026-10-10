cmake_minimum_required(VERSION 3.24)
project(LaplaceMCP VERSION 0.1.0 LANGUAGES C CXX ASM)

# Laplace-MCP: the HTTP service in front of Laplace. Built as the engine is: Laplace-Native as part of it, under the
# floating-point contract (Laplace-Operations toolchain) with laplace-math answering every libm call, so the entity
# this process names and the coordinate it computes are the ones every other Laplace build computes. The engine's
# reader (read.c, pg.c) is compiled in from the engine's source beside this repository.
set(CMAKE_C_STANDARD 17)
if(DEFINED ENV{LAPLACE_NATIVE} AND NOT "$ENV{LAPLACE_NATIVE}" STREQUAL "")
  set(LAPLACE_NATIVE "$ENV{LAPLACE_NATIVE}" CACHE PATH "Laplace-Native sources")
else()
  set(LAPLACE_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Native" CACHE PATH "Laplace-Native sources")
endif()
add_subdirectory(${LAPLACE_NATIVE} native-lib EXCLUDE_FROM_ALL)
include(${LAPLACE_NATIVE}/cmake/LaplacePaths.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceFlags.cmake)
find_package(OpenMP REQUIRED)

laplace_path(LAPLACE_ENGINE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Engine/src" "Laplace-Engine's sources: the reader this service links")
foreach(f read.c pg.c walk.c engine.h)
  if(NOT EXISTS "${LAPLACE_ENGINE_SRC}/${f}")
    message(FATAL_ERROR "LAPLACE_ENGINE_SRC: no ${f} at '${LAPLACE_ENGINE_SRC}'")
  endif()
endforeach()

# What the process uses when its environment does not say (native/config.h.in): each from the environment this is
# configured in (laplace.env, the machine's declaration), else the deployment target's value.
if(DEFINED ENV{LAPLACE_CONNINFO} AND NOT "$ENV{LAPLACE_CONNINFO}" STREQUAL "")
  set(LAPLACE_CONNINFO "$ENV{LAPLACE_CONNINFO}" CACHE STRING "the database, as a libpq connection string" FORCE)
else()
  set(LAPLACE_CONNINFO "host=/tmp port=5432 user=laplace dbname=laplace" CACHE STRING "the database, as a libpq connection string")
endif()
laplace_path(LAPLACE_BUILD "/repos/build" "build trees")
laplace_path(LAPLACE_ENGINE "${LAPLACE_BUILD}/Laplace-Engine/icx-release/laplace" "the engine program the routes run")
foreach(pair "LAPLACE_MCP_HOST=127.0.0.1" "LAPLACE_MCP_PORT=5188")
  string(REGEX MATCH "^[^=]+" name "${pair}")
  string(REGEX REPLACE "^[^=]+=" "" default "${pair}")
  if(DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
    set(${name} "$ENV{${name}}" CACHE STRING "lpm serve: ${name}" FORCE)
  else()
    set(${name} "${default}" CACHE STRING "lpm serve: ${name}")
  endif()
endforeach()
if(NOT LAPLACE_MCP_PORT MATCHES "^[0-9]+$")
  message(FATAL_ERROR "LAPLACE_MCP_PORT: '${LAPLACE_MCP_PORT}' is not a port")
endif()
configure_file(native/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/lpm_config.h @ONLY)

# tree-sitter's runtime at the manifest's pin, as the engine builds it.
add_library(ts_runtime STATIC ${LAPLACE_TREESITTER}/lib/src/lib.c)
target_include_directories(ts_runtime PUBLIC ${LAPLACE_TREESITTER}/lib/include PRIVATE ${LAPLACE_TREESITTER}/lib/src ${LAPLACE_TREESITTER}/lib/src/wasm)
target_compile_options(ts_runtime PRIVATE -w -O3 -fPIC)

# liblpm: the in-process client (check, text, admit, index, search); laplace_mcp.py loads it.
add_library(lpm_lib SHARED native/lpm.c)
set_target_properties(lpm_lib PROPERTIES OUTPUT_NAME lpm)
target_include_directories(lpm_lib PUBLIC native PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_NATIVE}/src)
target_compile_options(lpm_lib PRIVATE ${LP_C_FLAGS})
target_link_libraries(lpm_lib PRIVATE laplace_text laplace_static lp_crmath BLAKE3::blake3 ts_runtime dl ${LP_SYSLIBS})

# lpm: the commands and the service (serve).
add_executable(lpm_program native/main.c native/api.c native/db_bridge.c ${LAPLACE_ENGINE_SRC}/read.c ${LAPLACE_ENGINE_SRC}/pg.c ${LAPLACE_ENGINE_SRC}/walk.c)
set_target_properties(lpm_program PROPERTIES OUTPUT_NAME lpm)
target_include_directories(lpm_program PRIVATE native ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_ENGINE_SRC} ${LAPLACE_NATIVE}/src
                           ${LAPLACE_BLAKE3_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
target_link_directories(lpm_program PRIVATE ${LAPLACE_PG_DIR}/lib)
target_compile_options(lpm_program PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
target_link_libraries(lpm_program PRIVATE lpm_lib laplace_text laplace_static lp_crmath BLAKE3::blake3 ts_runtime pq OpenMP::OpenMP_C dl ${LP_SYSLIBS})
set_target_properties(lpm_lib lpm_program PROPERTIES
  BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib"
  INSTALL_RPATH "\$ORIGIN/../lib;${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib")

enable_testing()
add_test(NAME check COMMAND lpm_program check)

include(GNUInstallDirs)
install(TARGETS lpm_program RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS lpm_lib LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
