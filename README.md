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
| **Market Orders** | 150,336 | **259.3 ns** | **143.0 ns** | 450.0 ns | 566.0 ns | 931.0 ns | 2.44 µs | 876.5 µs | **~3.86M ops/sec** |
| **Cancellations** | 200,195 | **511.4 ns** | **542.0 ns** | 865.0 ns | 1.05 µs | 1.96 µs | 4.99 µs | 235.1 µs | **~1.96M ops/sec** |
| **New Limit Orders**| 599,473 | **637.8 ns** | **526.0 ns** | 878.0 ns | 1.05 µs | 1.67 µs | 5.32 µs | 454.1 µs | **~1.57M ops/sec** |
| **Modifications** | 49,996 | **812.9 ns** | **864.0 ns** | 1.45 µs | 1.79 µs | 2.89 µs | 9.93 µs | 189.6 µs | **~1.23M ops/sec** |
| **OVERALL WORKLOAD**| 1,000,000 | **564.3 ns** | **500.0 ns** | **892.0 ns** | **1.09 µs** | **1.80 µs** | **5.09 µs** | **876.5 µs** | **~1.57M ops/sec** |

### 2. Bulk Operation Benchmarks (1,000,000 Operations)

| Benchmark Scenario | Sample Count | Mean Latency | Median (\(P_{50}\)) | \(P_{99}\) | \(P_{99.9}\) | Throughput |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Bulk $O(1)$ Cancellations** | 1,000,000 | **48.7 ns** | **41.0 ns** | **155.0 ns** | **612.0 ns** | **~11.43M cancels/sec** |
| **Bulk Limit Insertions** | 1,000,000 | **1,116.0 ns** | **938.0 ns** | **3.90 µs** | **16.09 µs** | **~854,648 orders/sec** |

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

#### Using `uv` (Ultra-fast):
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

