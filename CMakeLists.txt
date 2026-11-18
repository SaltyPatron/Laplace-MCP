cmake_minimum_required(VERSION 3.24)
project(LaplaceMCP VERSION 0.1.0 LANGUAGES C CXX ASM)

# Laplace-MCP: the HTTP service in front of Laplace. Built as the engine is: against the installed Laplace-Native
# package (find_package(LaplaceNative); CMAKE_PREFIX_PATH or LaplaceNative_DIR), under the floating-point contract it
# carries, with laplace-math answering every libm call, so the entity this process names and the coordinate it
# computes are the ones every other Laplace build computes. The engine's reader (reader.h: read.c, pg.c) is linked from
# the installed Laplace-Engine package (find_package(LaplaceEngine); LaplaceEngine_DIR), which also names the program
# the routes run; no Engine source is compiled here.
set(CMAKE_C_STANDARD 17)
find_package(LaplaceNative CONFIG REQUIRED)
find_package(OpenMP REQUIRED)

find_package(LaplaceEngine CONFIG REQUIRED)

# Connections belong to the installation, never the build cache or shipped binary.
unset(LAPLACE_CONNINFO CACHE)
set(LAPLACE_CONNINFO "")
laplace_path(LAPLACE_BUILD "/repos/build" "build trees")
laplace_path(LAPLACE_ENGINE "${LaplaceEngine_PROGRAM}" "the engine program the routes run")
option(LAPLACE_INSTALL_ENGINE "Install the Engine program from its package beside lpm: a release holds both" OFF)
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

# liblpm: the in-process client (check, id, text): a text's entity and constituent tree, from the Native decomposer.
set(LPM_CORE_SOURCES native/lpm.c)
add_library(lpm_lib SHARED ${LPM_CORE_SOURCES})
set_target_properties(lpm_lib PROPERTIES OUTPUT_NAME lpm WINDOWS_EXPORT_ALL_SYMBOLS ON)
# Windows: the DLL's import library and export file must not share lpm.lib/lpm.exp with lpm.exe.
if(WIN32)
  set_target_properties(lpm_lib PROPERTIES ARCHIVE_OUTPUT_NAME lpm_import)
endif()
target_include_directories(lpm_lib PUBLIC native PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(lpm_lib PRIVATE ${LP_C_FLAGS})
target_link_libraries(lpm_lib PRIVATE Laplace::text Laplace::laplace_static Laplace::math BLAKE3::blake3 ${LP_SYSLIBS})

# lpm: the commands and the service (serve).
add_executable(lpm_program native/main.c native/api.c native/api_serve.c native/api_mcp.c native/api_substrate.c native/api_search.c native/api_code_db.c native/api_code_graph.c native/api_catalog.c native/api_chess.c native/api_chess_games.c native/db_bridge.c)
laplace_utf8_executable(lpm_program)
set_target_properties(lpm_program PROPERTIES OUTPUT_NAME lpm)
target_include_directories(lpm_program PRIVATE native ${CMAKE_CURRENT_BINARY_DIR}
                           ${LAPLACE_ICU_DIR}/include)
target_compile_options(lpm_program PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
if(WIN32)
  target_sources(lpm_program PRIVATE ${LPM_CORE_SOURCES})
  target_link_libraries(lpm_program PRIVATE Laplace::engine_reader Laplace::text Laplace::ts_runtime ws2_32 ${LP_SYSLIBS})
else()
  target_link_libraries(lpm_program PRIVATE lpm_lib Laplace::engine_reader Laplace::text Laplace::ts_runtime ${LP_SYSLIBS})
  set_target_properties(lpm_lib lpm_program PROPERTIES
    BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib"
    INSTALL_RPATH "\$ORIGIN/../lib;${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib")
endif()

enable_testing()
add_test(NAME check COMMAND lpm_program check)

include(GNUInstallDirs)
install(TARGETS lpm_program RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS lpm_lib LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(LAPLACE_INSTALL_ENGINE)
  install(PROGRAMS "${LaplaceEngine_PROGRAM}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
