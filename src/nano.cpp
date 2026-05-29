#include <iostream>
#include <vector>
#include <cstdint>
#include <fcntl.h>      // For open() - OS level file handling
#include <sys/mman.h>   // For mmap(), madvise() - Direct memory mapping
#include <sys/stat.h>   // For fstat() - Getting file sizes
#include <unistd.h>     // For close() - OS level file closing
#include <thread>       // For std::thread - Hardware multi-threading
#include <x86intrin.h>  // For __rdtsc() - Hardware CPU cycle counting
#include <atomic>       // For std::atomic - Lock-free synchronization
#include <algorithm>    // For std::min
#include <chrono>
#include <iomanip>
using namespace std;

// ============================================================================
// 1. CONSTANTS & TYPES
// ============================================================================
constexpr bool ENABLE_HOTPATH_LOGGING = false; // Set to true ONLY for debugging

constexpr int32_t  NULL_ORDER = -1;
constexpr uint32_t MAX_PRICE_TICKS = 100000; // Limits the array size

enum class Side : uint8_t {
    BUY,
    SELL
};

// ============================================================================
// 2. CACHE-ALIGNED DATA STRUCTURES
// ============================================================================
struct alignas(64) Order {
    uint64_t timestamp;   // 8 bytes (Kept for Tick-to-Trade latency logging)
    uint32_t price;       // 4 bytes
    uint32_t quantity;    // 4 bytes
    int32_t  next_id;     // 4 bytes
    int32_t  prev_id;     // 4 bytes
    Side     side;        // 1 byte
    bool     is_active;   // 1 byte
};

struct TradeRecord {
    uint64_t timestamp;
    uint32_t price;
    uint32_t quantity;
    Side side;
};

// ============================================================================
// 3. LOCK-FREE SPSC RING BUFFER
// ============================================================================
class LockFreeTradeLogger {
private:
    std::vector<TradeRecord> buffer;
    const size_t capacity;

    alignas(64) std::atomic<size_t> write_index{0};
    alignas(64) std::atomic<size_t> read_index{0};

public:
    LockFreeTradeLogger(size_t size) : capacity(size) {
        if ((capacity & (capacity - 1)) != 0 || capacity == 0) {
            std::cerr << "FATAL: Ring buffer capacity must be a power of 2!\n";
            exit(1);
        }
        buffer.resize(capacity);
    }

    bool is_empty() const {
        return read_index.load(std::memory_order_acquire)
            == write_index.load(std::memory_order_acquire);
    }

    bool push(const TradeRecord& trade) {
        const size_t current_write = write_index.load(std::memory_order_relaxed);
        const size_t next_write = (current_write + 1) & (capacity - 1);

        if (next_write == read_index.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        buffer[current_write] = trade; 
        write_index.store(next_write, std::memory_order_release);
        return true;
    }

    bool pop(TradeRecord& out_trade) {
        const size_t current_read = read_index.load(std::memory_order_relaxed);
        if (current_read == write_index.load(std::memory_order_acquire)) {
            return false; // Queue empty
        }
        out_trade = buffer[current_read];
        const size_t next_read = (current_read + 1) & (capacity - 1);
        read_index.store(next_read, std::memory_order_release);
        return true;
    }
};

// ============================================================================
// 4. O(1) CUSTOM MEMORY ALLOCATOR (Memory Pool)
// ============================================================================
class OrderPool {
private:
    std::vector<Order> pool;
    std::vector<int32_t> free_indices;

public:
    OrderPool(size_t capacity) {
        pool.resize(capacity);
        free_indices.reserve(capacity);
        for (int32_t i = capacity - 1; i >= 0; --i) {
            free_indices.push_back(i);
        }
        std::cout << "Engine: Allocated Memory Pool for " << capacity << " orders.\n";
    }

    int32_t allocate() {
        if (free_indices.empty()) return NULL_ORDER;
        int32_t id = free_indices.back();
        free_indices.pop_back();
        return id;
    }

    void deallocate(int32_t id) {
        pool[id].is_active = false;
        free_indices.push_back(id);
    }

    Order& get(int32_t id) { return pool[id]; }
};

// ============================================================================
// 5. THE LIMIT ORDER BOOK ENGINE
// ============================================================================
class LimitOrderBook {
private:
    OrderPool pool;
    LockFreeTradeLogger& trade_logger;

    std::vector<int32_t> bids_head;
    std::vector<int32_t> bids_tail;
    std::vector<int32_t> asks_head;
    std::vector<int32_t> asks_tail;

    std::vector<int32_t> order_map; 

    uint32_t best_bid = 0;
    uint32_t best_ask = MAX_PRICE_TICKS;
    
    uint64_t total_bid_volume = 0;
    uint64_t total_ask_volume = 0;

public:
    LimitOrderBook(size_t max_orders, LockFreeTradeLogger& logger)
        : pool(max_orders), trade_logger(logger) {
        bids_head.assign(MAX_PRICE_TICKS, NULL_ORDER);
        bids_tail.assign(MAX_PRICE_TICKS, NULL_ORDER);
        asks_head.assign(MAX_PRICE_TICKS, NULL_ORDER);
        asks_tail.assign(MAX_PRICE_TICKS, NULL_ORDER);
        
        order_map.assign(10000000, NULL_ORDER);
        std::cout << "Engine: Limit Order Book Initialized.\n";
    }

    void add_order(uint64_t timestamp, uint64_t order_id, Side side, uint32_t price, uint32_t qty) {
        if (side == Side::BUY) {
            match_buy(price, qty);
        } else {
            match_sell(price, qty);
        }

        if (qty == 0) return;

        int32_t new_id = pool.allocate();
        if (new_id == NULL_ORDER) {
            std::cerr << "Engine Error: Pool Full!\n";
            return;
        }

        if (order_id < order_map.size()) {
            order_map[order_id] = new_id; 
        }

        Order& new_order = pool.get(new_id);
        new_order.timestamp = timestamp;
        new_order.price = price;
        new_order.quantity = qty;
        new_order.side = side;
        new_order.is_active = true;
        new_order.next_id = NULL_ORDER;
        new_order.prev_id = NULL_ORDER;

        if (side == Side::BUY) {
            insert_bid(new_id, price);
            total_bid_volume += qty;
        } else {
            insert_ask(new_id, price);
            total_ask_volume += qty;
        }
    }

    void cancel_order(uint64_t order_id) {
        if (order_id >= order_map.size()) return;
        
        int32_t pool_id = order_map[order_id];
        if (pool_id == NULL_ORDER) return; 

        Order &order = pool.get(pool_id);
        if (!order.is_active) return;

        uint32_t price = order.price;
        Side side = order.side;

        int32_t p_id = order.prev_id;
        int32_t n_id = order.next_id;

        if (p_id != NULL_ORDER) {
            pool.get(p_id).next_id = n_id;
        } else {
            if (side == Side::BUY) bids_head[price] = n_id;
            else                   asks_head[price] = n_id;
        }

        if (n_id != NULL_ORDER) {
            pool.get(n_id).prev_id = p_id;
        } else {
            if (side == Side::BUY) bids_tail[price] = p_id;
            else                   asks_tail[price] = p_id;
        }

        order.prev_id = NULL_ORDER;
        order.next_id = NULL_ORDER;

        if (side == Side::BUY) {
            total_bid_volume -= order.quantity;
            if (price == best_bid && bids_head[price] == NULL_ORDER) {
                if (total_bid_volume == 0) best_bid = 0;
                else while (best_bid > 0 && bids_head[best_bid] == NULL_ORDER) best_bid--;
            }
        } else {
            total_ask_volume -= order.quantity;
            if (price == best_ask && asks_head[price] == NULL_ORDER) {
                if (total_ask_volume == 0) best_ask = MAX_PRICE_TICKS;
                else while (best_ask < MAX_PRICE_TICKS && asks_head[best_ask] == NULL_ORDER) best_ask++;
            }
        }

        pool.deallocate(pool_id);
        order_map[order_id] = NULL_ORDER;
    }

private:
    void match_buy(uint32_t incoming_price, uint32_t& incoming_qty) {
        while (incoming_qty > 0 && incoming_price >= best_ask) {
            int32_t resting_id = asks_head[best_ask];
            if (resting_id == NULL_ORDER) {
                best_ask++;
                if (best_ask > MAX_PRICE_TICKS) break;
                continue;
            }
            
            Order& resting_order = pool.get(resting_id);
            uint32_t trade_qty = std::min(resting_order.quantity, incoming_qty);
            incoming_qty -= trade_qty;
            resting_order.quantity -= trade_qty;
            total_ask_volume -= trade_qty;

            if constexpr (ENABLE_HOTPATH_LOGGING) {
                std::cout << "TRADE EXECUTED: " << trade_qty << " shares @ $" << best_ask << "\n";
            }
            
            TradeRecord receipt;
            receipt.timestamp = __rdtsc();
            receipt.price = best_ask;
            receipt.quantity = trade_qty;
            receipt.side = Side::BUY;
            trade_logger.push(receipt);

            if (resting_order.quantity == 0) {
                asks_head[best_ask] = resting_order.next_id;
                if (asks_head[best_ask] == NULL_ORDER) {
                    asks_tail[best_ask] = NULL_ORDER;
                    if (total_ask_volume == 0) {
                        best_ask = MAX_PRICE_TICKS;
                    } else {
                        while (best_ask < MAX_PRICE_TICKS && asks_head[best_ask] == NULL_ORDER) {
                            best_ask++;
                        }
                    }
                } else {
                    pool.get(asks_head[best_ask]).prev_id = NULL_ORDER;
                }
                pool.deallocate(resting_id);
            }
        }
    }

    void match_sell(uint32_t incoming_price, uint32_t& incoming_qty) {
        while (incoming_qty > 0 && incoming_price <= best_bid) {
            int32_t resting_id = bids_head[best_bid];
            if (resting_id == NULL_ORDER) {
                if (best_bid == 0) break;
                best_bid--;
                continue;
            }

            Order& resting_order = pool.get(resting_id);
            uint32_t trade_qty = std::min(incoming_qty, resting_order.quantity);
            incoming_qty -= trade_qty;
            resting_order.quantity -= trade_qty;
            total_bid_volume -= trade_qty;

            if constexpr (ENABLE_HOTPATH_LOGGING) {
                std::cout << "TRADE EXECUTED: " << trade_qty << " shares @ $" << best_bid << "\n";
            };
            
            TradeRecord receipt;
            receipt.timestamp = __rdtsc();
            receipt.price = best_bid;
            receipt.quantity = trade_qty;
            receipt.side = Side::SELL;
            trade_logger.push(receipt);

            if (resting_order.quantity == 0) {
                bids_head[best_bid] = resting_order.next_id;
                if (bids_head[best_bid] == NULL_ORDER) {
                    bids_tail[best_bid] = NULL_ORDER;
                    if (total_bid_volume == 0) {
                        best_bid = 0;
                    } else {
                        while (best_bid > 0 && bids_head[best_bid] == NULL_ORDER) {
                            best_bid--;
                        }
                    }
                } else {
                    pool.get(bids_head[best_bid]).prev_id = NULL_ORDER;
                }
                pool.deallocate(resting_id);
            }
        }
    }

    void insert_bid(int32_t new_id, uint32_t price) {
        Order& new_order = pool.get(new_id);
        if (bids_head[price] == NULL_ORDER) {
            bids_head[price] = new_id;
            bids_tail[price] = new_id;
        } else {
            int32_t tail_id = bids_tail[price];
            Order& tail_order = pool.get(tail_id);
            tail_order.next_id = new_id;
            new_order.prev_id = tail_id;
            bids_tail[price] = new_id;
        }
        if (price > best_bid) best_bid = price;
    }

    void insert_ask(int32_t new_id, uint32_t price) {
        Order& new_order = pool.get(new_id);
        if (asks_head[price] == NULL_ORDER) {
            asks_head[price] = new_id;
            asks_tail[price] = new_id;
        } else {
            int32_t tail_id = asks_tail[price];
            Order& order = pool.get(tail_id);
            order.next_id = new_id;
            new_order.prev_id = tail_id;
            asks_tail[price] = new_id;
        }
        if (price < best_ask) best_ask = price;
    }
};

// ============================================================================
// 6. ZERO-COPY FILE PARSER (Memory Mapping)
// ============================================================================
class MarketDataLoader {
private:
    int fd;
    size_t file_size;
    char* mapped_data;

    inline uint64_t fast_atoi64(const char** ptr) {
        uint64_t result = 0;
        while (**ptr >= '0' && **ptr <= '9') {
            result = (result * 10) + (**ptr - '0');
            (*ptr)++; 
        }
        return result;
    }

    inline uint32_t fast_atoi32(const char** ptr) {
        uint32_t result = 0;
        while (**ptr >= '0' && **ptr <= '9') {
            result = (result * 10) + (**ptr - '0');
            (*ptr)++;
        }
        return result;
    }

public:
    MarketDataLoader(const char* filepath) {
        fd = open(filepath, O_RDONLY);
        if (fd == -1) {
            std::cerr << "CRITICAL: Failed to open file " << filepath << ".\n";
            exit(1);
        }

        struct stat sb;
        fstat(fd, &sb);
        file_size = sb.st_size;

        if (file_size == 0) {
            std::cerr << "CRITICAL: Market data file is empty.\n";
            exit(1);
        }

        mapped_data = static_cast<char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (mapped_data == MAP_FAILED) {
            std::cerr << "CRITICAL: mmap failed.\n";
            exit(1);
        }

        madvise(mapped_data, file_size, MADV_SEQUENTIAL);
        std::cout << "Engine: Successfully mapped " << file_size << " bytes into RAM.\n";
    }

    ~MarketDataLoader() {
        munmap(mapped_data, file_size);
        close(fd);
    }

    uint64_t ingest_data(LimitOrderBook& engine) {
        const char* cursor = mapped_data;
        const char* end_of_file = mapped_data + file_size;
        uint64_t events_processed = 0;

        // Skip the CSV header row
        while (cursor < end_of_file && *cursor != '\n') cursor++;
        if (cursor < end_of_file) cursor++; 

        // SREEJA'S FORMAT: OrderID, Side, Price, Quantity
        // 'Side' is 'B' (Buy), 'S' (Sell), or 'C' (Cancel)
        while (cursor < end_of_file) {
            if (*cursor == '\n' || *cursor == '\r') {
                cursor++;
                continue;
            }

            uint64_t order_id = fast_atoi64(&cursor);
            cursor++; // Skip comma

            char side_char = *cursor;
            cursor += 2; // Skip Side char + comma

            if (side_char == 'C') {
                // It's a Cancel order
                engine.cancel_order(order_id);
                // Skip the remaining "0,0"
                while (cursor < end_of_file && *cursor != '\n') cursor++;
            } 
            else {
                // It's an Add order
                Side side = (side_char == 'B') ? Side::BUY : Side::SELL;
                
                uint32_t price = fast_atoi32(&cursor);
                cursor++; // Skip comma
                
                uint32_t qty = fast_atoi32(&cursor);

                // Pass 0 for timestamp since we dropped it
                engine.add_order(0, order_id, side, price, qty);
            }
            events_processed++;
        }
        return events_processed;
    }
};

// ============================================================================
// 7. MULTI-THREADING CONTROLLER
// ============================================================================
std::atomic<bool> market_is_open{true};

void run_background_logger(LockFreeTradeLogger& ring_buffer) {
    TradeRecord trade;
    uint64_t trades_logged = 0;

    // Pure Memory Counting: No slow Disk I/O!
    while (market_is_open.load(std::memory_order_acquire) || !ring_buffer.is_empty()) {
        if (ring_buffer.pop(trade)) {
            trades_logged++;
        } else {
            std::this_thread::yield();
        }
    }

    std::cout << "[Logger Thread] Shut down safely. Total trades executed & processed: " << trades_logged << "\n";
}

// ============================================================================
// 8. SYSTEM BOOTUP & SELF-PROFILING SEQUENCE
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./trading_server <dataset.csv>\n";
        return 1;
    }

    std::cout << "--- BOOTING NANOMATCH ENGINE (GOD MODE) ---\n";

    LockFreeTradeLogger trade_logger(1048576); 
    LimitOrderBook engine(5000000, trade_logger);
    std::thread logger_thread(run_background_logger, std::ref(trade_logger));

    MarketDataLoader data_loader(argv[1]);
    
    std::cout << "Engine: Ingesting dataset...\n";

    // --- START THE HARDWARE CLOCK ---
    auto start_time = std::chrono::high_resolution_clock::now();
    
    uint64_t total_events = data_loader.ingest_data(engine); 
    
    // --- STOP THE HARDWARE CLOCK ---
    auto end_time = std::chrono::high_resolution_clock::now();
    
    // CALCULATE THE METRICS
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    double duration_seconds = (duration_ms > 0) ? (duration_ms / 1000.0) : 0.001; 
    double throughput = total_events / duration_seconds;

    std::cout << "\n====================================================\n";
    std::cout << "              ENGINE PERFORMANCE REPORT             \n";
    std::cout << "====================================================\n";
    std::cout << " File Tested   : " << argv[1] << "\n";
    std::cout << " Events Processed: " << total_events << "\n";
    std::cout << " Total Time    : " << duration_ms << " milliseconds\n";
    std::cout << " Throughput    : " << std::fixed << std::setprecision(0) << throughput << " events / second\n";
    if(total_events > 0) {
        std::cout << " Avg Latency   : " << std::fixed << std::setprecision(2) << (duration_ms * 1000000.0) / total_events << " nanoseconds / event\n";
    }
    std::cout << "====================================================\n\n";

    market_is_open.store(false, std::memory_order_release);
    logger_thread.join();
    
    return 0;
}