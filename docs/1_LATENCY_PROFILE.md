# ⏱️ Latency Profile & Micro-Benchmarks

> [!NOTE]
> **📊 Benchmark Output Images:** The micro-benchmark latency data discussed below is directly visualized in the `docs/assets/benchmark_1.png` and `benchmark_2.png` terminal outputs.

This document breaks down the nano-second latency characteristics of the NanoMatch engine. Testing was conducted using Google Benchmark on an **Intel Core i5-12450HX (12th Gen Alder Lake)** architecture, measuring pure `L1/L2` cache execution times isolated from network or disk I/O. 

Results are ordered from most practically relevant to most technically detailed.

---

## 1. The Chaos Test: `BM_HFT_Realistic_Market` vs `BM_STL_Realistic_Market`

**The Engineering Challenge:** Real markets do not just cross orders perfectly; they involve chaotic, randomized resting liquidity. We created this benchmark to prove our custom `OrderPool` does not degrade over time due to memory fragmentation under realistic load.

**The Test:** We pre-loaded both engines with a deep resting limit order book. We then bombarded them with a continuous, randomized stream of passive and aggressive actions (simulating real-world order flow).

| Benchmark Scenario | p50 (Median) | p90 Tail | p99 Tail |
| :--- | :--- | :--- | :--- |
| **`BM_STL_Realistic_Market`** | 106 ns | 123 ns | 148 ns |
| **`BM_HFT_Realistic_Market`** | **38.4 ns** | **42.7 ns** | **50.3 ns** |
| 🚀 **Net Improvement** | **2.75x Faster** | **2.88x Faster** | **2.94x Faster** |

**The Result:** Under sustained, chaotic load, standard STL containers buckle, becoming nearly three times slower (106 ns median). The NanoMatch engine maintains a **38.4 ns** median execution time. This proves that pre-allocating contiguous memory and recycling indices via our custom `freeList` completely protects the L1 CPU cache from fragmentation decay.

---

## 2. The Baseline: High-Frequency Architecture vs. C++ STL

**The Context:** Standard algorithmic trading strategies often rely on high-level C++ standard template library (STL) containers like `std::map` and `std::list`. We created `BM_Naive_STL_Crossing` to expose how these Red-Black tree structures force the CPU to make dynamic OS `malloc` calls and chase pointers across fragmented memory, destroying latency. 

We compared this directly to our fully optimized NanoMatch engine (`BM_HFT_100Pct_Crossing`), which uses an intrusive, pre-allocated memory pool. Both engines were given the exact same task: instantly cross and match incoming Buy and Sell orders.

| Benchmark Scenario | p50 (Median) | p90 Tail | p99 Tail | Throughput |
| :--- | :--- | :--- | :--- | :--- |
| **`BM_Naive_STL_Crossing`** | 56.2 ns | 63.6 ns | 67.0 ns | ~32.5 Million ops/sec |
| **`BM_HFT_100Pct_Crossing`** | **9.36 ns** | **11.1 ns** | **12.5 ns** | **~227.4 Million ops/sec** |
| 🚀 **Net Improvement** | **6.0x Faster** | **5.7x Faster** | **5.3x Faster** | **7.0x Higher** |

**Simulating Resting Liquidity (`BM_HFT_100pct_With_Sentinels`):** We ran the same 100% crossing test but with two resting sentinel orders already in the book (one bid at 9999, one ask at 10001), simulating a real market where the book is never empty. 
* **Latency:** **9.11 ns** (Median) | p99: **13.4 ns**
* **Throughput:** **~237.3 Million ops/sec**
* **Conclusion:** The near-identical result to pure crossing confirms the engine's hot path doesn't degrade when resting liquidity exists.

---

## 3. True O(1) Memory Recycling: `BM_HFT_Order_Cancellation`

**The Engineering Challenge:** Standard matching engines suffer from $O(\log N)$ or $O(N)$ delays during mass cancellation events because they must search through price queues to find specific Order IDs. 

**The NanoMatch Solution:** NanoMatch completely bypasses queue traversal. We engineered a direct-mapped `std::vector<int32_t> orderMap`. 

**The Result:** When a cancel request arrives, the engine instantly looks up the physical RAM address in the array and manipulates the intrusive Doubly-Linked List pointers (`nextOrderIndex`, `prevOrderIndex`) to snip the order out of the active queue. 
* **Latency:** **6.81 ns** (Median)
* **Throughput:** **~269.0 Million ops/sec**
* **Performance:** This pure $O(1)$ pointer arithmetic ensures that pulling a resting order from the middle of a massive queue takes fewer than 30 CPU clock cycles.

---

## 4. Non-Zero Price Region: `BM_HFT_Sparse_Book_Scan`

**The Engineering Challenge:** Array-based limit order books can behave differently depending on where matching occurs relative to the baseline memory address.

**The Test:** This benchmark measures matching latency at a non-zero price region (tick 15,105) with no prior resting orders at that level, isolating the cost of a clean insert-then-match cycle away from the base price.

**The Result:** * **Latency:** **7.40 ns** (Median)
* **Throughput:** **~251.9 Million ops/sec**
* **Performance:** The slightly lower latency versus Section 2 is because there is no sentinel book state to traverse. The result confirms that the engine's per-operation cost is consistent regardless of which price region is active — there is no penalty for operating away from the base price tick.
