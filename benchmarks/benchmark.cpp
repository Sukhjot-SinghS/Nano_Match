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
#include <list> 
#include <queue>
#include <benchmark/benchmark.h>
#include <x86intrin.h> 

using namespace std;

// ============================================================================
// 1. LOW-LEVEL MEMORY ALIGNMENTS & HIGH-FREQUENCY TRADING DATA STRUCTURES
// ============================================================================

enum class Side : uint8_t { BUY, SELL };

struct TradeMsg {
    uint64_t buyerOrderId;
    uint64_t sellerOrderId;
    uint32_t price;
    uint32_t quantity;
};

struct alignas(32) Order {
    uint64_t orderId;        
    uint32_t price;          
    uint32_t quantity;       
    Side side;               
    int32_t nextOrderIndex = -1; 
    int32_t prevOrderIndex = -1; 
    bool isActive = false;   
};

struct PriceLevel {
    int32_t headOrderIndex = -1; 
    int32_t tailOrderIndex = -1; 
    uint64_t totalVolume = 0;    
};

template<size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Ring Buffer Capacity must be a power of 2!");

private:
    std::array<TradeMsg, Capacity> buffer; 
    alignas(64) std::atomic<size_t> head{0}; 
    alignas(64) std::atomic<size_t> tail{0}; 

public:
    inline bool push(const TradeMsg& msg) noexcept {
        size_t current_head = head.load(std::memory_order_relaxed);
        size_t next_head = (current_head + 1) & (Capacity - 1);
        if (next_head == tail.load(std::memory_order_acquire)) return false; 
        buffer[current_head] = msg; 
        head.store(next_head, std::memory_order_release);
        return true;
    }

    inline bool pop(TradeMsg& msg) noexcept {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        if (current_tail == head.load(std::memory_order_acquire)) return false; 
        msg = buffer[current_tail]; 
        tail.store((current_tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }
};

class OrderPool {
private:
    std::vector<Order> pool;       
    std::vector<int32_t> freeList; 
public:
    OrderPool(size_t capacity) {
        pool.resize(capacity); 
        freeList.reserve(capacity);
        for (int32_t i = capacity - 1; i >= 0; --i) {
            freeList.push_back(i);
        }
    }
    inline int32_t allocateOrder() noexcept {
        if (freeList.empty()) return -1; 
        int32_t index = freeList.back();
        freeList.pop_back();
        return index;
    }
    inline void freeOrder(int32_t index) noexcept {
        pool[index].isActive = false; 
        freeList.push_back(index);    
    }
    inline Order& getOrder(int32_t index) noexcept { return pool[index]; }
};

// ============================================================================
// 2. ULTRA-LOW LATENCY EXTREME-OPTIMIZED LIMIT ORDER BOOK ENGINE
// ============================================================================
class LimitOrderBook {
private:
    static constexpr size_t MAX_PRICE_TICKS = 200000; 
    
    std::vector<PriceLevel> bids; 
    std::vector<PriceLevel> asks; 
    std::vector<int32_t> orderMap; 

    OrderPool pool;
    SPSCRingBuffer<131072>* tradeBuffer; 

    uint32_t bestBidPrice = 0;
    uint32_t bestAskPrice = MAX_PRICE_TICKS; 

    uint64_t total_bid_volume = 0;
    uint64_t total_ask_volume = 0;

public:
    LimitOrderBook(size_t maxOrders, SPSCRingBuffer<131072>* buffer) 
        : pool(maxOrders), tradeBuffer(buffer) {
        bids.resize(MAX_PRICE_TICKS);
        asks.resize(MAX_PRICE_TICKS);
        // FIX: Increased to 10M to match nano.cpp and prevent segfaults
        orderMap.assign(10000000, -1); 
    }
    
    inline void addBuyOrder(uint64_t orderId, uint32_t price, uint32_t quantity) noexcept {
        while (quantity > 0 && price >= bestAskPrice) {
            PriceLevel& bestAskLevel = asks[bestAskPrice];
            
            if (bestAskLevel.headOrderIndex == -1) [[unlikely]] {
                bestAskPrice++; 
                if (bestAskPrice >= MAX_PRICE_TICKS) break;
                continue;
            }

            int32_t currentAskIndex = bestAskLevel.headOrderIndex;
            Order& restingAsk = pool.getOrder(currentAskIndex);

            uint32_t tradeQty = std::min(quantity, restingAsk.quantity);
            quantity -= tradeQty;
            restingAsk.quantity -= tradeQty;
            bestAskLevel.totalVolume -= tradeQty;
            total_ask_volume -= tradeQty;

            if (tradeQty > 0 && tradeBuffer) {
                tradeBuffer->push({orderId, restingAsk.orderId, bestAskPrice, tradeQty});
            }

            if (restingAsk.quantity == 0) [[likely]] {
                bestAskLevel.headOrderIndex = restingAsk.nextOrderIndex; 
                if (bestAskLevel.headOrderIndex == -1) [[unlikely]] bestAskLevel.tailOrderIndex = -1;
                else pool.getOrder(bestAskLevel.headOrderIndex).prevOrderIndex = -1; 
                
                // FIX: Bounds check to prevent Segfault
                if (restingAsk.orderId < orderMap.size()) orderMap[restingAsk.orderId] = -1; 
                pool.freeOrder(currentAskIndex);   
            }

            if (bestAskLevel.headOrderIndex == -1) {
                if (total_ask_volume == 0) {
                    bestAskPrice = MAX_PRICE_TICKS;
                } else {
                    do { bestAskPrice++; } while (bestAskPrice < MAX_PRICE_TICKS && asks[bestAskPrice].headOrderIndex == -1);
                }
            }
        }

        if (quantity > 0) [[likely]] {
            int32_t newOrderIndex = pool.allocateOrder();
            if (newOrderIndex == -1) [[unlikely]] return; 
            
            // FIX: Bounds check to prevent Segfault
            if (orderId < orderMap.size()) orderMap[orderId] = newOrderIndex;

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
                bidLevel.headOrderIndex = newOrderIndex;
            } else [[likely]] {
                Order& oldTail = pool.getOrder(bidLevel.tailOrderIndex);
                oldTail.nextOrderIndex = newOrderIndex;
                newOrder.prevOrderIndex = bidLevel.tailOrderIndex; 
            }
            bidLevel.tailOrderIndex = newOrderIndex;
            bidLevel.totalVolume += quantity;
            total_bid_volume += quantity; 

            if (price > bestBidPrice) bestBidPrice = price;
        }
    }

    inline void addSellOrder(uint64_t orderId, uint32_t price, uint32_t quantity) noexcept {
        while (quantity > 0 && price <= bestBidPrice) {
            PriceLevel& bestBidLevel = bids[bestBidPrice];
            if (bestBidLevel.headOrderIndex == -1) [[unlikely]] {
                if (bestBidPrice == 0) break; 
                bestBidPrice--; 
                continue;
            }

            int32_t currentBidIndex = bestBidLevel.headOrderIndex;
            Order& restingBid = pool.getOrder(currentBidIndex);

            uint32_t tradeQty = std::min(quantity, restingBid.quantity);
            quantity -= tradeQty;
            restingBid.quantity -= tradeQty;
            bestBidLevel.totalVolume -= tradeQty;
            total_bid_volume -= tradeQty; 

            if (tradeQty > 0 && tradeBuffer) {
                tradeBuffer->push({restingBid.orderId, orderId, bestBidPrice, tradeQty});
            }

            if (restingBid.quantity == 0) [[likely]] {
                bestBidLevel.headOrderIndex = restingBid.nextOrderIndex;
                if (bestBidLevel.headOrderIndex == -1) [[unlikely]] bestBidLevel.tailOrderIndex = -1;
                else pool.getOrder(bestBidLevel.headOrderIndex).prevOrderIndex = -1;
                
                if (restingBid.orderId < orderMap.size()) orderMap[restingBid.orderId] = -1;
                pool.freeOrder(currentBidIndex);
            }

            if (bestBidLevel.headOrderIndex == -1) {
                if (total_bid_volume == 0) {
                    bestBidPrice = 0;
                } else {
                    while (bestBidPrice > 0 && bids[bestBidPrice].headOrderIndex == -1) bestBidPrice--;
                }
            }
        }

        if (quantity > 0) [[likely]] {
            int32_t newOrderIndex = pool.allocateOrder();
            if (newOrderIndex == -1) [[unlikely]] return; 
            
            if (orderId < orderMap.size()) orderMap[orderId] = newOrderIndex;

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
            total_ask_volume += quantity; 

            if (price < bestAskPrice) bestAskPrice = price;
        }
    }

    inline void cancelOrder(uint64_t orderId) noexcept {
        if (orderId >= orderMap.size()) return;
        int32_t index = orderMap[orderId];
        if (index == -1) return; 

        Order& order = pool.getOrder(index);
        if (!order.isActive) return;

        uint32_t price = order.price;
        Side side = order.side;

        if (order.prevOrderIndex != -1) {
            pool.getOrder(order.prevOrderIndex).nextOrderIndex = order.nextOrderIndex;
        } else {
            if (side == Side::BUY) bids[price].headOrderIndex = order.nextOrderIndex;
            else                   asks[price].headOrderIndex = order.nextOrderIndex;
        }

        if (order.nextOrderIndex != -1) {
            pool.getOrder(order.nextOrderIndex).prevOrderIndex = order.prevOrderIndex;
        } else {
            if (side == Side::BUY) bids[price].tailOrderIndex = order.prevOrderIndex;
            else                   asks[price].tailOrderIndex = order.prevOrderIndex;
        }

        if (side == Side::BUY) {
            bids[price].totalVolume -= order.quantity;
            total_bid_volume -= order.quantity;
            if (price == bestBidPrice && bids[price].headOrderIndex == -1) {
                if (total_bid_volume == 0) bestBidPrice = 0;
                else while (bestBidPrice > 0 && bids[bestBidPrice].headOrderIndex == -1) bestBidPrice--;
            }
        } else {
            asks[price].totalVolume -= order.quantity;
            total_ask_volume -= order.quantity;
            if (price == bestAskPrice && asks[price].headOrderIndex == -1) {
                if (total_ask_volume == 0) bestAskPrice = MAX_PRICE_TICKS;
                else while (bestAskPrice < MAX_PRICE_TICKS && asks[bestAskPrice].headOrderIndex == -1) bestAskPrice++;
            }
        }
        orderMap[orderId] = -1;  
        pool.freeOrder(index);   
    }
};

// ============================================================================
// 3. STANDARD TEMPLATE LIBRARY (STL) BASELINE TRADING SERVER SPECIFICATION
// ============================================================================
class NaiveSTLOrderBook {
private:
    std::map<uint32_t, std::list<Order>, std::greater<uint32_t>> bids; 
    std::map<uint32_t, std::list<Order>> asks;                         
public:
    void add_order(uint64_t ts, Side side, uint32_t price, uint32_t qty) {
        if (side == Side::BUY) {
            while (qty > 0 && !asks.empty()) {
                auto best_ask = asks.begin(); 
                if (price >= best_ask->first) {
                    auto& queue = best_ask->second;
                    while (qty > 0 && !queue.empty()) {
                        auto& resting = queue.front();
                        uint32_t traded = std::min(qty, resting.quantity);
                        qty -= traded;
                        resting.quantity -= traded;
                        if (resting.quantity == 0) queue.pop_front(); 
                    }
                    if (queue.empty()) asks.erase(best_ask); 
                } else break;
            }
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
auto p50 = [](const std::vector<double>& v) -> double {
    std::vector<double> c = v; std::sort(c.begin(), c.end());
    return c[static_cast<size_t>(c.size() * 0.50)]; 
};
auto p90 = [](const std::vector<double>& v) -> double {
    std::vector<double> c = v; std::sort(c.begin(), c.end());
    return c[static_cast<size_t>(c.size() * 0.90)]; 
};
auto p99 = [](const std::vector<double>& v) -> double {
    std::vector<double> c = v; std::sort(c.begin(), c.end());
    return c[static_cast<size_t>(c.size() * 0.99)]; 
};

// ============================================================================
// 5. SCIENTIFIC HIGH-FREQUENCY SYSTEM BENCHMARK HARNESS SCENARIOS
// ============================================================================
struct RandomOrder { uint64_t id; uint32_t price; Side side; };
std::vector<RandomOrder> global_traffic;

void InitializeGlobalTraffic() {
    if (!global_traffic.empty()) return;
    
    global_traffic.reserve(5000000); 
    std::mt19937 rng(42); 
    std::uniform_int_distribution<uint32_t> marketPriceGen(9800, 10200);
    std::uniform_int_distribution<int> sideGen(0, 1);
    
    uint64_t id_counter = 1;
    for(int i = 0; i < 5000000; i++) {
        global_traffic.push_back({id_counter++, marketPriceGen(rng), sideGen(rng) == 0 ? Side::BUY : Side::SELL});
    }
}

// BATTLE 1: Naive STL Baseline evaluation harness.
static void BM_Naive_STL_Crossing(benchmark::State& state) {
    uint64_t id_counter = 1;
    constexpr uint64_t MASK = 1048575; // Recycle IDs to prevent memory explosion
    auto naive_engine = std::make_unique<NaiveSTLOrderBook>();

    for (auto _ : state) {
        uint64_t id = (id_counter & MASK) + 1;
        id_counter += 2;
        naive_engine->add_order(id, Side::SELL, 105, 10);
        naive_engine->add_order(id + 1, Side::BUY, 105, 10);
        benchmark::DoNotOptimize(naive_engine);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// BATTLE 2: Optimized HFT layout isolated matching performance velocity test.
static void BM_HFT_100Pct_Crossing(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    uint64_t id_counter = 1;
    constexpr uint64_t MASK = 1048575;

    for (auto _ : state) {
        uint64_t id = (id_counter & MASK) + 1;
        id_counter += 2;
        engine->addSellOrder(id, 105, 10); 
        engine->addBuyOrder(id + 1, 105, 10);  
        benchmark::DoNotOptimize(engine);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// SCENARIO: 100% Crossing WITH Market-Maker Sentinels
static void BM_HFT_100pct_With_Sentinels(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    
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
    uint64_t id_counter = 1;
    constexpr uint64_t MASK = 1048575;

    for (auto _ : state) {
        uint64_t id = (id_counter & MASK) + 1;
        id_counter += 2;
        engine->addSellOrder(id, 15105, 10);
        engine->addBuyOrder(id + 1, 15105, 10); 
        benchmark::DoNotOptimize(engine);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// BATTLE 4: Measure latency overhead tracking order cancellations via internal mapping tables.
static void BM_HFT_Order_Cancellation(benchmark::State& state) {
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(1000000, tradeLog.get()); 
    uint64_t id_counter = 1;
    constexpr uint64_t MASK = 1048575;

    for (auto _ : state) {
        uint64_t id = (id_counter++ & MASK) + 1;
        engine->addSellOrder(id, 105, 10); 
        engine->cancelOrder(id);           
        benchmark::DoNotOptimize(engine);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// BATTLE 5: Comprehensive Realistic HFT Market Simulation model under multi-threaded hardware contention.
static void BM_HFT_Realistic_Market(benchmark::State& state) {
    InitializeGlobalTraffic();
    auto tradeLog = std::make_unique<SPSCRingBuffer<131072>>(); 
    auto engine = std::make_unique<LimitOrderBook>(6000000, tradeLog.get()); 
    
    std::atomic<bool> keepRunning{true};
    std::thread loggerThread([&]() {
        TradeMsg msg;
        while (keepRunning.load(std::memory_order_relaxed)) {
            while (tradeLog->pop(msg)) {} 
            std::this_thread::yield(); 
        }
    });

    uint64_t id_counter = 1;

    for (int i = 0; i < 50000; ++i) {
        engine->addBuyOrder(id_counter++, 9500, 100);
        engine->addSellOrder(id_counter++, 10500, 100);
    }

    size_t traffic_idx = 0;

    for (auto _ : state) {
        const auto& order = global_traffic[traffic_idx];
        if (order.side == Side::BUY) {
            engine->addBuyOrder(order.id, order.price, 100);
        } else {
            engine->addSellOrder(order.id, order.price, 100);
        }
        traffic_idx = (traffic_idx + 1) % 5000000; 
        benchmark::DoNotOptimize(engine);
    }

    keepRunning.store(false, std::memory_order_release); 
    loggerThread.join();                                 
    state.SetItemsProcessed(state.iterations());
}

// BATTLE 6: Apples-to-Apples STL Baseline on the Cold Cache
static void BM_STL_Realistic_Market(benchmark::State& state) {
    InitializeGlobalTraffic();
    auto engine = std::make_unique<NaiveSTLOrderBook>();

    uint64_t id_counter = 1;
    for (int i = 0; i < 50000; ++i) {
        engine->add_order(id_counter++, Side::BUY, 9500, 100);
        engine->add_order(id_counter++, Side::SELL, 10500, 100);
    }

    size_t traffic_idx = 0;

    for (auto _ : state) {
        const auto& order = global_traffic[traffic_idx];
        engine->add_order(order.id, order.side, order.price, 100);
        
        traffic_idx = (traffic_idx + 1) % 5000000; 
        benchmark::DoNotOptimize(engine);
    }
    state.SetItemsProcessed(state.iterations());
}

#define REGISTER_HFT_BENCHMARK(func) \
    BENCHMARK(func)->Unit(benchmark::kNanosecond) \
                 ->MinTime(0.1) \
                 ->Repetitions(50) \
                 ->ComputeStatistics("p50", p50) \
                 ->ComputeStatistics("p90", p90) \
                 ->ComputeStatistics("p99", p99) \
                 ->DisplayAggregatesOnly(true)

REGISTER_HFT_BENCHMARK(BM_HFT_100Pct_Crossing);
REGISTER_HFT_BENCHMARK(BM_HFT_100pct_With_Sentinels);  
REGISTER_HFT_BENCHMARK(BM_HFT_Sparse_Book_Scan);
REGISTER_HFT_BENCHMARK(BM_HFT_Order_Cancellation);
REGISTER_HFT_BENCHMARK(BM_Naive_STL_Crossing);
REGISTER_HFT_BENCHMARK(BM_HFT_Realistic_Market);
REGISTER_HFT_BENCHMARK(BM_STL_Realistic_Market); 

BENCHMARK_MAIN();