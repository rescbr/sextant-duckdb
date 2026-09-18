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
target_link_libraries(${EXTENSION_NAME} PRIVATE "${SEXTANT_ENGINE_DIR}/build-x86/libsextant_c.a" "${SEXTANT_ENGINE_DIR}/build-x86/libsextant.a")
target_link_libraries(${LOADABLE_EXTENSION_NAME} PRIVATE "${SEXTANT_ENGINE_DIR}/build-x86/libsextant_c.a" "${SEXTANT_ENGINE_DIR}/build-x86/libsextant.a")

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
