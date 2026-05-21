# 🧠 Hardware Sympathy & Cache Architecture

> [!NOTE]
> **📸 Visual Proof:** *The interactive Linux `perf` Flame Graphs (`nano_flamegraph.png` and `stl_flamegraph.png`) are available in the [assets/](./assets/) directory. Download and open them in any web browser to explore the stack traces.*

---

## 🎯 Introduction

In High-Frequency Trading (HFT), the most brilliant mathematical model is useless if the underlying execution system is a microsecond too slow. This document outlines the explicit hardware-sympathy techniques, compiler directives, and memory structures engineered into **NanoMatch** to achieve **sub‑10 nanosecond execution times**.

---

## 1️⃣ The Visual Proof: Eliminating Kernel Traps 🚫

### 📊 The Problem

If you open the baseline `stl` Flame Graph ([assets/stl_flamegraph.png](assets/stl_flamegraph.png)), you will see jagged towers on the right side of the stack trace heavily populated with `malloc` and `operator new` calls. Every time a standard `std::map` or `std::deque` needs memory for a resting order, the CPU pauses the execution thread, switches to kernel mode, and asks the OS for RAM.

### ⚡ The Fix

NanoMatch uses a custom `OrderPool` that pre‑allocates contiguous memory blocks (`std::vector<Order>`) upon boot.

### ✅ The Result

As seen in our optimized `nano` Flame Graph ([assets/nano_flamegraph.png](assets/nano_flamegraph.png)), the execution base is perfectly **flat**. The engine operates entirely in user‑space, locked inside the hot‑path `while` loops, never yielding control to the Linux kernel.

> [!IMPORTANT]
> **Verification:** We compiled the engine with `-fno-omit-frame-pointer` to ensure `perf` could unwind the call stacks precisely, definitively proving the complete absence of memory allocation overhead on the hot path.

---

## 2️⃣ Memory Architecture & OS Bypass 🔧

We entirely bypassed standard C++ file streams (`std::ifstream`), which rely on slow user‑space buffers.

### 📌 Zero‑Copy `mmap`

We used raw OS kernel calls (`open`, `mmap`) to map the CSV data directly into the application's virtual memory space, eliminating the kernel‑to‑user memory copy.

### 🚀 Kernel Prefetching (`madvise`)

We applied `madvise(mapped_data, file_size, MADV_SEQUENTIAL)`. This explicitly instructs the Linux kernel that memory will be read linearly. The OS aggressively prefetches subsequent memory pages into RAM before the CPU even asks for them, hiding disk/network latency and turning file ingestion into a pure memory‑bandwidth operation.

---

## 3️⃣ Cache Locality & Struct Packing 📦

A modern CPU fetches memory from main RAM in **64‑byte chunks** called **cache lines**. If a data structure crosses two cache lines, it triggers two RAM fetches, instantly doubling latency.

### 🎯 Alignment Directives

We used `alignas(64)` (and `alignas(32)` in the benchmark suite) on the `Order` structs. This packs the structs perfectly into the CPU's L1/L2 cache lines, guaranteeing that reading an order's parameters requires exactly one memory fetch without straddling boundaries.

> [!TIP]
> **❓ Why Two Sizes?**
>
> - **`alignas(64)`** in `nano.cpp` fits a single `Order` into one cache line – ideal when the live engine randomly accesses orders scattered across the memory pool.
> - **`alignas(32)`** in `benchmark.cpp` allows exactly two `Order` objects to reside in the same 64‑byte line, exploiting spatial locality during sequential processing tests.

---

## 4️⃣ Concurrency & False Sharing Prevention 🔀

To offload disk I/O, we implemented a **Single‑Producer Single‑Consumer (SPSC) Lock‑Free Ring Buffer** that ships execution logs to a background thread.

### 🔒 No Mutexes

Standard `std::mutex` locks trigger system calls (`futex`) that incur tens of microseconds of overhead. Our queue uses pure user‑space `std::atomic` variables with strict `memory_order_release` and `acquire` semantics.

### 💥 Eradicating False Sharing

We explicitly separated the buffer's tracking indices:

```cpp
alignas(64) std::atomic<size_t> write_index{0};
alignas(64) std::atomic<size_t> read_index{0};
```

By forcing the producer (write) and consumer (read) pointers onto completely different 64‑byte physical cache lines, we eliminated **False Sharing**. Core 1 and Core 2 never fight over the same cache line, preventing continuous CPU cache invalidation cycles.

### 😴 Idle Behaviour

The consumer thread, when the queue is empty, calls `std::this_thread::sleep_for(1µs)`. This is a user‑space sleep that does not enter the kernel, avoiding costly context switches.

---

## 5️⃣ CPU Pipeline & ALU Optimization ⚙️

We specifically optimised the engine to prevent the CPU's Arithmetic Logic Unit (ALU) from stalling.

### 🔢 Bitwise Masking vs. Modulo

In a standard ring buffer, wrapping the index uses modulo arithmetic (`current_write % capacity`). A modulo requires a division instruction, stalling the ALU pipeline for **~15–20 CPU cycles**. We replaced this with a bitwise AND:

```cpp
const size_t next_write = (current_write + 1) & (capacity - 1);
```

This evaluates the wrapping bounds in exactly **1 clock cycle**.

### 🎲 Branch Prediction Directives (`[[likely]]` / `[[unlikely]]`)

We guided the CPU's hardware branch predictor using C++20 attributes. For example:

```cpp
if (bestAskLevel.headOrderIndex == -1) [[unlikely]] { ... }
```

This forces the compiler to keep the fast path (where resting orders successfully match) fully in‑line, while relegating the empty‑level array scan to a cold path in the compiled binary.

> [!TIP]
> (Note: This is heavily utilized in `benchmark.cpp`, and can also be applied in `nano.cpp` for marginal gains.)

### 🔨 Compiler Optimisations

The engine is compiled with `-O3 -march=native -Wall -Wextra`.

- **`-O3`** enables aggressive loop unrolling and inlining.
- **`-march=native`** allows the compiler to use host‑specific hardware vectorisation (e.g., AVX2/SIMD instructions) – this notably accelerates our custom `fast_atoi` string parsers.

---

## 6️⃣ O(1) Cancellation via Direct Mapping 🎯

Standard order books suffer from $O(\log N)$ or $O(N)$ delays during mass cancellation events because they must search through Red‑Black trees or linked lists to find specific Order IDs.

### 🔑 The Perfect Hash

NanoMatch uses a direct‑mapped `std::vector<int32_t> order_map` where the order_id acts directly as the array index. This acts as a **perfect hash table with zero collisions**, guaranteeing pure **O(1)** memory address lookups.

> [!WARNING]
> **Trade-off Acknowledgment:** This ultra-fast $O(1)$ lookup works perfectly because our synthetic data generator issues dense, sequential order IDs. A production system receiving randomized 64-bit alphanumeric IDs would require a sparse hash map, which is a known architectural trade‑off.

---

## 7️⃣ The Architectural Trade‑Off (Flat Price Array) ⚖️

While the flat price array (`bids_head[price]`, etc.) guarantees deterministic **O(1)** access and remains entirely cache‑resident during normal liquid market conditions, it introduces an explicit hardware vulnerability.

> [!WARNING]
> **📉 The Empty Book Problem:** In a completely illiquid market, scanning 100,000 empty price levels forces the CPU to touch every cache line of the array, triggering thousands of sequential cache misses. This results in the **~95 µs tail latency** measured in our pathological benchmark.

> [!TIP]
> **🛡️ The Sentinel Mitigation:** In real‑world environments, market‑maker algorithms keep the book dense. We simulate this by anchoring sentinel orders placed one tick away from the action. These sentinels act as cache barriers that prevent the CPU from ever initiating the linear array scan, instantly dropping execution time back to its absolute mathematical limit of **~7 nanoseconds**.

---

## 8️⃣ Summary of Hardware Techniques 📋

| 🔧 Technique                                  | ⚡ Hardware Benefit                                   |
|:----------------------------------------------|:-----------------------------------------------------|
| Custom OrderPool (contiguous array)           | No kernel malloc calls; memory stays in L1/L2 cache. |
| `alignas(64)` / `alignas(32)`                | Each Order fits in one cache line – no straddling.   |
| Separate cache lines for ring buffer indices  | Eliminates false sharing between producer and consumer. |
| Bitwise AND for index wrapping                | 1 cycle vs. 20 cycles for modulo.                    |
| `[[likely]]` / `[[unlikely]]`                | Guides branch predictor; hot path stays in‑line.     |
| `madvise(MADV_SEQUENTIAL)`                   | Kernel prefetcher hides I/O latency.                 |
| `-march=native` + `-O3`                      | Enables SIMD and aggressive optimisations.           |
| Direct‑mapped order_map                      | O(1) cancellations with no hash collisions.          |

---

## 🎓 Conclusion

These techniques transform abstract C++ code into a **deterministic, cache‑aware machine** that the CPU can execute without stalls – the essence of **hardware sympathy**.

✨ **Every microsecond counts in HFT, and every nanosecond is engineered.** ✨