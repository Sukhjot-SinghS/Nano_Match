# 🚄 End-to-End Throughput & Data Ingestion

> [!NOTE]
> **📸 Raw Data:** *The terminal screenshots showing the 5-million row ingestion speeds are available in the [assets/](./assets/) directory.*

While micro-benchmarks prove the nanosecond efficiency of the L1 cache, an order matching engine is only as strong as its I/O pipeline. This project was built to satisfy a strict requirement: **parse raw binary PCAP/ITCH files or multi-million row CSVs using zero-copy `mmap`.** This document outlines that memory mapping architecture and the stress-test datasets used to validate the engine's End-to-End (E2E) throughput.

## 1. Zero-Copy `mmap` Ingestion
Standard C++ file streams (`std::ifstream`) are notoriously slow for High-Frequency Trading because they rely on user-space buffers and constant context switching.

To achieve maximum throughput, NanoMatch bypasses C++ streams entirely:
1. It uses native OS kernel calls (`open`, `fstat`) to locate the dataset.
2. It uses `mmap` to map the multi-megabyte CSV directly into the application's virtual memory space.
3. It utilizes custom-written `fast_atoi64` and `fast_atoi32` pointer-arithmetic parsers to instantly convert ASCII text (like Timestamps, Prices, and Quantities) to integers without ever invoking standard library string functions.

---

## 2. The Datasets: O(1) Synthetic Generation
To accurately test the engine, we needed multi-million row datasets. Standard Python scripts taking $O(N)$ list-shifting time were too slow. Instead, we built a pure C++ generator (`data/generate_data.cpp`) that utilizes $O(1)$ vector `swap-and-pop` mechanics to forge 5,000,000 rows of deterministic market data in milliseconds.

We designed three specific market scenarios to map the absolute boundaries and peak capabilities of the architecture:

### Scenario A: The Cancellation Storm (High Volatility)
* **Design:** Simulates a highly volatile, algorithmic market state where market makers constantly place and revoke resting limit orders at the top of the book.
* **The Simulation Mechanics:** * **90% Cancellation Rate:** The generator tracks all active Order IDs in a dynamic `std::vector`. 90% of the time, it generates a cancellation ('X' message) by selecting a random active ID, swapping it with the back of the vector, and popping it in $O(1)$ time.  
  * **80/20 Spread Logic:** For the remaining 10% of order additions ('A' messages), it uses an 80/20 probability distribution. 80% are passive limit orders placed 1-10 ticks away from the base price, and 20% are aggressive orders placed 1-5 ticks deep into the spread.
* **Throughput Achieved:** **~93,489 events / second** (5 million orders in 53.4 seconds).

> [!WARNING]
> **The Architecture Trade-off:** This exposes the intentional, known trade-off of using a flat contiguous array instead of a Red-Black tree (`std::map`). When the 90% cancellation rate constantly strips away Top-of-Book orders, the engine must linearly scan the price array (`while (bids_head[best_bid] == NULL_ORDER) best_bid--;`) to find the next active price level. Despite millions of these worst-case scans, the engine still processes roughly 1 event every 10 microseconds.

> [!NOTE]
> **📸 Dashboard Proof:** > The visual proof of these results is shown in the following performance dashboard, directly generated from the Scenario A test run.  
> [![Scenario A Dashboard](assets/throughput_A.png)](assets/throughput_A.png)

---

### Scenario B: Pure Matching / The Pathological Book (Illiquid)
* **Design:** Represents a completely broken, illiquid market designed to test pure insertion and matching speed without resting queue buildup. 
* **The Simulation Mechanics:** The generator completely disables cancellations (0% cancel rate). It then alternates between generating aggressive Buy and Sell orders at extreme price bounds: specifically targeting price `$100` and price `$90,000`. 
* **Throughput Achieved:** **~9.54 Million events / second** (5 million orders in 524 ms).

> [!TIP]
> **The Architecture:** Because the alternating extreme prices (100 vs 90,000) instantly cross and execute against each other, the book never builds up a dense wall of resting orders. Therefore, the `best_bid` and `best_ask` pointers never have to conduct linear array scans. The engine operates in pure $O(1)$ execution time.

> [!NOTE]
> **📸 Dashboard Proof:** > The actual output from this pathological scenario is shown in the dashboard below.  
> [![Scenario B Dashboard](assets/throughput_B.png)](assets/throughput_B.png)

---

### Scenario C: Dense Continuous Flow (Equilibrium Market)
* **Design:** Represents a highly liquid, stable market environment (e.g., standard ETF or Blue Chip ticker) characterized by tight spreads, continuous matching, and a normalized ratio of insertions to cancellations.
* **The Simulation Mechanics:** * **20% Cancellation Rate:** The generator maintains a sliding window of the last 10,000 active orders in memory. 20% of the time, it executes an $O(1)$ random deletion from this window, creating a dense resting book without overloading memory capacity.
  * **Tight Spread Logic:** The remaining 80% are order additions placed in an extremely tight price band (Bids between 9995-10000, Asks between 10000-10005) to ensure continuous, high-probability crossing.
* **Throughput Achieved:** **~24.04 Million events / second** (5 million orders in 208 ms).
* **Average Latency:** **41.60 nanoseconds / event**.

> [!IMPORTANT]
> **The Architecture:** This scenario represents the "golden path" for an L1-cache aligned contiguous array. Without the constant forced array scans of Scenario A or the extreme edge-case boundaries of Scenario B, the engine stays perfectly locked in the CPU cache. The `best_bid` and `best_ask` tracking pointers only shift locally within adjacent array slots, allowing the hardware prefetcher to operate at maximum efficiency. 

> [!NOTE]
> **📸 Dashboard Proof:** > See the results of the equilibrium market stress test below. This dashboard provides concrete evidence of the engine's maximum throughput when operating on dense, continuous order flow:  
> [![Scenario C Dashboard](assets/throughput_C.png)](assets/throughput_C.png)

---

## 3. Conclusion on Hardware Sympathy
By engineering a 90% cancellation dataset (Scenario A), we successfully mapped the exact degradation limits of the L1 hardware prefetcher during heavy Top-of-Book volatility. Conversely, by forcing pure execution conditions (Scenario B), the engine effortlessly scales to nearly 10 Million ops/sec. Finally, when subjected to a standard, dense continuous flow market (Scenario C), the engine operates without friction to reach an exceptional throughput potential of **~24.04 Million ops/sec**.

This empirical data proves the system's deterministic behavior across both ideal matching conditions and extreme adversarial market stress states.