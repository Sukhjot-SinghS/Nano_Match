#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <string>
#include <chrono>

using namespace std;

const int NUM_ROWS = 5000000;
const uint64_t START_TIMESTAMP = 1700000000000; // Simulated epoch time

void generate_realistic_dataset(const string& filename) {
    ofstream file(filename);
    if (!file.is_open()) {
        cerr << "Failed to open " << filename << "\n";
        return;
    }

    // Write header (Optional, but helps with standard CSV viewers)
    file << "Timestamp,Type,OrderID,Side,Price,Quantity\n";

    mt19937 rng(42); // Fixed seed for reproducible benchmarks
    uniform_real_distribution<double> action_dist(0.0, 1.0);
    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<int> passive_spread(1, 10);
    uniform_int_distribution<int> aggressive_spread(1, 5);
    uniform_int_distribution<int> qty_dist(10, 500);
    uniform_int_distribution<int> time_jump(10, 100);

    vector<uint64_t> active_ids; // Tracks resting orders for O(1) random cancellations
    active_ids.reserve(NUM_ROWS);

    uint64_t current_ts = START_TIMESTAMP;
    uint64_t order_id_counter = 1;
    uint32_t base_price = 10000;

    for (int i = 0; i < NUM_ROWS; ++i) {
        current_ts += time_jump(rng);

        // 90% Cancel Rate (Only if we actually have active orders to cancel)
        if (!active_ids.empty() && action_dist(rng) < 0.90) {
            // O(1) Random Deletion
            uniform_int_distribution<int> index_dist(0, active_ids.size() - 1);
            int cancel_idx = index_dist(rng);
            uint64_t cancel_id = active_ids[cancel_idx];

            // Swap with the back and pop (O(1) removal from vector)
            active_ids[cancel_idx] = active_ids.back();
            active_ids.pop_back();

            // Format: Timestamp, X, OrderID (other fields empty/ignored)
            file << current_ts << ",X," << cancel_id << ",,,\n";
        } 
        else {
            // Add New Order (10% of the time, or if book is empty)
            char side = (side_dist(rng) == 0) ? 'B' : 'S';
            uint32_t price = base_price;
            uint32_t qty = qty_dist(rng);

            // 80/20 Spread Logic
            bool is_aggressive = (action_dist(rng) < 0.20);
            
            if (side == 'B') {
                price = is_aggressive ? (base_price + aggressive_spread(rng)) : (base_price - passive_spread(rng));
            } else {
                price = is_aggressive ? (base_price - aggressive_spread(rng)) : (base_price + passive_spread(rng));
            }

            file << current_ts << ",A," << order_id_counter << "," << side << "," << price << "," << qty << "\n";
            active_ids.push_back(order_id_counter);
            order_id_counter++;
        }
    }
    cout << "Generated " << filename << " (" << NUM_ROWS << " rows).\n";
}

void generate_pathological_dataset(const string& filename) {
    ofstream file(filename);
    if (!file.is_open()) return;

    file << "Timestamp,Type,OrderID,Side,Price,Quantity\n";

    mt19937 rng(1337);
    uniform_int_distribution<int> qty_dist(10, 100);
    uniform_int_distribution<int> time_jump(10, 100);

    uint64_t current_ts = START_TIMESTAMP;
    uint64_t order_id_counter = 1;

    // Shotgun Prices: Alternate between 100 and 90,000 to force max-distance scans
    uint32_t low_price = 100;
    uint32_t high_price = 90000;

    for (int i = 0; i < NUM_ROWS; ++i) {
        current_ts += time_jump(rng);
        
        // 0% Cancels. Pure Add/Match chaos.
        // We alternate Buy and Sell, and we alternate extreme prices to force crosses that empty the levels.
        char side = (i % 2 == 0) ? 'B' : 'S';
        uint32_t price = (i % 4 < 2) ? low_price : high_price; // Swaps every 2 orders
        uint32_t qty = qty_dist(rng);

        file << current_ts << ",A," << order_id_counter << "," << side << "," << price << "," << qty << "\n";
        order_id_counter++;
    }
    cout << "Generated " << filename << " (" << NUM_ROWS << " rows).\n";
}
void generate_dense_equilibrium_dataset(const string& filename) {
    ofstream file(filename);
    if (!file.is_open()) {
        cerr << "Failed to open " << filename << "\n";
        return;
    }
 
    // Write header matching nano.cpp's 6-column expectation
    file << "Timestamp,Type,OrderID,Side,Price,Quantity\n";
 
    mt19937 rng(9999); // Different seed for Dataset C (dense equilibrium)
    uniform_real_distribution<double> action_dist(0.0, 1.0);
    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<uint32_t> price_buy(9995, 10000);    // Tight bid side
    uniform_int_distribution<uint32_t> price_sell(10000, 10005);  // Tight ask side
    uniform_int_distribution<uint32_t> qty_dist(10, 1000);
    uniform_int_distribution<int> time_jump(5, 50);               // Faster order flow
 
    vector<uint64_t> active_orders; // Sliding window of resting orders
    active_orders.reserve(10000);   // Keep ~10k active orders in memory
 
    uint64_t current_ts = START_TIMESTAMP;
    uint64_t order_id_counter = 1;
 
    for (int i = 0; i < NUM_ROWS; ++i) {
        current_ts += time_jump(rng);
 
        // 20% Cancel Rate — lighter than Dataset A's 90%, creates dense book
        if (!active_orders.empty() && action_dist(rng) < 0.20) {
            // O(1) Random Deletion from sliding window
            uniform_int_distribution<int> index_dist(0, active_orders.size() - 1);
            int cancel_idx = index_dist(rng);
            uint64_t cancel_id = active_orders[cancel_idx];
 
            // Swap with the back and pop (O(1) removal from vector)
            active_orders[cancel_idx] = active_orders.back();
            active_orders.pop_back();
 
            // Format: Timestamp, X, OrderID (other fields empty/ignored)
            file << current_ts << ",X," << cancel_id << ",,,\n";
        } 
        else {
            // 80% Add orders — continuous flow
            char side = (side_dist(rng) == 0) ? 'B' : 'S';
            uint32_t price = (side == 'B') ? price_buy(rng) : price_sell(rng);
            uint32_t qty = qty_dist(rng);
 
            // Format: Timestamp, A, OrderID, Side, Price, Quantity
            file << current_ts << ",A," << order_id_counter << "," << side << "," << price << "," << qty << "\n";
            active_orders.push_back(order_id_counter);
            order_id_counter++;
 
            // Keep memory usage light — sliding window of last 10k orders
            if (active_orders.size() > 10000) {
                active_orders.erase(active_orders.begin());
            }
        }
    }
    cout << "Generated " << filename << " (" << NUM_ROWS << " rows).\n";
}
int main() {
    cout << "Starting Data Generation Pipeline...\n";
    
    auto start = chrono::high_resolution_clock::now();
    
    generate_realistic_dataset("dataset_A_realistic.csv");
    generate_pathological_dataset("dataset_B_pathological.csv");
    generate_dense_equilibrium_dataset("dataset_C_dense_equilibrium.csv");
    auto end = chrono::high_resolution_clock::now();
    cout << "Total generation time: " 
         << chrono::duration_cast<chrono::milliseconds>(end - start).count() 
         << " ms\n";
         
    return 0;
}