cmake_minimum_required(VERSION 3.10)
project(NanoMatch CXX)

# Force Modern C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==========================================
# GOD-TIER HARDWARE OPTIMIZATIONS
# ==========================================
# -O3: Maximum execution speed
# -march=native: Hardware vectorization (AVX-512) for the zero-copy parser
# -flto: Link-Time Optimization to aggressively inline functions across files
# -Wall -Wextra: Strict warning checks
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto -Wall -Wextra")

# Tell CMake where to look for header files (if you create an include/ folder later)
include_directories(${CMAKE_SOURCE_DIR}/include)

# ==========================================
# Target 1: The Main Trading Server
# ==========================================
add_executable(trading_server src/nano.cpp)
# Link the pthread library for the lock-free background logger
target_link_libraries(trading_server pthread)

# ==========================================
# Target 2: The Google Benchmark Harness
# ==========================================
add_executable(engine_bench benchmarks/benchmark.cpp)
# Link both Google Benchmark and multithreading libraries
target_link_libraries(engine_bench benchmark pthread)

# ==========================================
# Target 3: The 4-Column Data Generator
# ==========================================
add_executable(generator data/generate_data.cpp)
# No pthread needed for the synthetic data generator