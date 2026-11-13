cmake_minimum_required(VERSION 3.16)
project(dmrecomp C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB GEN_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/gen/*.c)
file(GLOB RT_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.c)
if(NOT GEN_SRC)
  message(FATAL_ERROR "gen/ is empty: run tools/regen.sh <extracted disc dir> first")
endif()

add_executable(dmrecomp ${RT_SRC} ${GEN_SRC})
target_include_directories(dmrecomp PRIVATE runtime gen)
# generated code is huge; -O1 keeps build times sane while staying fast
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-O1;-fno-strict-aliasing;-w")
target_compile_options(dmrecomp PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing)
target_link_libraries(dmrecomp PRIVATE winmm opengl32 gdi32)
if(MINGW)
  target_link_options(dmrecomp PRIVATE -static -Wl,--stack,16777216)
endif()
