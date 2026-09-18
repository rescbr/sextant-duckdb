cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME sextant)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/sextant_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Sextant engine: C API seam (sextant_c.h) over the prebuilt static lib.
# Local dev: -DSEXTANT_ENGINE_DIR=<path to sextant-engine checkout>.
set(SEXTANT_ENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}/../sextant-engine" CACHE PATH "sextant-engine checkout")
target_include_directories(${EXTENSION_NAME} PRIVATE "${SEXTANT_ENGINE_DIR}/include")
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE "${SEXTANT_ENGINE_DIR}/include")
# The engine's spdlog resolves via pkg-config (compiled shared lib on
# Ubuntu; header-only subproject elsewhere). Mirror whatever pkg-config
# says so the extension TUs see the same spdlog ABI as libsextant.a.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
	execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags spdlog
		OUTPUT_VARIABLE SPDLOG_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE SPDLOG_RC)
	execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --libs spdlog
		OUTPUT_VARIABLE SPDLOG_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE SPDLOG_RC2)
endif()
if(SPDLOG_RC EQUAL 0 AND SPDLOG_RC2 EQUAL 0)
	separate_arguments(SPDLOG_CFLAGS)
	separate_arguments(SPDLOG_LIBS)
	target_compile_options(${EXTENSION_NAME} PRIVATE ${SPDLOG_CFLAGS})
	target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE ${SPDLOG_CFLAGS})
else()
	set(SPDLOG_LIBS "")
	message(STATUS "spdlog not found via pkg-config (header-only mode?)")
endif()
set(SEXTANT_LIBS
	"${SEXTANT_ENGINE_DIR}/build-x86/libsextant_c.a"
	"${SEXTANT_ENGINE_DIR}/build-x86/libsextant.a"
	"${SEXTANT_ENGINE_DIR}/build-x86/libsextant_sources.a"
	"${SEXTANT_ENGINE_DIR}/build-x86/subprojects/carquet/libcarquet.a"
	"${SEXTANT_ENGINE_DIR}/build-x86/subprojects/nsync-5b420ef6b16532a4fad4be1fde8e483b6b7bd8f4/libnsync.a"
	"${SEXTANT_ENGINE_DIR}/build-x86/subprojects/cpptrace-91b6b78e408a8b1c0b7146c9034a03156c082da2/libcpptrace.a"  # cpptrace-91b6b78e408a8b1c0b7146c9034a03156c082da2
	${SPDLOG_LIBS})
target_link_libraries(${EXTENSION_NAME} ${SEXTANT_LIBS})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${SEXTANT_LIBS})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
