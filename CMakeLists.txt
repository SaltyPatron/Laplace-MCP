cmake_minimum_required(VERSION 3.24)
project(LaplaceMCP VERSION 0.1.0 LANGUAGES C CXX ASM)

# Laplace-MCP: the HTTP service in front of Laplace. Built as the engine is: Laplace-Native as part of it, under the
# floating-point contract (Laplace-Operations toolchain) with laplace-math answering every libm call, so the entity
# this process names and the coordinate it computes are the ones every other Laplace build computes. The engine's
# reader (read.c, pg.c) is compiled in from the engine's source beside this repository.
set(CMAKE_C_STANDARD 17)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LaplaceNative.cmake")
if(NOT TARGET laplace_static)
  add_subdirectory(${LAPLACE_NATIVE} native-lib EXCLUDE_FROM_ALL)
endif()
include(${LAPLACE_NATIVE}/cmake/LaplacePaths.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceFlags.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceWindowsUtf8.cmake)
find_package(OpenMP REQUIRED)

laplace_path(LAPLACE_ENGINE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Engine/src" "Laplace-Engine's sources: the reader this service links")
include("${LAPLACE_ENGINE_SRC}/../cmake/EngineReader.cmake")
laplace_define_engine_reader("${LAPLACE_ENGINE_SRC}")

# Connections belong to the installation, never the build cache or shipped binary.
unset(LAPLACE_CONNINFO CACHE)
set(LAPLACE_CONNINFO "")
laplace_path(LAPLACE_BUILD "/repos/build" "build trees")
option(LAPLACE_BUNDLE_ENGINE "Build the Engine and MCP in one Native dependency graph" OFF)
if(LAPLACE_BUNDLE_ENGINE)
  set(LAPLACE_ENGINE "${CMAKE_CURRENT_BINARY_DIR}/engine/laplace${CMAKE_EXECUTABLE_SUFFIX}")
elseif(WIN32)
  set(_default_engine "${LAPLACE_BUILD}/Laplace-Engine/icx-release/laplace.exe")
else()
  set(_default_engine "${LAPLACE_BUILD}/Laplace-Engine/icx-release/laplace")
endif()
if(NOT LAPLACE_BUNDLE_ENGINE)
  laplace_path(LAPLACE_ENGINE "${_default_engine}" "the engine program the routes run")
endif()
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

if(LAPLACE_BUNDLE_ENGINE AND NOT TARGET laplace_engine)
  add_subdirectory("${LAPLACE_ENGINE_SRC}/.." engine)
endif()

# liblpm: the in-process client (check, text, admit, index, search); laplace_mcp.py loads it.
add_library(lpm_lib SHARED native/lpm.c)
set_target_properties(lpm_lib PROPERTIES OUTPUT_NAME lpm WINDOWS_EXPORT_ALL_SYMBOLS ON)
# Windows: the DLL's import library and export file must not share lpm.lib/lpm.exp with lpm.exe.
if(WIN32)
  set_target_properties(lpm_lib PROPERTIES ARCHIVE_OUTPUT_NAME lpm_import)
endif()
target_include_directories(lpm_lib PUBLIC native PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_NATIVE}/src)
target_compile_options(lpm_lib PRIVATE ${LP_C_FLAGS})
target_link_libraries(lpm_lib PRIVATE laplace_engine_os laplace_text laplace_static lp_crmath BLAKE3::blake3 ts_runtime ${LP_SYSLIBS})

# lpm: the commands and the service (serve).
add_executable(lpm_program native/main.c native/api.c native/semantic.c native/db_bridge.c)
laplace_utf8_executable(lpm_program)
set_target_properties(lpm_program PROPERTIES OUTPUT_NAME lpm)
target_include_directories(lpm_program PRIVATE native ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_NATIVE}/src
                           ${LAPLACE_BLAKE3_DIR} ${LAPLACE_ICU_DIR}/include)
target_compile_options(lpm_program PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
if(WIN32)
  target_sources(lpm_program PRIVATE native/lpm.c)
  target_link_libraries(lpm_program PRIVATE laplace_engine_reader laplace_text ts_runtime ws2_32 ${LP_SYSLIBS})
else()
  target_link_libraries(lpm_program PRIVATE lpm_lib laplace_engine_reader laplace_text ts_runtime ${LP_SYSLIBS})
  set_target_properties(lpm_lib lpm_program PROPERTIES
    BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib"
    INSTALL_RPATH "\$ORIGIN/../lib;${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib")
endif()

enable_testing()
add_test(NAME check COMMAND lpm_program check)

include(GNUInstallDirs)
install(TARGETS lpm_program RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS lpm_lib LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
