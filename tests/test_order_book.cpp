#include "common/flat_hash_map.hpp"
#include "common/object_pool.hpp"
#include "common/utils.hpp"
#include "order-book/book.hpp"
#include "order-book/order.hpp"

#include <gtest/gtest.h>

using namespace MatchingEngine;

// ============================================================================
// 1. COMPREHENSIVE FUNCTIONAL TEST SUITES (ORDER BOOK)
// ============================================================================

TEST(OrderBookTest, BasicLimitOrdersAndBookDepth)
{
    Book book(150);

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.orderCount(), 0);
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_FALSE(book.spread().has_value());

    // Insert Bids: $100.00 (id 101, qty 50), $99.50 (id 102, qty 100)
    auto t1 = book.addOrder(OrderData{
        .order_id = 101,
        .price = 1'000'000,
        .quantity = 50,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    EXPECT_TRUE(t1.empty());
    EXPECT_EQ(book.orderCount(), 1);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), 1'000'000);

    auto t2 = book.addOrder(OrderData{
        .order_id = 102,
        .price = 995'000,
        .quantity = 100,
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    EXPECT_TRUE(t2.empty());
    EXPECT_EQ(book.orderCount(), 2);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), 1'000'000);

    // Insert Asks: $100.50 (id 201, qty 30), $101.00 (id 202, qty 70)
    auto t3 = book.addOrder(OrderData{
        .order_id = 201,
        .price = 1'005'000,
        .quantity = 30,
        .side = OrderBookUtils::OrderSide::SELL,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    EXPECT_TRUE(t3.empty());
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(book.bestAsk().value(), 1'005'000);

    auto t4 = book.addOrder(OrderData{
        .order_id = 202,
        .price = 1'010'000,
        .quantity = 70,
        .side = OrderBookUtils::OrderSide::SELL,
        .type = OrderBookUtils::OrderType::LIMIT,
    });
    EXPECT_TRUE(t4.empty());
    EXPECT_EQ(book.orderCount(), 4);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(book.bestAsk().value(), 1'005'000);

    // Check spread: $100.50 - $100.00 = $0.50 (5000 ticks)
    ASSERT_TRUE(book.spread().has_value());
    EXPECT_EQ(book.spread().value(), 5'000);

    // Verify order lookup
    const auto* node101 = book.findOrder(101);
    ASSERT_NE(node101, nullptr);
    EXPECT_EQ(node101->price, 1'000'000);
    EXPECT_EQ(node101->quantity, 50);
}

TEST(OrderBookTest, FifoPriceTimePriority)
{
    Book book(150);

    // Insert 3 orders at same price $100.00 (1'000'000 ticks):
    // Order 1: qty 50
    // Order 2: qty 30
    // Order 3: qty 20
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'000'000,
                            .quantity = 30,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 3,
                            .price = 1'000'000,
                            .quantity = 20,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    EXPECT_EQ(book.orderCount(), 3);

    // Send aggressive market sell order of qty 60.
    // Should fully fill Order 1 (50 units) and partially fill Order 2 (10 units).
    // Order 3 should remain untouched.
    auto trades = book.addOrder(OrderData{.order_id = 99,
                                          .price = 0,
                                          .quantity = 60,
                                          .side = OrderBookUtils::OrderSide::SELL,
                                          .type = OrderBookUtils::OrderType::MARKET});

    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].maker_order_id, 1);
    EXPECT_EQ(trades[0].quantity, 50);
    EXPECT_EQ(trades[1].maker_order_id, 2);
    EXPECT_EQ(trades[1].quantity, 10);

    // Order 1 should be gone from the book
    EXPECT_EQ(book.findOrder(1), nullptr);

    // Order 2 should have 20 remaining
    const auto* node2 = book.findOrder(2);
    ASSERT_NE(node2, nullptr);
    EXPECT_EQ(node2->quantity, 20);

    // Order 3 should have 20 remaining
    const auto* node3 = book.findOrder(3);
    ASSERT_NE(node3, nullptr);
    EXPECT_EQ(node3->quantity, 20);

    EXPECT_EQ(book.orderCount(), 2);
}

TEST(OrderBookTest, MarketOrdersAndMultiLevelSweeps)
{
    Book book(150);

    // Set up ask ladder:
    // Ask 1: $101.00 (id 1, qty 20)
    // Ask 2: $102.00 (id 2, qty 30)
    // Ask 3: $103.00 (id 3, qty 50)
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'010'000,
                            .quantity = 20,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'020'000,
                            .quantity = 30,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 3,
                            .price = 1'030'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Market Buy of 65:
    // - Fills 20 @ $101 (Ask 1 removed)
    // - Fills 30 @ $102 (Ask 2 removed)
    // - Fills 15 @ $103 (Ask 3 has 35 left)
    auto trades = book.addOrder(OrderData{.order_id = 10,
                                          .price = 0,
                                          .quantity = 65,
                                          .side = OrderBookUtils::OrderSide::BUY,
                                          .type = OrderBookUtils::OrderType::MARKET});

    ASSERT_EQ(trades.size(), 3);
    EXPECT_EQ(trades[0].price, 1'010'000);
    EXPECT_EQ(trades[0].quantity, 20);
    EXPECT_EQ(trades[1].price, 1'020'000);
    EXPECT_EQ(trades[1].quantity, 30);
    EXPECT_EQ(trades[2].price, 1'030'000);
    EXPECT_EQ(trades[2].quantity, 15);

    // Book state verification: only Ask 3 remains with qty 35
    EXPECT_EQ(book.orderCount(), 1);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(book.bestAsk().value(), 1'030'000);
    const auto* node3 = book.findOrder(3);
    ASSERT_NE(node3, nullptr);
    EXPECT_EQ(node3->quantity, 35);

    // Sweep remaining 35 with market sell-out
    auto trades2 = book.addOrder(OrderData{.order_id = 11,
                                           .price = 0,
                                           .quantity = 100,
                                           .side = OrderBookUtils::OrderSide::BUY,
                                           .type = OrderBookUtils::OrderType::MARKET});
    ASSERT_EQ(trades2.size(), 1);
    EXPECT_EQ(trades2[0].quantity, 35);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, AggressiveLimitCrossingSpread)
{
    Book book(150);

    // Resting Ask: $100.00 (id 1, qty 40)
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 40,
                            .side = OrderBookUtils::OrderSide::SELL,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Incoming Limit Buy: $102.00 (id 2, qty 60)
    // Crosses ask at $100.00:
    // - Should match 40 @ $100.00 (maker price)
    // - Remaining 20 units should rest on bid side at $102.00
    auto trades = book.addOrder(OrderData{.order_id = 2,
                                          .price = 1'020'000,
                                          .quantity = 60,
                                          .side = OrderBookUtils::OrderSide::BUY,
                                          .type = OrderBookUtils::OrderType::LIMIT});

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].price, 1'000'000);
    EXPECT_EQ(trades[0].quantity, 40);

    EXPECT_EQ(book.orderCount(), 1);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), 1'020'000);
    EXPECT_FALSE(book.bestAsk().has_value());

    const auto* node2 = book.findOrder(2);
    ASSERT_NE(node2, nullptr);
    EXPECT_EQ(node2->quantity, 20);
}

TEST(OrderBookTest, OrderModifications)
{
    Book book(150);

    // Insert 2 Buy orders at $100.00:
    // Order 1: qty 50
    // Order 2: qty 50
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'000'000,
                            .quantity = 50,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Modify Order 1 quantity DOWN to 30 (same price)
    // Should preserve FIFO priority
    bool mod1_ok = book.modifyOrder(1, 30);
    EXPECT_TRUE(mod1_ok);
    const auto* n1 = book.findOrder(1);
    ASSERT_NE(n1, nullptr);
    EXPECT_EQ(n1->quantity, 30);

    // Verify priority: aggressive sell of 30 fills Order 1 completely first
    auto trades = book.addOrder(OrderData{.order_id = 99,
                                          .price = 0,
                                          .quantity = 30,
                                          .side = OrderBookUtils::OrderSide::SELL,
                                          .type = OrderBookUtils::OrderType::MARKET});
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].maker_order_id, 1);
    EXPECT_EQ(trades[0].quantity, 30);
    EXPECT_EQ(book.findOrder(1), nullptr);

    // Modify Order 2 price from $100.00 to $105.00
    bool mod2_ok = book.modifyOrder(2, 1'050'000, 50);
    EXPECT_TRUE(mod2_ok);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), 1'050'000);

    // Modify Order 2 to qty 0 -> should cancel
    bool mod_zero = book.modifyOrder(2, 0);
    EXPECT_TRUE(mod_zero);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, OrderCancellations)
{
    Book book(150);

    // Add 3 orders at $100.00 (ids: 1, 2, 3)
    book.addOrder(OrderData{.order_id = 1,
                            .price = 1'000'000,
                            .quantity = 10,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 2,
                            .price = 1'000'000,
                            .quantity = 20,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});
    book.addOrder(OrderData{.order_id = 3,
                            .price = 1'000'000,
                            .quantity = 30,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    // Add 1 order at $95.00 (id: 4)
    book.addOrder(OrderData{.order_id = 4,
                            .price = 950'000,
                            .quantity = 40,
                            .side = OrderBookUtils::OrderSide::BUY,
                            .type = OrderBookUtils::OrderType::LIMIT});

    EXPECT_EQ(book.orderCount(), 4);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), 1'000'000);

    // Cancel middle order (id: 2)
    EXPECT_TRUE(book.cancelOrder(2));
    EXPECT_EQ(book.orderCount(), 3);
    EXPECT_EQ(book.findOrder(2), nullptr);

    // Cancel head order (id: 1)
    EXPECT_TRUE(book.cancelOrder(1));
    EXPECT_EQ(book.orderCount(), 2);
    EXPECT_EQ(book.findOrder(1), nullptr);

    // Cancel last order at $100.00 (id: 3) -> best bid should shift to $95.00
    EXPECT_TRUE(book.cancelOrder(3));
    EXPECT_EQ(book.orderCount(), 1);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), 950'000);

    // Cancel id: 4 -> book empty
    EXPECT_TRUE(book.cancelOrder(4));
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, EdgeCasesAndRobustness)
{
    Book book(150);

    // Cancel non-existent order
    EXPECT_FALSE(book.cancelOrder(9999));

    // Modify non-existent order
    EXPECT_FALSE(book.modifyOrder(9999, 100));
    EXPECT_FALSE(book.modifyOrder(9999, 1'000'000, 100));

    // Add zero-quantity order
    auto zero_trades = book.addOrder(OrderData{.order_id = 1,
                                               .price = 1'000'000,
                                               .quantity = 0,
                                               .side = OrderBookUtils::OrderSide::BUY,
                                               .type = OrderBookUtils::OrderType::LIMIT});
    EXPECT_TRUE(zero_trades.empty());
    EXPECT_TRUE(book.empty());

    // Market order on completely empty book
    auto mkt_empty = book.addOrder(OrderData{.order_id = 2,
                                             .price = 0,
                                             .quantity = 100,
                                             .side = OrderBookUtils::OrderSide::BUY,
                                             .type = OrderBookUtils::OrderType::MARKET});
    EXPECT_TRUE(mkt_empty.empty());
    EXPECT_TRUE(book.empty());

    // Find non-existent order
    EXPECT_EQ(book.findOrder(9999), nullptr);
}

// ============================================================================
// 2. DATA STRUCTURE UNIT TESTS
// ============================================================================

TEST(ObjectPoolTest, AcquireAndRelease)
{
    OrderBookUtils::ObjectPool<OrderNode> pool(10);

    EXPECT_EQ(pool.capacity(), 10);
    EXPECT_EQ(pool.size(), 0);
    EXPECT_EQ(pool.available(), 10);
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.full());

    std::vector<OrderNode*> allocated;
    for (std::size_t i = 0; i < 10; ++i)
    {
        OrderNode* node = pool.acquire();
        ASSERT_NE(node, nullptr);
        EXPECT_TRUE(pool.contains(node));
        allocated.push_back(node);
    }

    EXPECT_EQ(pool.size(), 10);
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.acquire(), nullptr); // Exhausted

    // Release all
    for (auto* node : allocated)
    {
        pool.release(node);
    }

    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(pool.size(), 0);
    EXPECT_EQ(pool.available(), 10);

    // Can reacquire
    OrderNode* node = pool.acquire();
    EXPECT_NE(node, nullptr);
    pool.release(node);
}

TEST(FlatHashMapTest, BasicOperationsAndBackwardShift)
{
    OrderBookUtils::FlatHashMap<std::uint64_t, std::string> map(16);

    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0);

    EXPECT_TRUE(map.insert(1, "one"));
    EXPECT_TRUE(map.insert(2, "two"));
    EXPECT_TRUE(map.insert(3, "three"));
    EXPECT_FALSE(map.insert(1, "ONE")); // update existing

    EXPECT_EQ(map.size(), 3);

    auto* v1 = map.find(1);
    ASSERT_NE(v1, nullptr);
    EXPECT_EQ(*v1, "ONE");

    EXPECT_EQ(map.find(999), nullptr);

    // Backward-shift deletion test
    EXPECT_TRUE(map.erase(2));
    EXPECT_EQ(map.size(), 2);
    EXPECT_EQ(map.find(2), nullptr);
    EXPECT_NE(map.find(1), nullptr);
    EXPECT_NE(map.find(3), nullptr);

    map.clear();
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0);
}
