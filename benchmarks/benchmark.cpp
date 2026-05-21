#include <iostream>
#include <vector>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <random>
#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <benchmark/benchmark.h>
#include <x86intrin.h> // Provides access to x86 hardware intrinsics like __rdtsc() for CPU cycle counting

using namespace std;

// ============================================================================
// 1. LOW-LEVEL MEMORY ALIGNMENTS & HIGH-FREQUENCY TRADING DATA STRUCTURES
// ============================================================================

enum class Side : uint8_t { BUY, SELL }; // Packs into exactly 1 byte to conserve cache footprint

// Trade record structure transmitted across the lock-free Single-Producer Single-Consumer queue
struct TradeMsg {
    uint64_t buyerOrderId;
    uint64_t sellerOrderId;
    uint32_t price;
    uint32_t quantity;
};

// Intrusive Node Order structure. Backed by an explicit memory layout alignment.
// alignas(32) ensures exactly two Order objects fit perfectly inside a single 64-byte CPU Cache Line.
// This prevents cross-line straddling, ensuring that fetching an order requires exactly one cache memory line read.
struct alignas(32) Order {
    uint64_t orderId;        // 8 bytes - Unique identifier issued by the trading system
    uint32_t price;          // 4 bytes - Fixed-point price integer representation
    uint32_t quantity;       // 4 bytes - Current outstanding execution volume
    Side side;               // 1 byte  - Buy or Sell enum indicator
    int32_t nextOrderIndex = -1; // 4 bytes - O(1) Intrusive pointer to next node in the Price Level array pool
    int32_t prevOrderIndex = -1; // 4 bytes - O(1) Intrusive pointer to previous node in the Price Level array pool
    bool isActive = false;   // 1 byte  - Memory pool occupancy validation state flag
};

// Represents a flat price bucket containing the terminal edges of our embedded intrusive doubly-linked list.
struct PriceLevel {
    int32_t headOrderIndex = -1; // 4 bytes - Array index pointer targeting the oldest resting order at this price
    int32_t tailOrderIndex = -1; // 4 bytes - Array index pointer targeting the newest resting order at this price
    uint64_t totalVolume = 0;    // 8 bytes - Aggregated resting queue size for market depth tracking
};

// High-Throughput Single-Producer Single-Consumer (SPSC) Lock-Free Ring Buffer Queue.
// Designed specifically to offload trade record persistence out of the execution critical path.
template<size_t Capacity>
class SPSCRingBuffer {
    // Compile-time guard ensuring capacity is a power of 2. 
    // Necessary because bitwise operations break if this constraint is violated.
    static_assert((Capacity & (Capacity - 1)) == 0, "Ring Buffer Capacity must be a power of 2!");

private:
    std::array<TradeMsg, Capacity> buffer; // Continuous block of physical memory allocated on instantiation

    // alignas(64) isolates the write-pointer (head) and read-pointer (tail) into entirely distinct physical 64-byte Cache Lines.
    // This completely eradicates "False Sharing", preventing Core 1 and Core 2 from invalidating each other's cache lines during updates.
    alignas(64) std::atomic<size_t> head{0}; 
    alignas(64) std::atomic<size_t> tail{0}; 

public:
    // O(1) Non-blocking push operation executed strictly by the Matching Engine Thread (Producer)
    inline bool push(const TradeMsg& msg) noexcept {
        // relaxed load because the producer thread owns the head variable and no other core modifies it
        size_t current_head = head.load(std::memory_order_relaxed);
        
        // Fast Bitwise Masking: Evaluates wrapping bounds in exactly 1 CPU clock cycle inside the ALU.
        // Replaces the standard integer division modulo (%) operator which stalls the pipeline for 15-20 cycles.
        size_t next_head = (current_head + 1) & (Capacity - 1);
        
        // acquire barrier synchronizes with the Consumer's release store to tail, reading a reliable state of RAM
        if (next_head == tail.load(std::memory_order_acquire)) return false; // Queue is full, signal drop
        
        buffer[current_head] = msg; // Write trade object into contiguous array slot
        
        // release barrier guarantees that the message contents are fully committed to memory *before* the head pointer advances
        head.store(next_head, std::memory_order_release);
        return true;
    }

    // O(1) Non-blocking pop operation executed strictly by the Background Logging Thread (Consumer)
    inline bool pop(TradeMsg& msg) noexcept {
        // relaxed load because the consumer thread entirely owns the tail variable
        size_t current_tail = tail.load(std::memory_order_relaxed);
        
        // acquire barrier synchronizes with the Producer's release store to head, ensuring data visibility
        if (current_tail == head.load(std::memory_order_acquire)) return false; // Queue is empty, yield
        
        msg = buffer[current_tail]; // Extract trade data from memory array slot
        
        // release barrier forces the extraction to finish before updating the visible tail location
        tail.store((current_tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }
};

// Pre-allocated continuous memory pool for Order structures.
// Completely eliminates the OS kernel system call layer (malloc/new) inside the matching loop.
class OrderPool {
private:
    std::vector<Order> pool;       // The underlying contiguous array hosting pre-allocated nodes
    std::vector<int32_t> freeList; // Vector stack storing empty available indices inside the pool
public:
    OrderPool(size_t capacity) {
        pool.resize(capacity); 
        freeList.reserve(capacity);
        // Populates the index stack in reverse order to encourage high spatial locality on early allocations
        for (int32_t i = capacity - 1; i >= 0; --i) {
            freeList.push_back(i);
        }
    }

    // Pulls an existing pre-allocated slot from memory in true deterministic O(1) execution time
    inline int32_t allocateOrder() noexcept {
        if (freeList.empty()) return -1; // Out of memory slots
        int32_t index = freeList.back();
        freeList.pop_back();
        return index;
    }

    // Recycles an slot index back into the stack without reclaiming physical block frames from the OS
    inline void freeOrder(int32_t index) noexcept {
        pool[index].isActive = false; // Soft-delete flag reset
        freeList.push_back(index);    // Return index slot to allocation pool
    }

    // Inline accessor bypasses pointer-indirection by directly evaluating raw offset reference mapping
    inline Order& getOrder(int32_t index) noexcept { return pool[index]; }
};

// ============================================================================
// 2. ULTRA-LOW LATENCY EXTREME-OPTIMIZED LIMIT ORDER BOOK ENGINE
// ============================================================================

class LimitOrderBook {
private:
    static constexpr size_t MAX_PRICE_TICKS = 200000; // Hard bounded price dimension range mapping array
    
    // Contiguous pre-allocated vectors representing dense array-mapped price level slots.
    // Price acts directly as the indexing key: Book[Price] gives O(1) lookups without map tree hops.
    std::vector<PriceLevel> bids; 
    std::vector<PriceLevel> asks; 
    std::vector<int32_t> orderMap; // Continuous allocation table providing lookup transitions from OrderID -> Pool Index

    OrderPool pool;
    SPSCRingBuffer<131072>* tradeBuffer; // Pointer hook mapping to our external thread synchronization channel

    // Cursor tracking registers. Keeps the hot boundaries close to prevent deep multi-index lookups.
    uint32_t bestBidPrice = 0;
    uint32_t bestAskPrice = MAX_PRICE_TICKS; 

public:
    LimitOrderBook(size_t maxOrders, SPSCRingBuffer<131072>* buffer) 
        : pool(maxOrders), tradeBuffer(buffer) {
        bids.resize(MAX_PRICE_TICKS);
        asks.resize(MAX_PRICE_TICKS);
        orderMap.assign(5000000, -1); // Generously sized lookup domain avoiding pointer reallocations or map bounds collisions
    }
    
    // Processes incoming Buy actions. Evaluates cross-matching logic or writes to resting queues.
    inline void addBuyOrder(uint64_t orderId, uint32_t price, uint32_t quantity) noexcept {
        // MATCHING PHASE: Iterates while there is execution quantity and the incoming cross-price hits the resting ask border
        while (quantity > 0 && price >= bestAskPrice) {
            PriceLevel& bestAskLevel = asks[bestAskPrice];
            
            // [[unlikely]] branch hint explicitly guides compiler code generation.
            // Injects instruction paths optimizing for the matching state; empty level updates are put out-of-line.
            if (bestAskLevel.headOrderIndex == -1) [[unlikely]] {
                bestAskPrice++; // Step cursor up to seek next available sell tier
                if (bestAskPrice >= MAX_PRICE_TICKS) break;
                continue;
            }

            int32_t currentAskIndex = bestAskLevel.headOrderIndex;
            Order& restingAsk = pool.getOrder(currentAskIndex);

            // Compute actual matched execution unit boundaries
            uint32_t tradeQty = std::min(quantity, restingAsk.quantity);
            quantity -= tradeQty;
            restingAsk.quantity -= tradeQty;
            bestAskLevel.totalVolume -= tradeQty;

            // Push execution transaction message parameters into our lock-free SPSC channel
            if (tradeQty > 0 && tradeBuffer) {
                tradeBuffer->push({orderId, restingAsk.orderId, bestAskPrice, tradeQty});
            }

            // [[likely]] direction instruction hint: Outbound volume exhaustions completely clear nodes
            if (restingAsk.quantity == 0) [[likely]] {
                bestAskLevel.headOrderIndex = restingAsk.nextOrderIndex; // Advance queue head index
                if (bestAskLevel.headOrderIndex == -1) [[unlikely]] bestAskLevel.tailOrderIndex = -1;
                else pool.getOrder(bestAskLevel.headOrderIndex).prevOrderIndex = -1; // Clear node backward linkage pointers
                
                orderMap[restingAsk.orderId] = -1; // Evict active mapping coordinates
                pool.freeOrder(currentAskIndex);   // Return structure block back to memory container allocations
            }

            // Continuous execution level maintenance sequence loop. 
            // Triggered if the active step tier queue became empty during current transaction match execution loop.
            if (bestAskLevel.headOrderIndex == -1) {
                // THE SPARSE SCAN HOOK: Linear array index lookup engine scans forward to reposition tracking pointers.
                // It is the source of the high microsecond latencies measured when testing completely clear order books.
                do { bestAskPrice++; } while (bestAskPrice < MAX_PRICE_TICKS && asks[bestAskPrice].headOrderIndex == -1);
            }
        }

        // INSERTION PHASE: If incoming quantity remains unexecuted, register the residual contents as a resting limit order
        if (quantity > 0) [[likely]] {
            int32_t newOrderIndex = pool.allocateOrder();
            if (newOrderIndex == -1) [[unlikely]] return; // Allocation safety backup boundary escape
            orderMap[orderId] = newOrderIndex;

            Order& newOrder = pool.getOrder(newOrderIndex);
            newOrder.orderId = orderId;
            newOrder.price = price;
            newOrder.quantity = quantity;
            newOrder.side = Side::BUY;
            newOrder.isActive = true;
            newOrder.nextOrderIndex = -1; 
            newOrder.prevOrderIndex = -1;

            PriceLevel& bidLevel = bids[price];
            if (bidLevel.tailOrderIndex == -1) [[unlikely]] {
                // Queue was empty; assign the newly generated block as the exclusive terminal head link point
                bidLevel.headOrderIndex = newOrderIndex;
            } else [[likely]] {
                // Queue exists; perform explicit O(1) pointer appending manipulation linking new node to structure tail
                Order& oldTail = pool.getOrder(bidLevel.tailOrderIndex);
                oldTail.nextOrderIndex = newOrderIndex;
                newOrder.prevOrderIndex = bidLevel.tailOrderIndex; 
            }
            bidLevel.tailOrderIndex = newOrderIndex;
            bidLevel.totalVolume += quantity;

            // Maintain the tracking boundary cursor, pinning it to the highest available buying premium level
            if (price > bestBidPrice) bestBidPrice = price;
        }
    }

    // Processes incoming Sell actions. Evaluates matching loops or writes to resting structures. Mirror logic of addBuyOrder.
    inline void addSellOrder(uint64_t orderId, uint32_t price, uint32_t quantity) noexcept {
        // MATCHING PHASE: Iterates down against resting bid tiers
        while (quantity > 0 && price <= bestBidPrice) {
            PriceLevel& bestBidLevel = bids[bestBidPrice];
            if (bestBidLevel.headOrderIndex == -1) [[unlikely]] {
                if (bestBidPrice == 0) break; 
                bestBidPrice--; // Drop tracking level to locate adjacent buying entries
                continue;
            }

            int32_t currentBidIndex = bestBidLevel.headOrderIndex;
            Order& restingBid = pool.getOrder(currentBidIndex);

            uint32_t tradeQty = std::min(quantity, restingBid.quantity);
            quantity -= tradeQty;
            restingBid.quantity -= tradeQty;
            bestBidLevel.totalVolume -= tradeQty;

            if (tradeQty > 0 && tradeBuffer) {
                tradeBuffer->push({restingBid.orderId, orderId, bestBidPrice, tradeQty});
            }

            if (restingBid.quantity == 0) [[likely]] {
                bestBidLevel.headOrderIndex = restingBid.nextOrderIndex;
                if (bestBidLevel.headOrderIndex == -1) [[unlikely]] bestBidLevel.tailOrderIndex = -1;
                else pool.getOrder(bestBidLevel.headOrderIndex).prevOrderIndex = -1;
                orderMap[restingBid.orderId] = -1;
                pool.freeOrder(currentBidIndex);
            }

            if (bestBidLevel.headOrderIndex == -1) {
                // Linear downward lookup array scan tracking for active bid levels when a price bucket is cleared out
                while (bestBidPrice > 0 && bids[bestBidPrice].headOrderIndex == -1) bestBidPrice--;
            }
        }

        // INSERTION PHASE: Record remaining volume inside resting asks array structure
        if (quantity > 0) [[likely]] {
            int32_t newOrderIndex = pool.allocateOrder();
            if (newOrderIndex == -1) [[unlikely]] return; 
            orderMap[orderId] = newOrderIndex;

            Order& newOrder = pool.getOrder(newOrderIndex);
            newOrder.orderId = orderId;
            newOrder.price = price;
            newOrder.quantity = quantity;
            newOrder.side = Side::SELL;
            newOrder.isActive = true;
            newOrder.nextOrderIndex = -1;
            newOrder.prevOrderIndex = -1;

            PriceLevel& askLevel = asks[price];
            if (askLevel.tailOrderIndex == -1) [[unlikely]] {
                askLevel.headOrderIndex = newOrderIndex;
            } else [[likely]] {
                Order& oldTail = pool.getOrder(askLevel.tailOrderIndex);
                oldTail.nextOrderIndex = newOrderIndex;
                newOrder.prevOrderIndex = askLevel.tailOrderIndex;
            }
            askLevel.tailOrderIndex = newOrderIndex;
            askLevel.totalVolume += quantity;

            // Maintain the tracking boundary cursor, pinning it to the lowest available selling offer level
            if (price < bestAskPrice) bestAskPrice = price;
        }
    }

    // High-frequency O(1) terminal cancellation engine. Rewires node index tracking pointers in place.
    inline void cancelOrder(uint64_t orderId) noexcept {
        if (orderId >= orderMap.size()) return;
        int32_t index = orderMap[orderId];
        if (index == -1) return; // Order is completely absent or already purged from live data structures

        Order& order = pool.getOrder(index);
        if (!order.isActive) return;

        uint32_t price = order.price;
        Side side = order.side;

        // Step 1: Delink forward neighbor connections
        if (order.prevOrderIndex != -1) {
            pool.getOrder(order.prevOrderIndex).nextOrderIndex = order.nextOrderIndex;
        } else {
            // Target item sat at the front of queue header; shift tracking link pointer downwards
            if (side == Side::BUY) bids[price].headOrderIndex = order.nextOrderIndex;
            else                   asks[price].headOrderIndex = order.nextOrderIndex;
        }

        // Step 2: Delink backward neighbor connections
        if (order.nextOrderIndex != -1) {
            pool.getOrder(order.nextOrderIndex).prevOrderIndex = order.prevOrderIndex;
        } else {
            // Target item sat at the back edge of queue; lift tail pointer boundary upwards
            if (side == Side::BUY) bids[price].tailOrderIndex = order.prevOrderIndex;
            else                   asks[price].tailOrderIndex = order.prevOrderIndex;
        }

        // Step 3: Recalculate book volumes and manage boundary cursors if hot limits were updated
        if (side == Side::BUY) {
            bids[price].totalVolume -= order.quantity;
            if (price == bestBidPrice && bids[price].headOrderIndex == -1) {
                while (bestBidPrice > 0 && bids[bestBidPrice].headOrderIndex == -1) bestBidPrice--;
            }
        } else {
            asks[price].totalVolume -= order.quantity;
            if (price == bestAskPrice && asks[price].headOrderIndex == -1) {
                while (bestAskPrice < MAX_PRICE_TICKS && asks[bestAskPrice].headOrderIndex == -1) bestAskPrice++;
            }
        }
        orderMap[orderId] = -1;  // Erase index references tracking allocation maps
        pool.freeOrder(index);   // Recycle block nodes back inside spatial data pool containers
    }
};

// ============================================================================
// 3. STANDARD TEMPLATE LIBRARY (STL) BASELINE TRADING SERVER SPECIFICATION
// ============================================================================

// Standard comparative target engine blueprint. Uses naive associative allocations.
// Under the hood, std::map relies on balanced Red-Black tree architectures ($O(\log N)$ inserts/deletes).
class NaiveSTLOrderBook {
private:
    std::map<uint32_t, std::deque<Order>, std::greater<uint32_t>> bids; // Sorted descending (highest bid first)
    std::map<uint32_t, std::deque<Order>> asks;                         // Sorted ascending (lowest ask first)
public:
    void add_order(uint64_t ts, Side side, uint32_t price, uint32_t qty) {
        if (side == Side::BUY) {
            while (qty > 0 && !asks.empty()) {
                auto best_ask = asks.begin(); // O(1) access to first node element pointer bounds
                if (price >= best_ask->first) {
                    auto& queue = best_ask->second;
                    while (qty > 0 && !queue.empty()) {
                        auto& resting = queue.front();
                        uint32_t traded = std::min(qty, resting.quantity);
                        qty -= traded;
                        resting.quantity -= traded;
                        if (resting.quantity == 0) queue.pop_front(); // Deallocates heap memory node boundaries on pop
                    }
                    if (queue.empty()) asks.erase(best_ask); // Node split/rebalance operations occur across tree layout
                } else break;
            }
            // Dynamic allocation occurs every time an order rests in the book
            if (qty > 0) bids[price].push_back({ts, price, qty, side, -1, -1, true});
        } else {
            while (qty > 0 && !bids.empty()) {
                auto best_bid = bids.begin();
                if (price <= best_bid->first) {
                    auto& queue = best_bid->second;
                    while (qty > 0 && !queue.empty()) {
                        auto& resting = queue.front();
                        uint32_t traded = std::min(qty, resting.quantity);
                        qty -= traded;
                        resting.quantity -= traded;
                        if (resting.quantity == 0) queue.pop_front();
                    }
                    if (queue.empty()) bids.erase(best_bid);
                } else break;
            }
            if (qty > 0) asks[price].push_back({ts, price, qty, side, -1, -1, true});
        }
    }
};

// ============================================================================
// 4. TAIL PERCENTILE STATISTICAL DISTRIBUTION CALCULATION MECHANICS
// ============================================================================

// Custom statistical calculators attached to the Google Benchmark harness execution tracking tables.
// Sorts the aggregated timing vectors generated across separate runs to extract specific percentiles.

auto p50 = [](const std::vector<double>& v) -> double {
    std::vector<double> c = v; std::sort(c.begin(), c.end());
    return c[static_cast<size_t>(c.size() * 0.50)]; // Extracts Median value performance index
};
auto p90 = [](const std::vector<double>& v) -> double {
    std::vector<double> c = v; std::sort(c.begin(), c.end());
    return c[static_cast<size_t>(c.size() * 0.90)]; // Bound marker where the worst 10% of latency deviations begin
};
auto p99 = [](const std::vector<double>& v) -> double {
    std::vector<double> c = v; std::sort(c.begin(), c.end());
    return c[static_cast<size_t>(c.size() * 0.99)]; // The Tail Latency metric indicating execution delays under extreme market pressure
};


// ============================================================================
// 5. SCIENTIFIC HIGH-FREQUENCY SYSTEM BENCHMARK HARNESS SCENARIOS
// ============================================================================

// BATTLE 1: Naive STL Baseline evaluation harness.
static void BM_Naive_STL_Crossing(benchmark::State& state) {
    uint64_t id = 1;
    
    // 1. SETUP PHASE (Outside the stopwatch loop)
    // Create the engine ONCE. Since we perfectly cross the orders in the loop,
    // the book will automatically empty itself out each iteration.
    auto naive_engine = std::make_unique<NaiveSTLOrderBook>();

    // 2. HOT LOOP (Pure execution, zero stopwatch manipulation)
    for (auto _ : state) {
        // Execute the crossing transaction
        naive_engine->add_order(id++, Side::SELL, 105, 10);
        naive_engine->add_order(id++, Side::BUY, 105, 10);
        
        // Force the compiler to actually do the work
        benchmark::DoNotOptimize(naive_engine);
    }
    
    // 3. TELEMETRY
    state.SetItemsProcessed(state.iterations() * 2);
}

// BATTLE 2: Optimized HFT layout isolated matching performance velocity test.
static void BM_HFT_100Pct_Crossing(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    uint64_t id = 1;

    for (auto _ : state) {
        engine->addSellOrder(id++, 105, 10); // Allocates a slot, places order at 105 tick location
        engine->addBuyOrder(id++, 105, 10);  // Matches the order, clears out state records
        benchmark::DoNotOptimize(engine);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
// SCENARIO: 100% Crossing WITH Market-Maker Sentinels
// This tests real-world crossing where liquidity anchors prevent the O(N) pathological scan.
static void BM_HFT_100pct_With_Sentinels(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    
    // SENTINELS: Simulating market-maker bounds 1-tick away from the action
    engine->addSellOrder(999998, 10001, 1);
    engine->addBuyOrder(999999, 9999, 1);

    uint64_t base_id = 1;
    constexpr uint64_t MASK_262K = 262143;
    for (auto _ : state) {
        uint64_t sellId = (base_id & MASK_262K) + 1;
        uint64_t buyId = sellId + 262144;
        base_id++;

        engine->addSellOrder(sellId, 10000, 100);
        engine->addBuyOrder(buyId, 10000, 100);

        benchmark::DoNotOptimize(buyId);
        benchmark::DoNotOptimize(sellId);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}


    
// BATTLE 3: Pathological "Sparse Book Scan" worst-case stress test scenario evaluation.
static void BM_HFT_Sparse_Book_Scan(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    uint64_t id = 1;

    for (auto _ : state) {
        // Crosses orders at tick 15,105, then clears the tier.
        // Forces the internal loop engine to scan nearly 185,000 array indices down to zero to update cursors.
        engine->addSellOrder(id++, 15105, 10);
        engine->addBuyOrder(id++, 15105, 10); 
        benchmark::DoNotOptimize(engine);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// BATTLE 4: Measure latency overhead tracking order cancellations via internal mapping tables.
static void BM_HFT_Order_Cancellation(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    uint64_t id = 1;

    for (auto _ : state) {
        engine->addSellOrder(id, 105, 10); // Create order item node entry boundaries
        engine->cancelOrder(id);           // Delink structural pointer references in true O(1) execution space
        id++;
        benchmark::DoNotOptimize(engine);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// BATTLE 5: Comprehensive Realistic HFT Market Simulation model under multi-threaded hardware contention.
static void BM_HFT_Realistic_Market(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    
    std::atomic<bool> keepRunning{true};
    
    // SPIN CONSUMER THREAD: Spawns off core to actively consume output data pipeline tracking logs
    std::thread loggerThread([&]() {
        TradeMsg msg;
        while (keepRunning.load(std::memory_order_relaxed)) {
            while (tradeLog->pop(msg)) {} // Pull transaction data out as fast as it appears
            
            // CONCURRENCY CONTROVERSY RESOLVED: Injects explicit platform system context yielding directives.
            // Relinquishes thread priority back to core schedules, ensuring logging actions don't saturate execution caches.
            std::this_thread::yield(); 
        }
    });

    // HARNESS WARMUP ENVIRONMENT PREPARATION: Pre-generates random distributions outside the timed loop.
    // This removes the random number generation latency from the final engine scoring tables.
    struct RandomOrder { uint64_t id; uint32_t price; Side side; };
    std::vector<RandomOrder> traffic;
    traffic.reserve(100000);
    
    std::mt19937 rng(42); // Mersenne Twister engine seeded deterministically to allow reproducible execution sequences
    std::uniform_int_distribution<uint32_t> marketPriceGen(9800, 10200);
    std::uniform_int_distribution<int> sideGen(0, 1);
    uint64_t id_counter = 1;

    for(int i = 0; i < 100000; i++) {
        traffic.push_back({id_counter++, marketPriceGen(rng), sideGen(rng) == 0 ? Side::BUY : Side::SELL});
    }

    // Injects thick market background buffers to establish dense resting volume boundaries inside array layers
    for (int i = 0; i < 50000; ++i) {
        engine->addBuyOrder(id_counter++, 9500, 100);
        engine->addSellOrder(id_counter++, 10500, 100);
    }

    size_t traffic_idx = 0;

    // THE EXPERIMENT EXECUTION LOOP
    for (auto _ : state) {
        const auto& order = traffic[traffic_idx];
        if (order.side == Side::BUY) {
            engine->addBuyOrder(order.id, order.price, 100);
        } else {
            engine->addSellOrder(order.id, order.price, 100);
        }
        
        traffic_idx = (traffic_idx + 1) % 100000; // Circular navigation bounds indexing mechanics over pre-generated vector array
        benchmark::DoNotOptimize(engine);
    }

    // TEARDOWN PROCESS CLEANUPS
    keepRunning.store(false, std::memory_order_release); // Signal background loop to break out
    loggerThread.join();                                 // Merge core execution structures back neatly
    state.SetItemsProcessed(state.iterations());
}

// ============================================================================
// 6. INDUSTRIAL CODE HARNESS EVALUATION SCHEDULER MACROS
// ============================================================================

// Custom test configuration wrapper macro setup. 
// MinTime(0.1) specifies that each discrete experiment phase executes matching tracks for at least 100 milliseconds.
// Repetitions(50) forces Google Benchmark to evaluate 50 unique complete test pass iterations.
// This feeds 50 distinct performance metrics into the lambda formulas to guarantee accurate p99 tails.
// DisplayAggregatesOnly(true) condenses table output lines by showing purely summarized data matrices.
#define REGISTER_HFT_BENCHMARK(func) \
    BENCHMARK(func)->Unit(benchmark::kNanosecond) \
                 ->MinTime(0.1) \
                 ->Repetitions(50) \
                 ->ComputeStatistics("p50", p50) \
                 ->ComputeStatistics("p90", p90) \
                 ->ComputeStatistics("p99", p99) \
                 ->DisplayAggregatesOnly(true)

// Explicit orchestration mappings compiling our performance profiles
// Explicit orchestration mappings compiling our performance profiles
REGISTER_HFT_BENCHMARK(BM_HFT_100Pct_Crossing);
REGISTER_HFT_BENCHMARK(BM_HFT_100pct_With_Sentinels);  
REGISTER_HFT_BENCHMARK(BM_HFT_Sparse_Book_Scan);
REGISTER_HFT_BENCHMARK(BM_HFT_Order_Cancellation);
REGISTER_HFT_BENCHMARK(BM_Naive_STL_Crossing);
REGISTER_HFT_BENCHMARK(BM_HFT_Realistic_Market);
// Triggers entry points compiling main setup wrappers inside standard google test frame configurations
BENCHMARK_MAIN();