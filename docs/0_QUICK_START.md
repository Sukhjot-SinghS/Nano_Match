D
# 🚀 Quick Start & Build Guide

This project is built for native Linux environments. It relies on OS-level system calls (`mmap`) and aggressive hardware-specific compiler optimizations (AVX2 SIMD, LTO) for maximum throughput. 

---

### ⚠️ Critical Note for WSL Users (Windows)
If you are using Windows Subsystem for Linux (WSL2), you **MUST** clone and build this repository inside the native Linux filesystem (e.g., `~/Nano_Match`). 
Do **NOT** run this from a mounted Windows drive (e.g., `/mnt/c/` or `/mnt/d/`). The WSL 9P network file-sharing protocol will severely bottleneck the zero-copy `mmap` parser and artificially cap throughput.

### 📦 Prerequisites
* **OS:** **Linux (Ubuntu/Debian recommended)**. 
* **Compiler:** GCC/G++ (Requires C++17 support or higher).
* **Build System:** CMake and Make.
* **Libraries:** Google Benchmark (`sudo apt-get install libbenchmark-dev`).

---

### 1. Build the Entire Suite (Release Mode)
Because this engine relies on cache-line padding and SIMD hardware vectorization, you **must** compile using our provided CMake configuration to trigger the `-O3 -march=native -flto` flags.

~~~bash
# [Current Directory: Any]

# 1. Clone the repository into native Linux space and enter the root directory
cd ~
git clone https://github.com/Sukhjot-SinghS/Nano_Match.git
cd Nano_Match

# 2. Create the build directory
mkdir build && cd build

# 3. Configure CMake for Release build (crucial for HFT performance)
cmake -DCMAKE_BUILD_TYPE=Release ..

# 4. Compile the engine, generator, and benchmarks utilizing all CPU cores
make -j $(nproc)
~~~

---

### 2. Generate the Order Book Datasets
Instead of relying on slow Python scripts, this repository includes a pure C++ $O(1)$ order generator. It creates highly optimized 4-column datasets (`OrderID, Side, Price, Qty`) that strip out timestamp bloat to mimic true HFT binary network ingestion.

~~~bash
# [Current Directory: ~/Nano_Match/build]

# Generate all three 5-million row market scenario datasets
./generator
~~~
*(Note: This creates three CSV files in the `data/` directory: `dataset_A_realistic.csv`, `dataset_B_pathological.csv`, and `dataset_C_dense_equilibrium.csv`)*

---

### 3. Run the End-to-End Engine
Test the raw ingestion, matching throughput, and concurrent lock-free logging using the zero-copy `mmap` pipeline.

~~~bash
# [Current Directory: ~/Nano_Match/build]
cd ..

# 2. Execute against the realistic dataset
./build/trading_server data/dataset_A_realistic.csv

# 3. Execute against the pathological dataset
./build/trading_server data/dataset_B_pathological.csv

# 4. Execute against the dense equilibrium dataset
./build/trading_server data/dataset_C_dense_equilibrium.csv
~~~

**🚀 Expected Output Note:** Throughput varies dramatically based on order book density and market structure (Tested natively on an Intel Core i5-12450HX, 12th Gen Alder Lake):
~~~
*(Note : The values that you get ,will most probably differ from the values written below , proofs  )
~~~
- **Cancellation Storm (Dataset A):** **~41.32 Million events/sec** (90% cancel rate, deep book, constant execution)
- **Pathological (Dataset B):** **~15.15 Million events/sec** (extreme price swings, worst-case empty array scanning)
- **Dense Equilibrium (Dataset C):** **~40.00 Million events/sec** (tight spreads, 20% cancels, continuous matching)

👉 *See [docs/2_THROUGHPUT_E2E.md](docs/2_THROUGHPUT_E2E.md) for a full mathematical breakdown of how these specific datasets affect L1 cache speeds.*

---

### 4. Run the Micro-Benchmark Suite
To verify the L1-cache optimizations and view the exact nanosecond tail latencies of the core matching loop (isolated from file I/O), run the Google Benchmark harness.

~~~bash
# [Current Directory: ~/Nano_Match]

# Execute the benchmark suite
./build/engine_bench
~~~

**⚠️ Note on Google Benchmark Warnings**

If you installed Google Benchmark via a Linux package manager (e.g., `apt`), you may see this warning at the top of the benchmark output:

> `***WARNING*** Library was built as DEBUG. Timings may be affected.`

**What this means:** The benchmarking harness itself was compiled in Debug mode, introducing minimal overhead (a few nanoseconds per iteration). Your actual matching engine code is still compiled with `-O3 -march=native -flto` and runs at full speed.

**Why we can still trust the results:**
* Relative performance comparisons (custom engine vs. STL baseline) are unaffected because both are measured with the same harness. 
* Speedup multipliers remain accurate. 
* Absolute latency numbers may have a tiny upward bias, meaning the engine is actually *faster* than reported.

For production‑grade absolute measurements, rebuild Google Benchmark from source with `-DCMAKE_BUILD_TYPE=Release`. For this project's comparative analysis, the warning is **safe to ignore**.