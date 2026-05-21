# 🚀 Quick Start & Build Guide

This project is built for native Linux environments. It relies on OS-level system calls (`mmap`) and hardware-specific compiler optimizations for maximum throughput. 

### Prerequisites
* **OS:** Linux (Ubuntu/Debian recommended) or WSL2 on Windows.
* **Compiler:** GCC/G++ (Requires C++17 support or higher).
* **Build System:** CMake and Make.
* **Libraries:** Google Benchmark (`sudo apt-get install libbenchmark-dev`).

---

### 1. Build the Project (Release Mode)
To unleash the sub-microsecond latency, you **must** compile the engine with maximum hardware optimizations (`-O3`) and disable debugging overhead. 

~~~bash
# [Current Directory: Any]

# 1. Clone the repository and enter the root directory
git clone https://github.com/Sukhjot-SinghS/Nano_Match.git
cd Nano_Match

# 2. Create the build directory
mkdir build && cd build

# 3. Configure CMake for Release build (crucial for HFT performance)
cmake -DCMAKE_BUILD_TYPE=Release ..

# 4. Compile the engine and benchmarks utilizing all CPU cores
make -j $(nproc)
~~~

---

### 2. Generate the Order Book Datasets
Instead of relying on slow Python scripts, this repository includes a pure C++ $O(1)$ order generator to forge multi-million row datasets in milliseconds.

~~~bash
# [Current Directory: Nano_Match/data]

# 2. Compile the generator
g++ -O3 generate_data.cpp -o generator

# 3. Generate all three market scenario CSV datasets
./generator
~~~
*(Note: This will generate three 5-million row CSV files in the `data/` directory: `dataset_A_realistic.csv`, `dataset_B_pathological.csv`, and `dataset_C_dense_equilibrium.csv`)*

---

### 3. Run the End-to-End Engine
Test the raw ingestion and matching throughput using the zero-copy `mmap` pipeline.

~~~bash
# [Current Directory: Nano_Match/data]

# 1. Return to the project root directory
cd ..

# 2. Execute the engine against the realistic dataset
./build/trading_server data/dataset_A_realistic.csv

# 3. Execute the engine against the pathological dataset
./build/trading_server data/dataset_B_pathological.csv

# 4. Execute the engine against the dense equilibrium dataset
./build/trading_server data/dataset_C_dense_equilibrium.csv

~~~
**Expected Output Note:** Throughput varies dramatically based on order book density and market structure:
- **Dense Equilibrium (Dataset C):** ~24.04 Million events/sec (tight spreads, 20% cancels, continuous matching)
- **Pathological (Dataset B):** ~9.54 Million events/sec (extreme price swings, pure matching, no scans)
- **Cancellation Storm (Dataset A):** ~93,489 events/sec (90% cancel rate, constant array scans, intentional stress test)

👉 *See `docs/2_THROUGHPUT_E2E.md` for a full mathematical breakdown of how these specific datasets affect L1 cache speeds.*

---

### 4. Run the Micro-Benchmark Suite
To verify the L1-cache optimizations and view the exact nanosecond tail latencies, run the Google Benchmark harness.

~~~bash
# [Current Directory: Nano_Match]

# 1. Navigate into the build directory
cd build

# 2. Execute the benchmark suite
./engine_bench
~~~

**⚠️ Note on Google Benchmark Warnings**

If you installed Google Benchmark via a Linux package manager (e.g., `apt`), you may see this warning at the top of the benchmark output:

> `***WARNING*** Library was built as DEBUG. Timings may be affected.`

**What this means:** The benchmarking harness itself was compiled in Debug mode, introducing minimal overhead (a few nanoseconds per iteration). Your actual matching engine code is still compiled with `-O3 -march=native` and runs at full speed.

**Why we can still trust the results:**
* Relative performance comparisons (custom engine vs. STL baseline) are unaffected because both are measured with the same harness.  
* Speedup multipliers (e.g., 8.7x faster) remain accurate.  
* Absolute latency numbers (e.g., 10.7 ns median) may have a tiny upward bias, meaning the engine is actually *faster* than reported.

For production‑grade absolute measurements, rebuild Google Benchmark from source with `-DCMAKE_BUILD_TYPE=Release`. For this project's comparative analysis, the warning is **safe to ignore**.