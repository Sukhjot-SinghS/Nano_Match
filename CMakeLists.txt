cmake_minimum_required(VERSION 3.10)
project(NanoMatch CXX)

# Force Modern C++17 and Extreme Hardware Optimizations
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -O3: Maximum speed
# -march=native: Optimizes the binary for your specific CPU architecture
# -Wall -Wextra: Turns on all warnings to catch bugs
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -Wall -Wextra")

# Tell CMake where to look for header files (if you create an include/ folder later)
include_directories(${CMAKE_SOURCE_DIR}/include)

# ==========================================
# Target 1: The Main Trading Server
# ==========================================
add_executable(trading_server src/nano.cpp)
# Link the pthread library for multithreading
target_link_libraries(trading_server pthread)



add_executable(engine_bench benchmarks/benchmark.cpp)
# Link both Google Benchmark and multithreading libraries
target_link_libraries(engine_bench benchmark pthread)