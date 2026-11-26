cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME dax_sql)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# QuickJS (quickjs-ng), the JavaScript engine that runs dax-sql's compiler. Its
# C sources are compiled into the extension with the flags its own build gives
# them.
set(QJS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs-ng)
set(QJS_SOURCES ${QJS_DIR}/dtoa.c ${QJS_DIR}/libregexp.c
                ${QJS_DIR}/libunicode.c ${QJS_DIR}/quickjs.c)
include_directories(${QJS_DIR})
if(MSVC)
  set(QJS_OPTIONS /std:c11 /experimental:c11atomics /W0)
  set(QJS_DEFINES WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
else()
  set(QJS_OPTIONS -std=gnu11 -funsigned-char -w)
  set(QJS_DEFINES _GNU_SOURCE)
endif()
set_source_files_properties(
  ${QJS_SOURCES} PROPERTIES COMPILE_OPTIONS "${QJS_OPTIONS}" COMPILE_DEFINITIONS
                                                             "${QJS_DEFINES}")

# dax-sql's compiler and the glue, embedded as their text.
include(cmake/embed_js.cmake)
dax_sql_embed_js(${CMAKE_CURRENT_BINARY_DIR}/dax_sql_js_modules.hpp)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(EXTENSION_SOURCES src/dax_sql_extension.cpp src/js_engine.cpp
                      src/onelake_fs.cpp ${QJS_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
