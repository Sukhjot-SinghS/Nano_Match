# ⚡ NanoMatch — Sub-10ns HFT Limit Order Book Engine
 
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg?style=for-the-badge&logo=c%2B%2B)
![Linux](https://img.shields.io/badge/Platform-Linux%20%2F%20WSL2-lightgrey.svg?style=for-the-badge&logo=linux)
![CMake](https://img.shields.io/badge/Build-CMake-orange.svg?style=for-the-badge&logo=cmake)
![Benchmark](https://img.shields.io/badge/Benchmarked-Google%20Benchmark-informational.svg?style=for-the-badge)
 
> A from-scratch, cache-aware C++ matching engine engineered for High-Frequency Trading. Achieves **9.36 ns median latency** on isolated order crossing and **38.4 ns under realistic market load** — by designing every data structure to live inside the CPU's L1 cache. No STL containers. No kernel allocations. No compromises.
 
---
 
## 🏆 Headline Results
 
### Micro-Benchmark: NanoMatch vs STL Baseline
 
> **Important context:** The STL baseline (`NaiveSTLOrderBook`) uses `std::map<price, std::list<Order>>`. `std::list` represents standard node-based heap allocation — one `malloc` per order, pointer-chasing across fragmented memory — which is exactly the allocation pattern NanoMatch is engineered to eliminate. Every speedup ratio below is measured against this real architecture, not a strawman.
 
| Benchmark | Engine | p50 | p90 | p99 | Throughput |
|:---|:---|:---:|:---:|:---:|:---:|
| **Pure Crossing** | STL (`std::map` + `std::list`) | 56.2 ns | 63.6 ns | 67.0 ns | ~32.5M ops/sec |
| **Pure Crossing** | NanoMatch | **9.36 ns** | **11.1 ns** | **12.5 ns** | **~227.4M ops/sec** |
| 🚀 **Speedup** | | **6.0×** | **5.7×** | **5.3×** | **7.0×** |
| | | | | | |
| **Realistic Market** | STL (`std::map` + `std::list`) | 106 ns | 123 ns | 148 ns | ~9.5M ops/sec |
| **Realistic Market** | NanoMatch | **38.4 ns** | **42.7 ns** | **50.3 ns** | **~24.3M ops/sec** |
| 🚀 **Speedup** | | **2.75×** | **2.88×** | **2.94×** | **2.56×** |
 
The realistic market test pre-loads **100,000 resting orders** and bombards both engines with randomized passive/aggressive flow across a 400-tick price window, with a live SPSC logger thread running concurrently. This is the primary result — not the isolated crossing number.
 
**The tail latency story is equally important:** under sustained load, NanoMatch's p99 degrades only **+11.9 ns** above its median (38.4 → 50.3 ns). The STL baseline degrades **+42 ns** (106 → 148 ns) — **3.5× more tail variance**. In HFT, predictability under stress matters as much as median speed.
 
### All Benchmark Results at a Glance
 
| Scenario | p50 | p90 | p99 | Throughput | Notes |
|:---|:---:|:---:|:---:|:---:|:---|
| `BM_HFT_Realistic_Market` ⭐ | 38.4 ns | 42.7 ns | 50.3 ns | ~24.3M ops/sec | Primary result. 100k resting orders, live logger thread |
| `BM_STL_Realistic_Market` | 106 ns | 123 ns | 148 ns | ~9.5M ops/sec | STL baseline under identical load |
| `BM_HFT_100Pct_Crossing` | 9.36 ns | 11.1 ns | 12.5 ns | ~227.4M ops/sec | Isolated hot-path ceiling |
| `BM_Naive_STL_Crossing` | 56.2 ns | 63.6 ns | 67.0 ns | ~32.5M ops/sec | STL baseline, identical task |
| `BM_HFT_Order_Cancellation` | **6.81 ns** | 7.61 ns | 8.38 ns | ~269.0M ops/sec | O(1) via direct-mapped `order_map` |
| `BM_HFT_Sparse_Book_Scan` | 7.40 ns | 9.35 ns | 11.4 ns | ~251.9M ops/sec | Non-zero price region, no penalty |
| `BM_HFT_100pct_With_Sentinels` | 9.11 ns | 11.1 ns | 13.4 ns | ~237.3M ops/sec | Minimal sentinel boundary padding (1 order each side) |
 
> 📊 Raw terminal output: [benchmark_1.png](docs/assets/benchmark_1.png) · [benchmark_2.png](docs/assets/benchmark_2.png)
 
### End-to-End Throughput: 5,000,000 Orders via `mmap`
 
| Dataset | Market Scenario | Throughput | Avg Latency | Total Time |
|:---|:---|:---:|:---:|:---:|
| **Dataset A** | Cancellation Storm (90% cancel rate) | **~41.32M events/sec** | 24.20 ns | 121 ms |
| **Dataset B** | Pathological Extremes (worst case) | **~15.15M events/sec** | 66.00 ns | 330 ms |
| **Dataset C** | Dense Equilibrium (liquid market) | **~40.00M events/sec** | 25.00 ns | 125 ms |
 
---
 
## 🔍 What Is This?
 
Standard algorithmic trading systems built on STL suffer from three structural problems that compound at nanosecond scale:
 
- `std::map<price, std::list<Order>>` requires O(log N) Red-Black tree navigation for every price lookup, plus a heap allocation per node insertion and a tree rebalance on every erase.
- Dynamic memory allocation (`malloc`/`new`) triggers kernel mode traps. Each trap costs tens of microseconds of scheduling overhead — catastrophic on a matching hot path.
- Pointer-chasing through fragmented heap nodes destroys the CPU's hardware prefetcher. Cache misses at 200+ cycles each are the real latency killer, not algorithmic complexity.
**NanoMatch** abandons this model entirely. It treats the CPU's L1 cache as primary memory and the OS as an enemy to be bypassed:
 
- **Flat price array** (`bids_head[price]`, `asks_head[price]`): O(1) lookup via direct array indexing. No tree, no pointer chase, no rebalance.
- **Pre-allocated `OrderPool`**: a single contiguous `std::vector<Order>` allocated at boot. Zero `malloc` or `new` calls on the hot path. Memory recycling is pure user-space via a `free_indices` stack.
- **Intrusive doubly-linked list**: `next_id` and `prev_id` are integer indices stored directly inside each `alignas(64)` `Order` struct. O(1) insertion and removal with no heap allocation per node.
- **Direct-mapped `order_map`**: `order_id` is used as a direct array index. Cancellation resolves to a single array read followed by O(1) pointer arithmetic — no search, no hash collision, no tree traversal.
- **SPSC lock-free ring buffer**: trade logging is offloaded to a background thread via `std::atomic` with `memory_order_release`/`memory_order_acquire` semantics. No mutex, no `futex`, no kernel involvement on the matching hot path.
The result is proved by `perf` flame graphs: the NanoMatch hot path is a flat, allocation-free band. The STL baseline shows `malloc`, `operator new`, `cfree`, and `[libc.so.6]` towers consuming CPU time that should be spent matching orders.
 
See [`docs/3_CACHE_ARCHITECTURE.md`](docs/3_CACHE_ARCHITECTURE.md) for the complete hardware-sympathy breakdown and flame graph analysis.
 
---
 
## 🗺️ Navigation Guide
 
| File | What's Inside | Go Here When… |
|:---|:---|:---|
| [`docs/0_QUICK_START.md`](docs/0_QUICK_START.md) | Build guide, prerequisites, commands, expected output, debug tips | You want to build and run the project |
| [`docs/1_LATENCY_PROFILE.md`](docs/1_LATENCY_PROFILE.md) | Full percentile tables, per-scenario analysis, benchmark methodology | You want the complete nanosecond breakdown |
| [`docs/2_THROUGHPUT_E2E.md`](docs/2_THROUGHPUT_E2E.md) | mmap pipeline architecture, 3-dataset throughput, dataset design rationale | You want to understand the E2E ingestion pipeline |
| [`docs/3_CACHE_ARCHITECTURE.md`](docs/3_CACHE_ARCHITECTURE.md) | Hardware-sympathy techniques, flame graph analysis, struct packing, SPSC design | You want to understand why the engine is built this way |
| [`src/nano.cpp`](src/nano.cpp) | Production engine: `alignas(64)` Order, `OrderPool`, `LimitOrderBook`, SPSC logger, `mmap` parser | You want to read the core engine source |
| [`benchmarks/benchmark.cpp`](benchmarks/benchmark.cpp) | Google Benchmark harness: 7 scenarios, `alignas(32)` Order, `PriceLevel` struct, custom p50/p90/p99 statistics | You want to inspect or reproduce the benchmarks |
| [`data/generate_data.cpp`](data/generate_data.cpp) | Pure C++ O(1) dataset generator (swap-and-pop mechanics), 3 market scenarios | You want to generate the test datasets |
 
---
 
## 🔬 Benchmark Deep-Dive
 
The benchmark suite tests seven scenarios designed to measure different slices of the engine's performance profile. All results use 50 repetitions with custom p50/p90/p99 statistics computed over the repetition distribution — not Google Benchmark's default 1–3 repetitions.
 
### 1. Realistic Market — The Primary Result
 
`BM_HFT_Realistic_Market` vs `BM_STL_Realistic_Market` is the most important comparison in this project. Both engines are pre-loaded with 100,000 resting orders spread across buy side (9,500) and sell side (10,500) before the timing loop starts. During measurement, both receive the same pre-generated random order flow across a 400-tick price window (9,800–10,200) via a Mersenne Twister distribution seeded identically. The NanoMatch engine runs a live SPSC logger consumer thread concurrently throughout.
 
This setup eliminates the three most common microbenchmark cheats: empty book, single price level, and RNG inside the timing loop.
 
| | p50 | p90 | p99 | p99 − p50 |
|:---|:---:|:---:|:---:|:---:|
| NanoMatch | **38.4 ns** | **42.7 ns** | **50.3 ns** | **+11.9 ns** |
| STL (`std::map` + `std::list`) | 106 ns | 123 ns | 148 ns | +42.0 ns |
| Speedup | **2.75×** | **2.88×** | **2.94×** | **3.5× tighter tail** |
 
The STL baseline's p99 degradation of +42 ns above its median is the fragmentation story in numbers: as the book grows and orders are repeatedly allocated and freed across scattered heap addresses, cache miss rates compound and tail latency balloons. NanoMatch's contiguous pool prevents this entirely — the p99 tail barely moves.
 
### 2. Pure Crossing — The Isolated Hot-Path Ceiling
 
`BM_HFT_100Pct_Crossing` vs `BM_Naive_STL_Crossing` measures the theoretical floor: every order immediately crosses and matches. No resting book state, no logger thread, no price scan. This isolates the raw cost of one matching operation.
 
| | p50 | p90 | p99 | Throughput |
|:---|:---:|:---:|:---:|:---:|
| NanoMatch | **9.36 ns** | **11.1 ns** | **12.5 ns** | **~227.4M ops/sec** |
| STL | 56.2 ns | 63.6 ns | 67.0 ns | ~32.5M ops/sec |
| Speedup | **6.0×** | **5.7×** | **5.3×** | **7.0×** |
 
The 6× gap here vs 2.75× in the realistic test is expected and honest: in the isolated test, the STL engine's `std::map` and `std::list` are also operating on a nearly empty book, so the tree traversal and allocation penalty is at its minimum. The realistic test, where the STL book carries 100k resting nodes scattered across heap, is actually the fairer comparison.
 
`BM_HFT_100pct_With_Sentinels` (9.11 ns) runs the same test with two resting orders already in the book to simulate a non-empty market. The near-identical result confirms the engine's hot path doesn't degrade under resting liquidity.
 
### 3. Order Cancellation — O(1) Direct Mapping
 
`BM_HFT_Order_Cancellation` measures the cost of inserting an order then immediately cancelling it. This is the most architecturally distinctive result in the suite.
 
Standard order books must search for a specific order ID within a price queue to cancel it — O(log N) for tree-based books, O(N) for queue scans. NanoMatch uses a direct-mapped `order_map` vector where `order_id` is the array index. Cancellation resolves to a single array read followed by pure O(1) doubly-linked list pointer extraction — no search, no scan, no tree traversal:
 
```cpp
int32_t pool_id = order_map[order_id];  // single array read — O(1)
// followed by O(1) doubly-linked list pointer extraction
```
 
**Result: 6.81 ns median, ~269.0M ops/sec.** Pulling a resting order from the middle of a deep queue takes fewer than 30 CPU clock cycles. The `NULL_ORDER` constant (`constexpr int32_t NULL_ORDER = -1`) is used throughout `nano.cpp` as the sentinel value, replacing raw magic numbers.
 
### 4. Sparse Book Scan — No Penalty at Non-Zero Price Regions
 
`BM_HFT_Sparse_Book_Scan` runs isolated crossing at price tick 15,105 — far from the base price — to verify the engine doesn't carry any latency penalty for operating away from index zero.
 
**Result: 7.40 ns median, ~251.9M ops/sec.** Consistent with the pure crossing result. Direct array indexing is O(1) regardless of which price region is active.
 
---
 
## ⚠️ Disclosed Worst Case: The Linear Scan Problem
 
This project documents its architectural edge case — and the mitigation built to address it.
 
The flat price array has one non-O(1) operation: finding the next valid best price after the current level empties. The engine mitigates this with `total_bid_volume` and `total_ask_volume` counters. When the book is fully empty, the scan is bypassed with an O(1) reset:
 
```cpp
if (total_bid_volume == 0) best_bid = 0;
else while (best_bid > 0 && bids_head[best_bid] == NULL_ORDER) best_bid--;
```
 
The linear scan only fires in one specific case: resting orders exist at other price levels but the *current* best level just emptied. In a pathological market alternating between \$100 and \$90,000 (Dataset B), this fires on nearly every event.
 
**Measured worst-case floor: ~15.15M events/sec (66.00 ns average latency).**
 
Even here the floor is high: the contiguous array lets the CPU prefetcher rip through empty slots sequentially with no pointer chasing. Dataset A (90% cancellation, ~41.32M events/sec) confirms the mitigation works under real stress — the volume counter short-circuits the scan whenever the book fully drains, which is the common case during cancellation storms.
 
 
 
---
 
## 🚄 End-to-End Throughput: Three Market Scenarios
 
The E2E engine ingests 5,000,000-row CSV files via zero-copy `mmap` with `MADV_SEQUENTIAL` kernel prefetch hints. A background SPSC logger thread runs concurrently throughout. Three synthetic datasets expose the architecture's boundaries.
 
### Dataset A — Cancellation Storm (90% Cancel Rate)
 
The generator maintains a live vector of active order IDs. 90% of events are cancellations drawn randomly from that vector; 10% are new orders with an 80/20 passive/aggressive spread distribution.
 
**Result: ~41.32M events/sec | 24.20 ns average | 5M orders in 121 ms.**
 
This is the fastest E2E result despite the highest cancellation rate. Two reasons: first, the direct-mapped `order_map` means every cancellation resolves in ~6.81 ns via a single array index lookup — the engine never scans the 100,000 active orders to find the target. Second, the ingestion pipeline uses a lean 4-column format (`OrderID, Side, Price, Qty`) with no timestamp column. This keeps the `fast_atoi` parser working entirely in CPU registers with no intermediate buffers, which is what sustains 41M+ events/sec through the `mmap` pipeline.
 
### Dataset C — Dense Equilibrium (Liquid Market)
 
Tight 5-tick spread (bids 9,995–10,000, asks 10,000–10,005), 20% cancellation rate, sliding window of 10,000 active orders. Designed to simulate a Blue Chip or ETF ticker under normal conditions.
 
**Result: ~40.00M events/sec | 25.00 ns average | 5M orders in 125 ms.**
 
`best_bid` and `best_ask` step only between adjacent filled slots, keeping access patterns sequential and the hardware prefetcher perfectly fed. The engine stays entirely within L1/L2 throughout.
 
### Dataset B — Pathological Extremes (Worst Case)
 
Orders alternate between price $100 and price $90,000 with no cancellations. Every match empties the current price level, forcing the engine to scan ~89,900 empty array slots to locate the next valid best price.
 
**Result: ~15.15M events/sec | 66.00 ns average | 5M orders in 330 ms.**
 
This is the disclosed performance floor. It is included because any architecture that only shows its best case is hiding something. See [`docs/2_THROUGHPUT_E2E.md`](docs/2_THROUGHPUT_E2E.md) for the full mathematical breakdown of why each dataset produces its specific throughput.
 
---
 
## 🧠 Hardware-Sympathy Techniques
 
| Technique | Implementation | Hardware Benefit |
|:---|:---|:---|
| Pre-allocated `OrderPool` | Single `std::vector<Order>` at boot, `free_indices` stack for recycling | Zero `malloc`/`new` on hot path; pool stays L1/L2 resident |
| `alignas(64)` Order struct (`nano.cpp`) | One order per 64-byte cache line | Prevents cache pollution during random pool traversal (cancellations) |
| `alignas(32)` Order struct (`benchmark.cpp`) | Two orders per 64-byte cache line | Doubles sequential memory throughput in controlled benchmark loops |
| Flat price array (`bids_head[price]`) | Direct index lookup, no tree | O(1) price level access, no pointer chase, hardware prefetcher stays fed |
| `NULL_ORDER` sentinel (`constexpr int32_t NULL_ORDER = -1`) | Named constant throughout `nano.cpp` | No magic numbers; used in all linked-list head/tail/next/prev checks |
| Direct-mapped `order_map` | `order_id` as array index | O(1) cancellation; zero hash collisions; single array read to locate any order |
| Intrusive doubly-linked list | `next_id`/`prev_id` indices inside `Order` struct | O(1) removal without heap allocation per node |
| `alignas(64)` SPSC atomic indices | Separate cache lines for `write_index` and `read_index` | Eliminates false sharing; prevents MESI protocol invalidation between producer and consumer cores |
| Bitwise AND ring buffer wrap | `(idx + 1) & (capacity - 1)` | 1-cycle index wrap vs ~20-cycle modulo division |
| `[[likely]]` / `[[unlikely]]` hints | On hot-path branch conditions | Moves cold code to binary tail; keeps L1 i-cache filled with matching logic |
| `mmap` + `madvise(MADV_SEQUENTIAL)` | OS-level file mapping with prefetch hint | Zero-copy ingestion; kernel prefetches pages before CPU requests them |
| `fast_atoi` inline parsers | Pointer-arithmetic integer parsing | No locale checks, no function call overhead; vectorizable with `-march=native` |
| `-O3 -march=native` compilation | Host-specific SIMD, full inlining, LTO | AVX2 vectorization on arithmetic loops; no cross-platform overhead |
 
> The flame graph proof: `nano_flamegraph.png` shows a flat, allocation-free execution band dominated by `BM_HFT_Realistic_Market`. `stl_flamegraph.png` shows `malloc`, `operator new`, `cfree`, and `[libc.so.6]` towers — the CPU spending matching time in memory management instead.
 
---
 
## 🖥️ Hardware & Test Environment
 
All benchmarks were run on:
 
| Component | Specification |
|:---|:---|
| **Processor** | Intel Core i5-12450HX (12th Gen Alder Lake, 8C/12T) |
| **Architecture** | x86_64 with AVX2/SIMD (`-march=native`) |
| **CPU Cache** | 32KB L1d / 48KB L1i per P-core · 8MB L2 · 12MB L3 |
| **Memory** | 16 GB DDR5 @ 4800 MT/s |
| **OS** | Windows 11 + WSL2 (Ubuntu 24.04.1 LTS) |
| **Compiler** | GCC/G++ 13.3.0 |
| **Compile Flags** | `-O3 -march=native -Wall -Wextra -fno-omit-frame-pointer` |
| **Benchmark Harness** | Google Benchmark · 50 repetitions · custom p50/p90/p99 statistics |
| **Profiling** | Linux `perf` + flame graphs (`-fno-omit-frame-pointer`) |
 
> **WSL2 Note:** Build and run inside the native Linux filesystem (`~/Nano_Match`), not from a mounted Windows drive (`/mnt/c/`). The WSL2 9P file-sharing protocol will bottleneck the `mmap` parser and artificially suppress throughput numbers.
 
---
 
## 🚀 Quick Start
 
Full guide in [`docs/0_QUICK_START.md`](docs/0_QUICK_START.md). Minimum commands:
 
```bash
# 1. Clone into native Linux space
git clone https://github.com/Sukhjot-SinghS/Nano_Match.git
cd Nano_Match
 
# 2. Build (Release mode — mandatory for accurate results)
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j $(nproc)
cd ..
 
# 3. Generate the three 5M-row datasets
./build/generator
 
# 4. Run the E2E engine against each dataset
./build/trading_server data/dataset_A_realistic.csv
./build/trading_server data/dataset_B_pathological.csv
./build/trading_server data/dataset_C_dense_equilibrium.csv
 
# 5. Run the micro-benchmark suite
./build/engine_bench
```
 
---
 
## 🛠️ Tech Stack
 
| Component | Detail |
|:---|:---|
| **Language** | C++17 |
| **Build System** | CMake + Make |
| **Benchmarking** | Google Benchmark (`libbenchmark-dev`) |
| **Profiling** | Linux `perf` + flame graphs |
| **OS** | Linux / WSL2 (POSIX `mmap` required) |
| **Compiler Flags** | `-O3 -march=native -Wall -Wextra -fno-omit-frame-pointer` |
 
---
 
## 📂 Repository Structure
 
```
Nano_Match/
├── CMakeLists.txt
├── src/
│   └── nano.cpp                       # Production engine
├── benchmarks/
│   └── benchmark.cpp                  # Google Benchmark harness (7 scenarios)
├── data/
│   └── generate_data.cpp              # C++ O(1) synthetic dataset generator
└── docs/
    ├── 0_QUICK_START.md
    ├── 1_LATENCY_PROFILE.md
    ├── 2_THROUGHPUT_E2E.md
    ├── 3_CACHE_ARCHITECTURE.md
    └── assets/
        ├── benchmark_1.png            # Google Benchmark terminal output (part 1)
        ├── benchmark_2.png            # Google Benchmark terminal output (part 2)
        ├── nano_flamegraph.png        # NanoMatch: flat, allocation-free flame graph
        ├── stl_flamegraph.png         # STL baseline: malloc/operator new towers
        ├── throughput_A.png           # Dataset A performance dashboard
        ├── throughput_B.png           # Dataset B performance dashboard
        └── throughput_C.png           # Dataset C performance dashboard
```
 
> The CSV datasets are not committed. Run `./build/generator` to create them locally. The generator produces all three 5M-row files in milliseconds via O(1) swap-and-pop mechanics.
 
---
 
*Every nanosecond is engineered. Every benchmark tells a story.*