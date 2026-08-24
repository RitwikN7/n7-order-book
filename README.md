# n7-order-book

A deterministic, high-performance, low-latency **Limit Order Book (LOB)** and matching engine written in modern **C++23**.

Designed with institutional financial exchange principles: zero dynamic memory allocations on the hot path, cache-conscious data layout, open-addressing flat hash tables, PMR memory pool allocators, $O(1)$ cancellations via intrusive doubly-linked queues, and fixed-point integer arithmetic for 100% determinism.

---

## Architecture & Data Structures

```
                             ┌──────────────────────────────────┐
                             │  ObjectPool<OrderNode> (Memory)  │
                             │   Pre-allocated contiguous RAM   │
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
    │     orders_map_ (Lookup)     │      │        bids_ & asks_ Trees       │
    │  FlatHashMap<OrderID, Node*> │      │  pmr::map<Price, PriceLevel>     │
    └──────────────────────────────┘      │   (PMR Unsynchronized Pool)      │
                                          └──────────────────────────────────┘
```

### 1. Zero-Allocation `ObjectPool<T>`
- **Mechanism**: Pre-allocates contiguous memory blocks of `Slot`s at startup. Uses an in-place free list (`Slot* next`) overlaid on `std::array<std::byte, sizeof(T)>` aligned storage.
- **Latency**: Sub-10ns $O(1)$ `acquire()` and `release()` without calling `malloc`, `new`, or `free`.
- **Compile-Time Elision**: Destructor calls are completely eliminated at compile time for trivially destructible types (`std::is_trivially_destructible_v<T>`).

### 2. $O(1)$ Open-Addressing Flat Hash Index (`FlatHashMap`)
- Custom cache-line friendly flat hash table with linear probing and power-of-two capacity bitmasking.
- Uses backward-shift erasure to eliminate tombstones and maintain optimal probe sequences.
- Eliminates node heap allocations and pointer chasing present in standard `std::unordered_map`.

### 3. Zero-Allocation Price Levels with PMR Pool Allocators (`PriceLevel`)
- **Bids**: Sorted descending using `std::pmr::map<Price, PriceLevel, std::greater<>>` (highest price at `bids_.begin()`).
- **Asks**: Sorted ascending using `std::pmr::map<Price, PriceLevel, std::less<>>` (lowest price at `asks_.begin()`).
- **PMR Memory Pool**: Allocates tree nodes from a local `std::pmr::unsynchronized_pool_resource`, eliminating global `malloc`/`free` lock contention and OS memory jitter.
- **Intrusive Doubly-Linked Lists**: Each `PriceLevel` maintains `head` and `tail` pointers to `OrderNode`s. Because `prev` and `next` pointers live directly inside `OrderNode` (intrusive), unlinking an order takes $O(1)$ time with **zero extra heap node allocations**.

### 4. Fixed-Point Arithmetic ($10^4$ Precision)
- All internal prices and quantities use explicit `<cstdint>` fixed-width integers (`int64_t` for prices, `uint64_t` for quantities).
- Scaled by $10^4$ ($10,000$ ticks per \$1.0000), eliminating binary floating-point rounding errors and non-deterministic behavior.

---

## Core Algorithms & Complexity

- **Zero Heap Allocations on Hot Path**: All core trading operations execute with **0 dynamic heap allocations** (`malloc`/`free`) via the pre-allocated `ObjectPool` and PMR pool resource.

| Operation | Method | Time Complexity |
| :--- | :--- | :--- |
| **Add Limit Order** | `addOrder(order)` | $O(\log P)$ price lookup + $O(1)$ queue append |
| **Match Market Order** | `addOrder(order)` | $O(M)$ where $M$ is matched fills |
| **Cancel by ID** | `cancelOrder(order_id)` | $O(1)$ flat lookup + $O(1)$ intrusive unlink |
| **Modify (Decrease Qty)**| `modifyOrder(id, qty)` | $O(1)$ in-place update (retains priority) |
| **Modify (Price/Increase)**| `modifyOrder(id, p, q)` | $O(1)$ cancel + $O(\log P)$ re-insert |
| **Best Bid / Best Ask** | `bestBid()`, `bestAsk()`| $O(1)$ top of tree dereference |
| **Spread Calculation** | `spread()` | $O(1)$ |

---

## Benchmark Results & Latency Distribution

Benchmarked on **1,000,000 operations** per scenario with nanosecond-resolution timing (`std::chrono::steady_clock`):

### 1. Mixed High-Frequency Trading Workload (1,000,000 Events)
*(60% Limit Orders, 20% Cancels, 15% Market Orders, 5% Modifies)*

| Action Type | Event Count | Mean Latency | Median (\(P_{50}\)) | \(P_{90}\) | \(P_{95}\) | \(P_{99}\) | \(P_{99.9}\) | Max Latency | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Market Orders** | 150,336 | **218.7 ns** | **133.0 ns** | 415.0 ns | 491.0 ns | 704.0 ns | 1.11 µs | 115.3 µs | **~4.57M ops/sec** |
| **Cancellations** | 200,195 | **424.5 ns** | **519.0 ns** | 679.0 ns | 740.0 ns | 998.0 ns | 4.14 µs | 124.5 µs | **~2.36M ops/sec** |
| **New Limit Orders**| 599,473 | **532.7 ns** | **473.0 ns** | 744.0 ns | 812.0 ns | 1.01 µs | 3.40 µs | 393.9 µs | **~1.88M ops/sec** |
| **Modifications** | 49,996 | **687.6 ns** | **822.0 ns** | 1.25 µs | 1.36 µs | 1.69 µs | 6.82 µs | 37.9 µs | **~1.45M ops/sec** |
| **OVERALL WORKLOAD**| 1,000,000 | **471.6 ns** | **458.0 ns** | **742.0 ns** | **848.0 ns** | **1.20 µs** | **2.74 µs** | **393.9 µs** | **~1.86M ops/sec** |

### 2. Bulk Operation Benchmarks (1,000,000 Operations)

| Benchmark Scenario | Sample Count | Mean Latency | Median (\(P_{50}\)) | \(P_{99}\) | \(P_{99.9}\) | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Bulk $O(1)$ Cancellations** | 1,000,000 | **37.7 ns** | **36.0 ns** | **109.0 ns** | **323.0 ns** | **~14.00M cancels/sec** |
| **Bulk Limit Insertions** | 1,000,000 | **847.3 ns** | **789.0 ns** | **1.58 µs** | **10.78 µs** | **~1.12M orders/sec** |

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
cmake --build build
```

### 2. Run Test Suites & Benchmarks
```bash
# Runs functional tests (7 suites, 86 assertions) and 1M benchmarks
./build/n7_order_book
```
*Outputs latency metrics automatically to `data/latency_metrics.csv`.*

### 3. Set Up Python Virtual Environment & Generate Graphs

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
├── CMakeLists.txt              # CMake build configuration (C++23)
├── requirements.txt            # Python dependencies (matplotlib, seaborn, pandas, numpy)
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
│   └── plot_latency.py         # Python visualization generator (matplotlib/seaborn)
└── src/
    ├── common/
    │   ├── flat_hash_map.hpp   # High-speed open-addressing flat hash table
    │   ├── object_pool.hpp     # Generic zero-allocation ObjectPool<T>
    │   └── utils.hpp           # OrderID, Price, Quantity, Enums
    ├── order-book/
    │   ├── order.hpp           # OrderData & intrusive OrderNode
    │   ├── price_level.hpp     # Intrusive doubly-linked FIFO PriceLevel
    │   ├── trade.hpp           # Trade execution event struct
    │   ├── book.hpp            # OrderBook with FlatHashMap and PMR pool
    │   └── book.cpp            # Zero-allocation matching engine implementation
    └── main.cpp                # Test suite, latency distribution benchmarks & CSV exporter
```

---

## Future Work

- **Cache-Aligned Price Index**: Replace binary search trees (`std::pmr::map`) with a B-Tree or Radix array to eliminate pointer-chasing and cache misses during level traversal.
- **Advanced Order Types & TIF**: Add Time-in-Force policies (IOC, FOK, GTC), Stop-Loss / Take-Profit triggers, and Iceberg / Hidden orders.
- **Multi-Symbol Sharding & Sequencer**: Partition symbols across dedicated CPU cores with lock-free Write-Ahead Logging (WAL) for deterministic state replay.
- **Deterministic Sequencer & Replay Log**: Implement a lock-free ring-buffer Write-Ahead Log (WAL) for microsecond crash recovery and state replication.
