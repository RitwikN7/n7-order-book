# n7-order-book

A deterministic, high-performance, low-latency **Limit Order Book (LOB)** and matching engine written in modern **C++23**.

Designed with institutional financial exchange principles: zero dynamic memory allocations on the hot path, cache-conscious data layout (64-byte cache-line aligned order nodes), composition-based architecture to eliminate object slicing, in-place union memory pools, open-addressing flat hash tables, PMR pool allocators, $O(1)$ cancellations via intrusive doubly-linked queues, strongly typed fixed-point price/quantity classes, and support for **Limit**, **Market**, **Stop**, and **Reserve** orders.

---

## Architecture & Data Structures

```
                             ┌──────────────────────────────────┐
                             │  ObjectPool<OrderNode> (Memory)  │
                             │  Pre-allocated contiguous RAM    │
                             │  (64B cache-line aligned slots)  │
                             └──────────────────────────────────┘
                                      │ acquire()   ▲ release()
                                      ▼             │
                               ┌──────────────┐     │
                               │  OrderNode   │─────┘
                               └──────────────┘
                                 ▲          ▲
        (O(1) lookup by ID)      │          │ (Intrusive FIFO queue links)
                                 │          │
    ┌──────────────────────────────┐      ┌──────────────────────────────────┐
    │     orders_map_ (Lookup)     │      │   Active (bids_ & asks_) &       │
    │  FlatHashMap<OrderID, Node*> │      │   Dormant Stop Trees             │
    └──────────────────────────────┘      │   pmr::map<Price, PriceLevel>    │
                                          └──────────────────────────────────┘
```

### 1. 64-Byte Cache-Aligned `OrderNode` & Slicing Elimination via Composition
- **Composition over Inheritance**: `OrderNode` composes `OrderData data` as a member (`struct OrderNode { OrderData data; Quantity reserve_quantity; OrderNode* prev; OrderNode* next; }`). Removing inheritance guarantees **zero object slicing at compile time**—assigning a node to an `OrderData` is strictly caught by the compiler.
- **Cache-Line Alignment**: Decorated with `alignas(64)`, `sizeof(OrderNode) == 64` and `alignof(OrderNode) == 64`. Every order node sits on its own dedicated 64-byte L1 cache line, eliminating cache-line crossings and false sharing.
- **Compile-Time Verification**: Enforced via `static_assert(sizeof(OrderNode) == 64)` and `static_assert(alignof(OrderNode) == 64)`.

### 2. Zero-Allocation `ObjectPool<T>` with In-Place `union Slot`
- **Mechanism**: Pre-allocates contiguous memory blocks at startup. Uses an in-place `union Slot` where the free-list pointer (`Slot* next`) directly overlays the inactive storage array (`std::array<std::byte, sizeof(T)>`).
- **Zero Overhead Pooling**: `sizeof(Slot) == 64` and `alignof(Slot) == 64`. Memory footprint for 1.2M orders is reduced from ~153 MB down to 76.8 MB with zero cross-cache-line boundary traversals.
- **Latency**: Sub-10ns $O(1)$ `acquire()` and `release()` with zero system calls (`malloc`/`free`).
- **Compile-Time Elision**: Destructor calls are completely eliminated at compile time for trivially destructible types (`std::is_trivially_destructible_v<T>`).

### 3. $O(1)$ Open-Addressing Flat Hash Index (`FlatHashMap`)
- Custom cache-line friendly flat hash table with linear probing and power-of-two capacity bitmasking.
- Uses backward-shift erasure to eliminate tombstones and maintain optimal probe sequences.
- Eliminates node heap allocations and pointer chasing present in standard `std::unordered_map`.

### 4. Zero-Allocation Price Levels with PMR Pool Allocators (`PriceLevel`)
- **Active Bids**: Sorted descending using `std::pmr::map<Price, PriceLevel, std::greater<>>` (highest price at `bids_.begin()`).
- **Active Asks**: Sorted ascending using `std::pmr::map<Price, PriceLevel, std::less<>>` (lowest price at `asks_.begin()`).
- **Dormant Stop Orders**: Stored in `stop_bids_` (ascending) and `stop_asks_` (descending). When no stop orders are resting, evaluations are short-circuited in 1 CPU cycle via a zero-cost fast path (`stop_order_count_ == 0`).
- **PMR Memory Pool**: Allocates tree nodes from a local `std::pmr::unsynchronized_pool_resource`, eliminating global memory lock contention.
- **Intrusive Doubly-Linked Lists**: Each `PriceLevel` maintains `head` and `tail` pointers to `OrderNode`s. Because `prev` and `next` pointers live directly inside `OrderNode` (intrusive), unlinking an order takes $O(1)$ time with **zero extra heap node allocations**.

### 5. Strongly Typed Fixed-Point Arithmetic ($10^4$ Precision)
- **`Price` Class**: Strongly typed wrapper around `std::uint32_t` (4 bytes). Prices are strictly non-negative, supporting values up to \$429,496.7295 scaled by $10^4$ (10,000 ticks per \$1.0000).
- **`Quantity` Class**: Strongly typed wrapper around `std::uint64_t` (8 bytes).
- **Type Safety**: Explicit single-argument constructors and standard `<compare>` three-way comparison operator (`<=>`) prevent accidental swapping of price and quantity arguments at compile time.

### 6. Extended Order Types
- **Reserve Orders**: Allows an institutional participant to display a smaller visible slice (`display_quantity`) while hiding the remainder (`reserve_quantity`). When the visible slice is filled, it automatically replenishes from reserve and re-queues to the **tail** of the price level, preserving fair FIFO time priority for other resting participants.
- **Stop Orders**: Supports `STOP_MARKET` and `STOP_LIMIT` orders. Dormant stop orders rest in priority maps until execution trades cross the trigger price against `last_traded_price_`. Triggering is handled iteratively without recursive stack overhead.
- **Universal `OrderBuilder`**: Fluent, zero-overhead inline builder (`.limit()`, `.market()`, `.stopMarket()`, `.stopLimit()`, `.reserve()`) with compile-time invariant verification (`.buildChecked()`).

---

## Core Algorithms & Complexity

- **Zero Heap Allocations on Hot Path**: All core trading operations execute with **0 dynamic heap allocations** (`malloc`/`free`) via the pre-allocated `ObjectPool` and PMR pool resource.

| Operation | Method | Time Complexity | Details |
| :--- | :--- | :--- | :--- |
| **Add Limit Order** | `addOrder(order)` | $O(\log P)$ price lookup + $O(1)$ queue append | Fast level insertion or FIFO queue append |
| **Match Market Order** | `addOrder(order)` | $O(M)$ where $M$ is matched fills | Sweeps price levels; triggers stops if active |
| **Reserve Replenishment** | `addOrder(order)` | $O(1)$ queue demotion | Slices replenish and re-queue to tail |
| **Stop Order Trigger** | `triggerStopOrders()` | $O(S \cdot M)$ where $S$ is triggered stops | Iterative cascading; zero-cost if no stops |
| **Cancel by ID** | `cancelOrder(order_id)` | $O(1)$ flat lookup + $O(1)$ intrusive unlink | Instant unlink from active or stop books |
| **Modify (Decrease Qty)**| `modifyOrder(id, qty)` | $O(1)$ in-place update | Deducts from reserve first, retains priority |
| **Modify (Price/Increase)**| `modifyOrder(id, p, q)` | $O(1)$ cancel + $O(\log P)$ re-insert | Price change resets FIFO time priority |
| **Best Bid / Best Ask** | `bestBid()`, `bestAsk()`| $O(1)$ top of tree dereference | Instant root iterator dereference |
| **Spread Calculation** | `spread()` | $O(1)$ | Best ask minus best bid |

---

## Benchmark Results & Latency Distribution

Benchmarked on **1,000,000 operations** per scenario with cache-aligned preallocated order book capacity ($1.2\times$ headroom = 1,200,000 slots) to guarantee zero data structure resizing during execution, measured with nanosecond-resolution timing (`std::chrono::steady_clock`):

### 1. Mixed High-Frequency Trading Workload (1,000,000 Events)
*(50% Limit Orders, 15% Reserve Orders, 10% Stop Orders, 15% Market Orders, 5% Cancels, 5% Modifies)*

| Action Type | Event Count | Mean Latency | Median (\(P_{50}\)) | \(P_{90}\) | \(P_{95}\) | \(P_{99}\) | \(P_{99.9}\) | Max Latency | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Market Orders** | 150,422 | **421.9 ns** | **355.0 ns** | 619.0 ns | 739.0 ns | 1.11 µs | 2.58 µs | 110.0 µs | **~2.37M ops/sec** |
| **Stop Orders** | 99,927 | **813.5 ns** | **694.0 ns** | 1.07 µs | 1.24 µs | 3.06 µs | 12.50 µs | 192.7 µs | **~1.23M ops/sec** |
| **Cancellations** | 50,080 | **760.0 ns** | **700.0 ns** | 1.02 µs | 1.20 µs | 3.10 µs | 14.20 µs | 154.3 µs | **~1.32M ops/sec** |
| **New Limit Orders**| 499,089 | **848.7 ns** | **734.0 ns** | 1.08 µs | 1.26 µs | 2.46 µs | 12.95 µs | 149.2 µs | **~1.18M ops/sec** |
| **Reserve Orders** | 150,409 | **882.7 ns** | **766.0 ns** | 1.12 µs | 1.30 µs | 2.49 µs | 13.22 µs | 159.8 µs | **~1.13M ops/sec** |
| **Modifications** | 50,073 | **1335.1 ns**| **1307.0 ns**| 1.86 µs | 2.20 µs | 4.62 µs | 16.89 µs | 86.4 µs | **~0.75M ops/sec** |
| **OVERALL WORKLOAD**| **1,000,000** | **806.0 ns** | **716.0 ns** | **1.14 µs** | **1.38 µs** | **2.51 µs** | **12.45 µs** | **192.7 µs** | **~1.14M ops/sec** |

### 2. Bulk Operation Benchmarks (1,000,000 Operations)

| Benchmark Scenario | Sample Count | Mean Latency | Median (\(P_{50}\)) | \(P_{99}\) | \(P_{99.9}\) | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Bulk $O(1)$ Cancellations** | 1,000,000 | **399.4 ns** | **354.0 ns** | **1.00 µs** | **2.81 µs** | **~2.29M cancels/sec** |
| **Bulk Limit Insertions** | 1,000,000 | **1237.6 ns**| **1092.0 ns**| **4.73 µs** | **17.05 µs** | **~0.78M orders/sec** |

---

## Building and Running

### Prerequisites
- CMake $\ge$ 3.25
- Modern C++23 compliant compiler (GCC 13+, Clang 16+)
- Python 3.10+ (for graph visualization)

### 1. Build the Project
```bash
# Configure and build executable
cmake -B build -S .
cmake --build build -j
```

### 2. Run Application Demo
```bash
# Runs the order book live demonstration (limit, market, reserve replenishment, stop orders)
./build/n7_order_book
```

### 3. Run Google Test Suites & Latency Benchmarks
```bash
# Run all 20 unit tests and benchmarks via CTest
ctest --test-dir build --output-on-failure

# Or run via Google Test runner:
# 1. Run only functional & data structure unit tests (runs in < 5ms)
./build/tests/order_book_tests --gtest_filter="OrderBookTest.*:ObjectPoolTest.*:FlatHashMapTest.*:OrderBuilderTest.*"

# 2. Run nanosecond latency distribution benchmarks (1,000,000 orders, preallocated capacity)
./build/tests/order_book_tests --gtest_filter="LatencyBenchmarkTest.*"
```
*Latency benchmarks automatically export metrics to `data/latency_metrics.csv`.*

### 4. Set Up Python Virtual Environment & Generate Graphs

You can set up the Python environment using standard `python3 -m venv` or `uv`:

#### Using standard `python3 -m venv`:
```bash
# Create virtual environment
python3 -m venv .venv

# Activate and install dependencies
source .venv/bin/activate
pip install -r requirements.txt

# Generate publication-quality PNG and SVG latency charts
python scripts/plot_latency.py
```

#### Using `uv`:
```bash
# Create virtual environment and install from requirements.txt
uv venv .venv
uv pip install --python .venv/bin/python -r requirements.txt

# Generate charts
.venv/bin/python scripts/plot_latency.py
```

Generated charts will be saved to the `charts/` directory:
- `charts/latency_percentiles.png` & `.svg`
- `charts/latency_tail_curves.png` & `.svg`
- `charts/throughput_comparison.png` & `.svg`

---

## Project Structure

```
n7-order-book/
├── CMakeLists.txt              # CMake build configuration (C++23 & Google Test FetchContent)
├── requirements.txt            # Python dependencies (matplotlib, pandas, numpy)
├── .clang-format               # Project formatting rules
├── .clang-tidy                 # Static analysis rules
├── .gitignore
├── README.md
├── data/
│   └── latency_metrics.csv     # Exported benchmark latency distribution metrics
├── charts/
│   ├── latency_percentiles.png # Percentile comparison bar chart (PNG)
│   ├── latency_percentiles.svg # Percentile comparison bar chart (SVG)
│   ├── latency_tail_curves.png # Log-scale tail latency curves (PNG)
│   ├── latency_tail_curves.svg # Log-scale tail latency curves (SVG)
│   ├── throughput_comparison.png# Throughput (ops/sec) comparison (PNG)
│   └── throughput_comparison.svg# Throughput (ops/sec) comparison (SVG)
├── scripts/
│   └── plot_latency.py         # Python visualization generator (matplotlib)
├── src/
│   ├── common/
│   │   ├── flat_hash_map.hpp   # High-speed open-addressing flat hash table
│   │   ├── object_pool.hpp     # Generic zero-allocation ObjectPool<T> with in-place union Slot
│   │   └── utils.hpp           # OrderID, Price (uint32_t), Quantity (uint64_t), Enums
│   ├── order-book/
│   │   ├── order.hpp           # 64-byte cache-line aligned OrderData & composed OrderNode
│   │   ├── order_builder.hpp   # Fluent zero-overhead OrderBuilder with validation
│   │   ├── price_level.hpp     # Intrusive doubly-linked FIFO PriceLevel
│   │   ├── trade.hpp           # Trade execution event struct
│   │   ├── book.hpp            # OrderBook with FlatHashMap, PMR pools, and dormant stop books
│   │   └── book.cpp            # Low-latency matching engine implementation
│   └── main.cpp                # Order book engine demonstration using OrderBuilder
└── tests/
    ├── CMakeLists.txt          # GTest target configuration & CTest discovery
    ├── latency_tracker.hpp     # Nanosecond latency metrics tracker & CSV exporter
    ├── test_order_book.cpp     # 17 GTest functional and edge-case unit test suites
    └── test_latency_benchmark.cpp # 1M order benchmarks (Bulk Limit, Bulk Cancel, Mixed Workload)
```

---

## Future Work

- **Cache-Aligned Price Index**: Replace binary search trees (`std::pmr::map`) with a B-Tree or Radix array to eliminate pointer-chasing and cache misses during level traversal.
- **Advanced Order Types & TIF**: Add Time-in-Force policies (IOC, FOK, GTC) and Trailing Stops.
- **Multi-Symbol Sharding & Sequencer**: Partition symbols across dedicated CPU cores with lock-free Write-Ahead Logging (WAL) for deterministic state replay.
- **Deployable API and Market Data**: Integrate matching engine with an order entry protocol and dispense live market data.
