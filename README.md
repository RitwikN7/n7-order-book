# n7-order-book

A deterministic, high-performance, low-latency **Limit Order Book (LOB)** and matching engine written in modern **C++23**.

Designed with financial exchange principles: zero dynamic memory allocations on the matching path, cache-conscious data layout, $O(1)$ cancellations via intrusive doubly-linked queues, and fixed-point integer arithmetic for 100% determinism.

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
    │ unordered_map<OrderID, Node*>│      │      map<Price, PriceLevel>      │
    └──────────────────────────────┘      │ (Intrusive Doubly-Linked Lists)  │
                                          └──────────────────────────────────┘
```

### 1. Zero-Allocation `ObjectPool<T>`
- **Mechanism**: Pre-allocates a single contiguous memory block of `Slot`s at startup. Uses an in-place free list (`Slot* next`) overlaid on `std::array<std::byte, sizeof(T)>` aligned storage.
- **Latency**: Sub-10ns $O(1)$ `acquire()` and `release()` without calling `malloc`, `new`, or `free`.
- **Compile-Time Elision**: Destructor calls are completely eliminated at compile time for trivially destructible types (`std::is_trivially_destructible_v<T>`).

### 2. $O(1)$ Order Lookup Index (`orders_map_`)
- `std::unordered_map<OrderID, OrderNode*>` provides constant-time direct pointer access to any resting order in the book by its 64-bit ID.

### 3. Price-Time (FIFO) Priority Queues (`PriceLevel`)
- **Bids**: Sorted descending using `std::map<Price, PriceLevel, std::greater<>>` (highest price at `bids_.begin()`).
- **Asks**: Sorted ascending using `std::map<Price, PriceLevel, std::less<>>` (lowest price at `asks_.begin()`).
- **Intrusive Doubly-Linked Lists**: Each `PriceLevel` maintains `head` and `tail` pointers to `OrderNode`s. Because `prev` and `next` pointers live directly inside `OrderNode` (intrusive), unlinking an order takes $O(1)$ time with **zero extra heap node allocations**.

### 4. Fixed-Point Arithmetic ($10^4$ Precision)
- All internal prices and quantities use explicit `<cstdint>` fixed-width integers (`int64_t` for prices, `uint32_t` for quantities).
- Scaled by $10^4$ ($10,000$ ticks per \$1.0000), eliminating binary floating-point rounding errors and non-deterministic behavior.

---

## Core Algorithms & Complexity

- **Zero Heap Allocations on Hot Path**: All core trading operations execute with **0 dynamic heap allocations** (`malloc`/`free`) via the pre-allocated `ObjectPool`.

| Operation | Method | Time Complexity |
| :--- | :--- | :--- |
| **Add Limit Order** | `addOrder(order)` | $O(\log P)$ price lookup + $O(1)$ append |
| **Match Market Order** | `addOrder(order)` | $O(M)$ where $M$ is matched fills |
| **Cancel by ID** | `cancelOrder(order_id)` | $O(1)$ avg lookup + $O(1)$ intrusive unlink |
| **Modify (Decrease Qty)**| `modifyOrder(id, qty)` | $O(1)$ in-place update (retains priority) |
| **Modify (Price/Increase)**| `modifyOrder(id, p, q)` | $O(1)$ cancel + $O(\log P)$ re-insert |
| **Best Bid / Best Ask** | `bestBid()`, `bestAsk()`| $O(1)$ top of tree dereference |
| **Spread Calculation** | `spread()` | $O(1)$ |

---

## Hardware Specifications & Benchmark Results

### Test Environment Hardware
- **CPU**: 11th Gen Intel(R) Core(TM) i7-1165G7 @ 2.80GHz (4 Cores / 8 Threads)
- **CPU Caches**:
  - **L1d / L1i**: 192 KiB / 128 KiB
  - **L2 / L3**: 5 MiB / 12 MiB
- **RAM**: 8.0 GB (7.7 GiB available)
- **OS**: Ubuntu 26.04 LTS (x86_64, Linux 6.18 Kernel)
- **Compiler**: GCC 15.2.0 (`-std=c++23 -O3`)

### Benchmark Measurements

```
===============================================================
      N7 LOW-LATENCY ORDER BOOK BENCHMARK & TEST SUITE         
===============================================================
```

| Benchmark Scenario | Sample Size | Total Elapsed Time | Avg Latency / Order | Throughput |
| :--- | :--- | :--- | :--- | :--- |
| **$O(1)$ Order Cancellations** | 1,000,000 cancels | **34,680 $\mu s$** (34.68 ms) | **34.68 ns** (0.0347 $\mu s$) | **~28.84 Million** cancels/sec |
| **Mixed High-Frequency Simulation**<br>*(60% Limit, 20% Cancel, 15% Market, 5% Modify)* | 1,000,000 events | **610,330 $\mu s$** (610.33 ms) | **610.33 ns** (0.6103 $\mu s$) | **~1.64 Million** ops/sec |
| **Bulk Limit Insertions**<br>*(1,000,000 orders across tree)* | 1,000,000 orders | **1,015,597 $\mu s$** (1,015.60 ms) | **1,015.60 ns** (1.0156 $\mu s$) | **~984,643** orders/sec |

---

## Building and Running

### Prerequisites
- CMake $\ge$ 3.25
- Modern C++23 compliant compiler (GCC 13+, Clang 16+)
- `clang-format` (optional, for code formatting)

### 1. Build the Project
```bash
# Configure build with CMake
cmake -B build -S .

# Build library and standalone executable
cmake --build build
```

### 2. Run the Benchmark & Demo
```bash
./build/n7_order_book
```

### 3. Code Formatting & Linting
```bash
# Format code using project's .clang-format
cmake --build build --target format

# Check formatting
cmake --build build --target format-check

# Run Clang-Tidy static analysis (if clang-tidy is installed)
cmake --build build --target tidy
```

---

## Project Structure

```
n7-order-book/
├── CMakeLists.txt              # CMake build configuration (C++23)
├── .clang-format               # Project formatting rules
├── .clang-tidy                 # Static analysis rules
├── .gitignore
├── README.md
└── src/
    ├── common/
    │   ├── object_pool.hpp     # Generic zero-allocation ObjectPool<T>
    │   └── utils.hpp           # OrderID, Price, Quantity, Enums
    ├── order-book/
    │   ├── order.hpp           # OrderData & intrusive OrderNode
    │   ├── price_level.hpp     # Intrusive doubly-linked FIFO PriceLevel
    │   ├── trade.hpp           # Trade execution event struct
    │   ├── book.hpp            # OrderBook header (namespace MatchingEngine)
    │   └── book.cpp            # Matching engine implementation
    └── main.cpp                # Benchmark suite & interactive demo
```
