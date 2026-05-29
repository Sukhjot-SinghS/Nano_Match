#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <string>
#include <chrono>

using namespace std;

const int NUM_ROWS = 5000000;

// ============================================================================
// DATASET A: REALISTIC MARKET (90% Cancels, Deep Book)
// ============================================================================
void generate_realistic_dataset(const string& filename) {
    ofstream file(filename);
    if (!file.is_open()) {
        cerr << "Failed to open " << filename << "\n";
        return;
    }

    // 4-Column Header to match Sreeja's script
    file << "OrderID,Side,Price,Quantity\n";

    mt19937 rng(42); 
    uniform_real_distribution<double> action_dist(0.0, 1.0);
    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<int> passive_spread(1, 10);
    uniform_int_distribution<int> aggressive_spread(1, 5);
    uniform_int_distribution<int> qty_dist(10, 500);

    vector<uint64_t> active_ids; 
    active_ids.reserve(NUM_ROWS);

    uint64_t order_id_counter = 1;
    uint32_t base_price = 10000;

    for (int i = 0; i < NUM_ROWS; ++i) {
        if (!active_ids.empty() && action_dist(rng) < 0.90) {
            // CANCEL ORDER
            uniform_int_distribution<int> index_dist(0, active_ids.size() - 1);
            int cancel_idx = index_dist(rng);
            uint64_t cancel_id = active_ids[cancel_idx];

            active_ids[cancel_idx] = active_ids.back();
            active_ids.pop_back();

            // Format: OrderID, C, 0, 0
            file << cancel_id << ",C,0,0\n";
        } 
        else {
            // ADD ORDER
            char side = (side_dist(rng) == 0) ? 'B' : 'S';
            uint32_t price = base_price;
            uint32_t qty = qty_dist(rng);

            bool is_aggressive = (action_dist(rng) < 0.20);
            if (side == 'B') {
                price = is_aggressive ? (base_price + aggressive_spread(rng)) : (base_price - passive_spread(rng));
            } else {
                price = is_aggressive ? (base_price - aggressive_spread(rng)) : (base_price + passive_spread(rng));
            }

            // Format: OrderID, Side, Price, Qty
            file << order_id_counter << "," << side << "," << price << "," << qty << "\n";
            
            active_ids.push_back(order_id_counter);
            order_id_counter++;
        }
    }
    cout << "Generated " << filename << " (" << NUM_ROWS << " rows).\n";
}

// ============================================================================
// DATASET B: PATHOLOGICAL SCAN (Worst-Case Empty Book Test)
// ============================================================================
void generate_pathological_dataset(const string& filename) {
    ofstream file(filename);
    if (!file.is_open()) return;

    file << "OrderID,Side,Price,Quantity\n";

    mt19937 rng(1337);
    uniform_int_distribution<int> qty_dist(10, 100);

    uint64_t order_id_counter = 1;
    uint32_t low_price = 100;
    uint32_t high_price = 90000;

    for (int i = 0; i < NUM_ROWS; ++i) {
        // Pure Add/Match chaos. No cancels.
        char side = (i % 2 == 0) ? 'B' : 'S';
        uint32_t price = (i % 4 < 2) ? low_price : high_price; 
        uint32_t qty = qty_dist(rng);

        file << order_id_counter << "," << side << "," << price << "," << qty << "\n";
        order_id_counter++;
    }
    cout << "Generated " << filename << " (" << NUM_ROWS << " rows).\n";
}

// ============================================================================
// DATASET C: DENSE EQUILIBRIUM (Tight Spread, High Throughput)
// ============================================================================
void generate_dense_equilibrium_dataset(const string& filename) {
    ofstream file(filename);
    if (!file.is_open()) {
        cerr << "Failed to open " << filename << "\n";
        return;
    }

    file << "OrderID,Side,Price,Quantity\n";

    mt19937 rng(9999); 
    uniform_real_distribution<double> action_dist(0.0, 1.0);
    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<uint32_t> price_buy(9995, 10000);    
    uniform_int_distribution<uint32_t> price_sell(10000, 10005);  
    uniform_int_distribution<uint32_t> qty_dist(10, 1000);

    vector<uint64_t> active_orders; 
    active_orders.reserve(10000);   

    uint64_t order_id_counter = 1;

    for (int i = 0; i < NUM_ROWS; ++i) {
        if (!active_orders.empty() && action_dist(rng) < 0.20) {
            // CANCEL ORDER
            uniform_int_distribution<int> index_dist(0, active_orders.size() - 1);
            int cancel_idx = index_dist(rng);
            uint64_t cancel_id = active_orders[cancel_idx];

            active_orders[cancel_idx] = active_orders.back();
            active_orders.pop_back();

            file << cancel_id << ",C,0,0\n";
        } 
        else {
            // ADD ORDER
            char side = (side_dist(rng) == 0) ? 'B' : 'S';
            uint32_t price = (side == 'B') ? price_buy(rng) : price_sell(rng);
            uint32_t qty = qty_dist(rng);

            file << order_id_counter << "," << side << "," << price << "," << qty << "\n";
            
            active_orders.push_back(order_id_counter);
            order_id_counter++;

            if (active_orders.size() > 10000) {
                active_orders.erase(active_orders.begin());
            }
        }
    }
    cout << "Generated " << filename << " (" << NUM_ROWS << " rows).\n";
}

// ============================================================================
// MAIN EXECUTION
// ============================================================================
int main() {
    cout << "Starting 4-Column Data Generation Pipeline...\n";
    
    auto start = chrono::high_resolution_clock::now();
    
    generate_realistic_dataset("../data/dataset_A_realistic.csv");
    generate_pathological_dataset("../data/dataset_B_pathological.csv");
    generate_dense_equilibrium_dataset("../data/dataset_C_dense_equilibrium.csv");
    
    auto end = chrono::high_resolution_clock::now();
    cout << "Total generation time: " 
         << chrono::duration_cast<chrono::milliseconds>(end - start).count() 
         << " ms\n";
        
    return 0;
}