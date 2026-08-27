cmake_minimum_required(VERSION 3.20)
project(itch_orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" HAVE_MARCH_NATIVE)

add_executable(itch_replay src/main.cpp)
target_include_directories(itch_replay PRIVATE src)
target_compile_options(itch_replay PRIVATE -Wall -Wextra)

if(HAVE_MARCH_NATIVE)
    target_compile_options(itch_replay PRIVATE $<$<CONFIG:Release>:-march=native>)
endif()
