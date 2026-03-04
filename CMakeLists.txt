cmake_minimum_required(VERSION 3.15)

if(NOT UNIX)
  message(FATAL_ERROR "unix only")
endif()

project(shrill C)

set(CMAKE_C_STANDARD 23)

option(NATIVE_BUILD "build with -march=native -mtune=native" OFF)
option(STATIC_BUILD "build a static executable" OFF)
option(ENABLE_LTO "enable link-time optimization" OFF)

add_executable(shrill shrill.c)

if(NATIVE_BUILD)
  target_compile_options(shrill PRIVATE -march=native -mtune=native)
endif()

if(STATIC_BUILD)
  target_link_options(shrill PRIVATE -static)
endif()

if(ENABLE_LTO)
  set_property(TARGET shrill PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(JSONC REQUIRED json-c)

target_include_directories(shrill PRIVATE ${JSONC_INCLUDE_DIRS})
target_link_directories(shrill PRIVATE ${JSONC_LIBRARY_DIRS})
target_link_libraries(shrill PRIVATE ${JSONC_LIBRARIES})

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_custom_command(TARGET shrill POST_BUILD
    COMMAND strip --strip-unneeded $<TARGET_FILE:shrill>
    COMMENT "stripping executable"
  )
endif()

install(TARGETS shrill DESTINATION bin)
