cmake_minimum_required(VERSION 3.16)
project(layout_harness CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(layout_harness
  main.cpp
  ${REPO_ROOT}/lib/Aozora/AozoraStrip.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(layout_harness PRIVATE
  ${REPO_ROOT}/lib/Aozora
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/activities/reader
)
