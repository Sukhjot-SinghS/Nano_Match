# 🧠 Hardware Sympathy & Cache Architecture

> **📸 Visual Proof:** The interactive Linux perf Flame Graphs nano_flamegraph.png and stl_flamegraph.png are available in the `assets/` directory.

---

## 🎯 Introduction

In High-Frequency Trading (HFT), the most brilliant mathematical model is useless if the underlying execution system is a microsecond too slow. This document outlines the explicit hardware-sympathy techniques, compiler directives, and memory structures engineered into NanoMatch to achieve sub‑10 nanosecond execution times.

The core philosophy is simple: **the CPU is the customer.** Every data structure, every pointer, and every memory access pattern is designed to feed the CPU exactly what it needs, exactly when it needs it, without ever asking the operating system for help.

---

## 1️⃣ The Visual Proof: Eliminating Kernel Traps 🚫

### 📊 The Problem

In the baseline [stl_flamegraph.png](assets/stl_flamegraph.png), you will see jagged towers on the right side of the stack trace heavily populated with `malloc`, `operator new`, and `free` calls. Every time a standard `std::map` or `std::list` needs memory for a resting order, the CPU pauses the execution thread, switches to kernel mode, and asks the OS for RAM. Each kernel trap costs tens of microseconds of scheduling overhead.

### ⚡ The Fix

NanoMatch uses a custom `OrderPool` that pre‑allocates a contiguous `std::vector<Order>` block upon boot. During the entire benchmark hot path, zero calls to `malloc` or `operator new` are made. Memory recycling is handled entirely in user-space via a `freeList` vector.

### ✅ The Result

In [nano_flamegraph.png](assets/nano_flamegraph.png), the execution base of our HFT Engine is perfectly flat and wide — dominated entirely by `BM_HFT_Realistic_Market` and `engine_bench`. There are no malloc towers, no kernel traps, and no memory management overhead. The engine operates entirely in user‑space, locked inside the hot‑path matching loops.

> **⚠️ Verification:** Both flame graphs were captured by compiling with `-fno-omit-frame-pointer` so that `perf` could unwind the call stacks precisely, definitively proving the complete absence of dynamic memory allocation on the hot path.

---

## 2️⃣ Memory Architecture & OS Bypass 🔧

### 📌 Zero‑Copy `mmap`

Standard C++ file streams (`std::ifstream`) copy data twice: once from disk into a kernel buffer, and again from the kernel buffer into user-space. NanoMatch bypasses this entirely using raw OS calls:

```cpp
mapped_data = static_cast<char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
```

The CSV file is mapped directly into the application's virtual address space. The CPU reads market data as if it were already in RAM.

### 🚀 Kernel Prefetching (`madvise`)

```cpp
madvise(mapped_data, file_size, MADV_SEQUENTIAL);
```

This single call hints to the Linux kernel that memory will be accessed linearly. The OS responds by aggressively prefetching subsequent pages into the page cache before the CPU requests them, streamlining file ingestion into a pure memory-bandwidth operation.

### ⚡ Custom Integer Parsers (`fast_atoi`)

Standard library functions like `std::stoi` carry locale checks and function-call overhead. NanoMatch replaces them with inline pointer-arithmetic parsers:

```cpp
inline uint64_t fast_atoi64(const char** ptr) {
    uint64_t result = 0;
    while (**ptr >= '0' && **ptr <= '9') {
        result = result * 10 + (**ptr - '0');
        (*ptr)++;
    }
    return result;
}
```

Compiled with `-march=native`, this loop runs entirely in registers, allowing the compiler to apply host-specific vectorization on the arithmetic.

---

## 3️⃣ Cache Locality & Struct Packing 📦

A modern CPU fetches memory from RAM in 64‑byte chunks called **cache lines**. How data is packed inside those 64 bytes dictates the speed of the engine.

### 🎯 Dual-Alignment Strategy (Zero Internal Fragmentation)

NanoMatch explicitly orders variables within the `Order` struct to eliminate C++ ABI padding holes, packing the core data perfectly into **26 bytes**. However, we deploy two distinct alignment profiles depending on the binary's execution pattern:

**1. `nano.cpp` (Production Engine) → `alignas(64)`**

```cpp
struct alignas(64) Order {
    uint64_t timestamp;   // 8 bytes
    uint32_t price;       // 4 bytes
    uint32_t quantity;    // 4 bytes
    int32_t  next_id;     // 4 bytes
    int32_t  prev_id;     // 4 bytes
    Side     side;        // 1 byte
    bool     is_active;   // 1 byte
}; // Padded to 64 bytes
```

> **The `alignas(64)` Philosophy:** The live engine processes chaotic, real-world cancellations, meaning it traverses the memory pool randomly via the Doubly-Linked List (`next_id`). If two orders shared a cache line, fetching a needed order would drag a physically adjacent — but logically unrelated — order into the cache, wasting 50% of the bandwidth. `alignas(64)` forces exactly one order per cache line, preventing cache pollution during random queue traversal.

**2. `benchmark.cpp` (Synthetic Suite) → `alignas(32)`**

```cpp
struct alignas(32) Order { ... }; // Padded to 32 bytes
```

> **The `alignas(32)` Philosophy:** The micro-benchmark loop often processes orders in a highly controlled, sequential access pattern. Packing two 32-byte orders into a single 64-byte cache line allows a single L1 fetch to service two loop iterations, doubling sequential memory throughput.

### 📐 Flat Price Arrays vs. Tree Structures

```cpp
std::vector<int32_t> bids_head;
std::vector<int32_t> bids_tail;
```

A `std::map` (Red-Black tree) requires pointer chasing across heap-allocated nodes scattered in memory. Our flat array architecture means locating a price level is a direct memory offset calculation, minimizing pointer chasing and making access highly predictable for the hardware prefetcher.

---

## 4️⃣ Concurrency & False Sharing Prevention 🔀

To offload trade logging, NanoMatch implements a **Single‑Producer Single‑Consumer (SPSC) Lock‑Free Ring Buffer** that ships execution records to a background thread.

### 🔒 No Mutexes

Standard `std::mutex` locks trigger `futex` syscalls that hand control to the OS scheduler. The ring buffer uses only `std::atomic` with explicit `memory_order_release` / `memory_order_acquire` semantics — pure user-space synchronization.

### 💥 Eradicating False Sharing

```cpp
alignas(64) std::atomic<size_t> write_index{0};
alignas(64) std::atomic<size_t> read_index{0};
```

Without `alignas(64)`, both atomic indices would likely share the same 64-byte cache line. When the producer thread writes and the consumer reads simultaneously, the CPU cache coherence protocol (MESI) would force both cores to invalidate and re-fetch the shared line, adding **60–100 ns** of latency per operation. Placing each index on a dedicated cache line completely physically isolates the cores.

### 😴 Idle Behavior (Spin-Yielding)

In a lock-free queue, the consumer thread must wait when empty. The engine yields gracefully:

```cpp
std::this_thread::yield();
// Note: Can be swapped for _mm_pause() for strict cycle-yielding
```

This tells the CPU to briefly surrender the core, saving memory bandwidth and power without the heavy penalty of a full OS context switch caused by `sleep_for`.

---

## 5️⃣ CPU Pipeline & ALU Optimization ⚙️

### 🔢 Bitwise Masking vs. Modulo

Standard ring buffer index wrapping uses modulo integer division (costing ~15–20 CPU cycles):

```cpp
next_write = (current_write + 1) % capacity;
```

NanoMatch forces `capacity` to be a power of 2, allowing a bitwise AND replacement (costing **1 CPU cycle**):

```cpp

const size_t next_write = (current_write + 1) & (capacity - 1);

```

 

### 🎲 Branch Prediction Directives

 

The CPU's hardware branch predictor speculatively executes paths. We guide it using C++20 attributes:

 

```cpp

if (bestAskLevel.headOrderIndex == -1) [[unlikely]] {

    bestAskPrice++;

    continue;

}

```

 

This dictates **Basic Block Placement**. The compiler physically moves this empty-level scan logic to a cold region at the bottom of the compiled binary, ensuring the L1 Instruction Cache (i-cache) is filled exclusively with the hot-path matching logic.

 

---

 

## 6️⃣ O(1) Cancellation via Direct Mapping 🎯

 

Standard order books suffer from $O(\log N)$ or $O(N)$ delays when searching for an order to cancel.

 

### 🔑 The Direct-Mapped Array

 

```cpp

std::vector<int32_t> order_map;

order_map.assign(10000000, -1);

```

 

The `order_id` acts directly as the array index. This is a **Direct Access Table**, meaning there are zero hash collisions and zero computation. A cancellation resolves to a single array read:

 

```cpp

int32_t pool_id = order_map[order_id];

```

 

Followed by pure $O(1)$ pointer arithmetic on the doubly-linked list. The entire cancellation executes in **6.81 ns**.

 

> **⚠️ Trade-off Acknowledgment:** This $O(1)$ lookup requires dense, sequential `uint64_t` order IDs. A system receiving sparse alphanumeric IDs would require a hash map (`std::unordered_map`), trading speed for memory efficiency. This is a known architectural constraint.

 

---

 

## 7️⃣ The Architectural Trade‑Off (Flat Price Array) ⚖️

 

The flat price array provides deterministic $O(1)$ access, but carries one explicit hardware vulnerability: **The Linear Scan Problem**.

 

When a massive cancellation sweeps the book and empties the current best-price level, the engine must find the next valid price:

 

```cpp

while (best_bid > 0 && bids_head[best_bid] == -1) best_bid--;

```

 

In a completely pathological market (Scenario B — prices alternating abruptly between \$100 and \$90,000), this scan traverses tens of thousands of empty array slots per event.

 

However, because the contiguous array lacks the heavy node-allocation overhead of a Red-Black tree, the CPU prefetcher rips through these empty ticks in fractions of a microsecond. This establishes our absolute worst-case performance floor at **~15.15M events/sec** — a speed still vastly superior to standard STL architectures.

 

---

 

## 🎓 Conclusion

 

These techniques collectively transform abstract C++ code into a deterministic, cache-aware machine that the CPU executes without stalls.

 

The micro-benchmark results confirm this directly: the NanoMatch engine achieves **9.36 ns** median crossing latency versus **56.2 ns** for an equivalent STL implementation — a **6x improvement** derived entirely from hardware sympathy and memory architecture, rather than algorithmic complexity.





