# ⚡ NanoMatch — Sub-10ns HFT Limit Order Book Engine

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg?style=for-the-badge&logo=c%2B%2B)
![Linux](https://img.shields.io/badge/Platform-Linux-lightgrey.svg?style=for-the-badge&logo=linux)
![CMake](https://img.shields.io/badge/CMake-Build-orange.svg?style=for-the-badge&logo=cmake)
![Latency](https://img.shields.io/badge/Latency-7.36_ns-brightgreen.svg?style=for-the-badge)
![Throughput](https://img.shields.io/badge/Throughput-24M_ops%2Fsec-ff69b4.svg?style=for-the-badge)

> A from-scratch, cache-aware C++ matching engine engineered for High-Frequency Trading. Achieves **7.36 ns median latency** (micro-benchmark with sentinels) and **~24.04 Million events/sec** throughput (dense equilibrium market) by designing data structures to remain L1-cache resident — no STL containers, no kernel allocations, no compromises.

---

## 🏆 Headline Results

| Metric                                   | NanoMatch                   | STL Baseline              | Improvement      |
|:------------------------------------------|:----------------------------|:--------------------------|:----------------|
| **Median Latency p50 (Crossing)**         | 7.36 ns                     | 65.2 ns                   | **8.8× faster** |
| **p90 Tail Latency (Crossing)**           | 7.97 ns                     | 76.5 ns                   | **9.6× faster** |
| **p99 Tail Latency (Crossing)**           | 8.67 ns                     | 106.0 ns                  | **12.2× faster**|
| **Micro-Benchmark Throughput**            | ~273.4 Million ops/sec      | ~30.7 Million ops/sec     | **8.9× higher** |
| **Peak E2E Throughput (Dataset C — Dense)** | **~24.04 Million events/sec** | —                        | Liquid market   |
| **E2E Average Latency (Dataset C)**       | **41.60 ns/event**          | —                         | Over 5M orders  |

**Clarification:**  
- The 7.36 ns figure is from the `BM_HFT_100pct_With_Sentinels` micro-benchmark (isolated order crossing).
- The ~24M events/sec is from the end-to-end pipeline ingesting Dataset C via `mmap` (dense equilibrium with 20% cancellations, tight 5-tick spreads).
- The realistic market test (`BM_HFT_Realistic_Market`) in micro-benchmarks shows **40.5 ns p50** with 100,000 resting orders.  
See [`docs/1_LATENCY_PROFILE.md`](docs/1_LATENCY_PROFILE.md) for the complete breakdown.

---

## 📂 Repository Structure

```
Nano_Match/
├── CMakeLists.txt
├── src/
│   └── nano.cpp                  # Core matching engine (alignas(64), mmap, SPSC ring buffer)
│
├── benchmarks/
│   └── benchmark.cpp             # Google Benchmark harness (6 scenarios)
│
├── data/
│   └── generate_data.cpp         # O(1) C++ synthetic data generator (generates datasets locally)
│
├── docs/
│   ├── 0_QUICK_START.md          # Build, generate data, run benchmarks
│   ├── 1_LATENCY_PROFILE.md      # Nanosecond latency breakdown & benchmark analysis
│   ├── 2_THROUGHPUT_E2E.md       # End-to-end mmap pipeline & 3-dataset throughput
│   ├── 3_CACHE_ARCHITECTURE.md   # Hardware-sympathy techniques & flame graph analysis
│   └── assets/
│       ├── benchmark_latency.png     # Google Benchmark terminal output
│       ├── nano_flamegraph.png       # NanoMatch: flat flame graph (zero malloc)
│       ├── stl_flamegraph.png        # STL baseline: malloc/free towers
│       ├── throughput_A.png          # Dataset A dashboard
│       ├── throughput_B.png          # Dataset B dashboard
│       └── throughput_C.png          # Dataset C dashboard
```

> **Note:**  
> The CSV datasets (`dataset_A_realistic.csv`, `dataset_B_pathological.csv`, `dataset_C_dense_equilibrium.csv`) are **NOT** included in the repository.  
> They are generated locally by running `generate_data.cpp` (see [`docs/0_QUICK_START.md`](docs/0_QUICK_START.md)).  
> The generator creates 5M-row files in milliseconds via O(1) mechanics.

---

## 🧭 Navigation Guide

| What you want to see                              | Where to look                                   |
|:--------------------------------------------------|:------------------------------------------------|
| Build & run the project                           | [`docs/0_QUICK_START.md`](docs/0_QUICK_START.md)      |
| Nanosecond latency numbers & benchmark explanation| [`docs/1_LATENCY_PROFILE.md`](docs/1_LATENCY_PROFILE.md) |
| End-to-end throughput across 3 market scenarios   | [`docs/2_THROUGHPUT_E2E.md`](docs/2_THROUGHPUT_E2E.md)   |
| Hardware-sympathy internals & flame graphs        | [`docs/3_CACHE_ARCHITECTURE.md`](docs/3_CACHE_ARCHITECTURE.md) |

---

## 🔍 What Is This?

Standard algorithmic trading systems built on STL containers suffer from fundamental architectural problems:

- **`std::map<price, std::deque<Order>>`**: O(log N) tree navigation for every price lookup; every insert triggers at least one Red-Black tree rebalance.
- **`std::deque<Order>`** at each price level performs dynamic heap allocation (`malloc`/`new`) per order, causing kernel mode traps.
- Each price level erase triggers tree rebalancing and deque deallocation — multiple kernel round-trips per trade.

For ultra-low-latency trading, these are dealbreakers: in HFT, every nanosecond counts.

**NanoMatch** is a from-scratch Limit Order Book engine that treats the CPU's L1 cache as primary memory. Instead of trees/pointers & allocations, it uses:

- Flat contiguous price array: O(1) lookup via `bids_head[price]` and `asks_head[price]` — always array, never tree.
- Pre-allocated `OrderPool` (single `std::vector<Order>`): eliminates malloc/new during the order lifecycle.
- Intrusive doubly-linked lists using array indices: no heap allocation per node.
- Every `Order` is `alignas(64)`: lives perfectly within one cache line.

Hot-path execution is user-space only: no malloc, no kernel allocation, no dynamic tree.  
**The result:** as proved by `perf` flame graphs and Google Benchmark, a median matching latency of **7.36 ns** — **8.8× faster** than STL.

---

## 🔬 Benchmark Deep-Dive

The benchmark suite ([`benchmarks/benchmark.cpp`](benchmarks/benchmark.cpp)) tests six core scenarios.  
**Key comparison:**

### The Primary Comparison: NanoMatch vs STL

Both engines are given the same liquid-market order flow:

- **STL Baseline** (`BM_Naive_STL_Crossing`): `std::map<price, std::deque<Order>>` — O(log N) tree search, Red-Black rebalancing, dynamic allocations/deletions for each operation.
- **NanoMatch** (`BM_HFT_100pct_With_Sentinels`): Flat array & pooled orders; no dynamic allocations, no tree ops.

| Test Name                       | p50     | p90     | p99     | Data Structure            |
|----------------------------------|---------|---------|---------|--------------------------|
| BM_Naive_STL_Crossing           | 65.2 ns | 76.5 ns | 106.0 ns| std::map + std::deque    |
| BM_HFT_100pct_With_Sentinels     | 7.36 ns | 7.97 ns | 8.67 ns | flat array + pool        |

**Results:** 8.8× faster median, 12.2× tighter p99 tail.

#### Why Sentinels Matter

- If the order book is *empty* (every match eliminates the price level), the engine sometimes must linearly scan up to 100,000 slots for the next live best_bid/best_ask: O(N) scan, p50 spikes to ~104,726 ns.
- **Real HFT firms always place "sentinel" orders 1-2 ticks away**: this keeps the array region dense, letting the engine stay strictly within the L1 cache.
- The benchmark with sentinels returns the engine to 7.36 ns.  
  This is not a hack — it's how actual markets operate under normal liquidity.

**Trade-off:** If the market is deeply illiquid (huge empty gaps), performance degrades to O(N) level scan.  
This is a deliberate architectural choice — not an oversight.

---

## 🚄 End-to-End Throughput: Three Market Scenarios

The E2E engine ingests 5,000,000-row CSV files via zero-copy `mmap`.  
**Three datasets expose boundaries:**

### Dataset A — Cancellation Storm (Stress Test)
- **Throughput:** ~93,489 events/sec | 5M orders in 53.4s
- 90% of all resting orders are canceled, book is perpetually sparse — engine forced into millions of worst-case O(N) scans.
  
  *Not realistic — purposefully pathological scenario. Still, engine processes >1 event per 10.7µs.*

### Dataset B — Pathological Matching (Illiquid Extremes)
- **Throughput:** ~9.54 Million events/sec | 5M orders in 524ms
- All orders cross at alternating price extremes; order book never accumulates.

### Dataset C — Dense Equilibrium (Liquid Market)
- **Throughput:** ~24.04 Million events/sec | 5M orders in ~208 ms  
- **Average Latency:** 41.60 ns/event
- Tight live spread, realistic cancellation, continuous order flow. `best_bid` and `best_ask` step only along adjacent filled slots — peak O(1) cacheline speed.

See [`docs/2_THROUGHPUT_E2E.md`](docs/2_THROUGHPUT_E2E.md) for dashboard & math breakdown.

---

## 🧠 Hardware-Sympathy Techniques

| Technique                                                    | Hardware Benefit                                        |
|:-------------------------------------------------------------|:--------------------------------------------------------|
| Custom `OrderPool` (contiguous array)                        | Zero kernel `malloc`, prefetch stays in L1/L2           |
| `alignas(64)` / `alignas(32)` order structs                  | Avoids cache-line straddling                            |
| Flat price array (`bids_head[price]`)                        | O(1) lookup, no pointer chase, no tree                  |
| Separate `alignas(64)` ring buffer indices                   | No false sharing: clean multi-core SPSC                 |
| Bitwise AND for ring buffer wrapping (`& (capacity-1)`)      | Single cycle mod wrap                                   |
| `[[likely]]` / `[[unlikely]]` branch hints                   | Hot path inlines, cold moved to slow binary sections    |
| `madvise(MADV_SEQUENTIAL)` on mmap                           | Kernel prefetches CSV pipeline                          |
| `-O3 -march=native` compile                                  | Full AVX/AVX2 SIMD, vectorized parsing                  |
| Direct-mapped `order_map` vector (OrderID = array index)     | O(1) cancellation; perfect hash                         |
| SPSC lock-free ring buffer for logger                        | No mutex/futex on hot-path logging                      |

> In flame graphs: STL engine shows heavy kernel malloc/free & tree ops; NanoMatch is a flat, allocation-free band.

See [`docs/3_CACHE_ARCHITECTURE.md`](docs/3_CACHE_ARCHITECTURE.md) for flame graphs and technique breakdown.

---

## 🖥️ Hardware & Test Environment

To ensure reliable, reproducible nanosecond measurements, all benchmarks were run on:

| Component                  | Specification                                              |
|:---------------------------|:----------------------------------------------------------|
| **Processor (CPU)**        | Intel Core i5-12450HX (12th Gen Alder Lake, 8C/12T)       |
| **Architecture**           | x86_64 (`-march=native` with AVX2/SIMD)                   |
| **CPU Cache Hierarchy**    | 32KB L1d / 48KB L1i per P-core, 8MB L2, 12MB L3           |
| **Memory (RAM)**           | 16 GB DDR5 @ 4800 MT/s                                    |
| **Operating System**       | Windows 11 + WSL2 (Ubuntu 24.04.1 LTS)                    |
| **Compiler**               | GCC/G++ 13.3.0                                            |
| **Profiling Tools**        | Google Benchmark, Linux `perf`                            |
| **Flags Used**             | `-O3 -march=native -Wall -Wextra -fno-omit-frame-pointer` |

---

## 🚀 Quick Start

See **[`docs/0_QUICK_START.md`](docs/0_QUICK_START.md)** for the build guide (prereqs, commands, output, debug tips).

---

## 🛠️ Tech Stack

- **Language:** C++17
- **Build System:** CMake + Make
- **Benchmarking:** Google Benchmark
- **Profiling:** Linux `perf` + Flame Graphs (`-fno-omit-frame-pointer`)
- **OS:** **Linux / WSL2** (Mandatory: requires POSIX `mmap` & system headers)
- **Compiler Flags:** `-O3 -march=native -Wall -Wextra`

---

*Every nanosecond is engineered. Every benchmark tells a story.*
