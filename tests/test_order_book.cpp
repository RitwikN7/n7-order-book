#include "common/flat_hash_map.hpp"
#include "common/object_pool.hpp"
#include "common/utils.hpp"
#include "order-book/book.hpp"
#include "order-book/order.hpp"
#include "order-book/order_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace MatchingEngine;
using OrderBookUtils::Price;
using OrderBookUtils::Quantity;

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
    auto t1 =
        book.addOrder(OrderBuilder{}.id(101).buy().limit(Price(1'000'000), Quantity(50)).build());
    EXPECT_TRUE(t1.empty());
    EXPECT_EQ(book.orderCount(), 1);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), Price(1'000'000));

    auto t2 =
        book.addOrder(OrderBuilder{}.id(102).buy().limit(Price(995'000), Quantity(100)).build());
    EXPECT_TRUE(t2.empty());
    EXPECT_EQ(book.orderCount(), 2);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), Price(1'000'000));

    // Insert Asks: $100.50 (id 201, qty 30), $101.00 (id 202, qty 70)
    auto t3 =
        book.addOrder(OrderBuilder{}.id(201).sell().limit(Price(1'005'000), Quantity(30)).build());
    EXPECT_TRUE(t3.empty());
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(book.bestAsk().value(), Price(1'005'000));

    auto t4 =
        book.addOrder(OrderBuilder{}.id(202).sell().limit(Price(1'010'000), Quantity(70)).build());
    EXPECT_TRUE(t4.empty());
    EXPECT_EQ(book.orderCount(), 4);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(book.bestAsk().value(), Price(1'005'000));

    // Check spread: $100.50 - $100.00 = $0.50 (5000 ticks)
    ASSERT_TRUE(book.spread().has_value());
    EXPECT_EQ(book.spread().value(), Price(5'000));

    // Verify order lookup
    const auto* node101 = book.findOrder(101);
    ASSERT_NE(node101, nullptr);
    EXPECT_EQ(node101->data.price, Price(1'000'000));
    EXPECT_EQ(node101->data.quantity, Quantity(50));
}

TEST(OrderBookTest, FifoPriceTimePriority)
{
    Book book(150);

    // Insert 3 orders at same price $100.00 (1'000'000 ticks):
    // Order 1: qty 50
    // Order 2: qty 30
    // Order 3: qty 20
    book.addOrder(OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(50)).build());
    book.addOrder(OrderBuilder{}.id(2).buy().limit(Price(1'000'000), Quantity(30)).build());
    book.addOrder(OrderBuilder{}.id(3).buy().limit(Price(1'000'000), Quantity(20)).build());

    EXPECT_EQ(book.orderCount(), 3);

    // Send aggressive market sell order of qty 60.
    // Should fully fill Order 1 (50 units) and partially fill Order 2 (10 units).
    // Order 3 should remain untouched.
    auto trades = book.addOrder(OrderBuilder{}.id(99).sell().market(Quantity(60)).build());

    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].maker_order_id, 1);
    EXPECT_EQ(trades[0].quantity, Quantity(50));
    EXPECT_EQ(trades[1].maker_order_id, 2);
    EXPECT_EQ(trades[1].quantity, Quantity(10));

    // Order 1 should be gone from the book
    EXPECT_EQ(book.findOrder(1), nullptr);

    // Order 2 should have 20 remaining
    const auto* node2 = book.findOrder(2);
    ASSERT_NE(node2, nullptr);
    EXPECT_EQ(node2->data.quantity, Quantity(20));

    // Order 3 should have 20 remaining
    const auto* node3 = book.findOrder(3);
    ASSERT_NE(node3, nullptr);
    EXPECT_EQ(node3->data.quantity, Quantity(20));

    EXPECT_EQ(book.orderCount(), 2);
}

TEST(OrderBookTest, MarketOrdersAndMultiLevelSweeps)
{
    Book book(150);

    // Set up ask ladder:
    // Ask 1: $101.00 (id 1, qty 20)
    // Ask 2: $102.00 (id 2, qty 30)
    // Ask 3: $103.00 (id 3, qty 50)
    book.addOrder(OrderBuilder{}.id(1).sell().limit(Price(1'010'000), Quantity(20)).build());
    book.addOrder(OrderBuilder{}.id(2).sell().limit(Price(1'020'000), Quantity(30)).build());
    book.addOrder(OrderBuilder{}.id(3).sell().limit(Price(1'030'000), Quantity(50)).build());

    // Market Buy of 65:
    // - Fills 20 @ $101 (Ask 1 removed)
    // - Fills 30 @ $102 (Ask 2 removed)
    // - Fills 15 @ $103 (Ask 3 has 35 left)
    auto trades = book.addOrder(OrderBuilder{}.id(10).buy().market(Quantity(65)).build());

    ASSERT_EQ(trades.size(), 3);
    EXPECT_EQ(trades[0].price, Price(1'010'000));
    EXPECT_EQ(trades[0].quantity, Quantity(20));
    EXPECT_EQ(trades[1].price, Price(1'020'000));
    EXPECT_EQ(trades[1].quantity, Quantity(30));
    EXPECT_EQ(trades[2].price, Price(1'030'000));
    EXPECT_EQ(trades[2].quantity, Quantity(15));

    // Book state verification: only Ask 3 remains with qty 35
    EXPECT_EQ(book.orderCount(), 1);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(book.bestAsk().value(), Price(1'030'000));
    const auto* node3 = book.findOrder(3);
    ASSERT_NE(node3, nullptr);
    EXPECT_EQ(node3->data.quantity, Quantity(35));

    // Sweep remaining 35 with market buy
    auto trades2 = book.addOrder(OrderBuilder{}.id(11).buy().market(Quantity(100)).build());
    ASSERT_EQ(trades2.size(), 1);
    EXPECT_EQ(trades2[0].quantity, Quantity(35));
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, AggressiveLimitCrossingSpread)
{
    Book book(150);

    // Resting Ask: $100.00 (id 1, qty 40)
    book.addOrder(OrderBuilder{}.id(1).sell().limit(Price(1'000'000), Quantity(40)).build());

    // Incoming Limit Buy: $102.00 (id 2, qty 60)
    // Crosses ask at $100.00:
    // - Should match 40 @ $100.00 (maker price)
    // - Remaining 20 units should rest on bid side at $102.00
    auto trades =
        book.addOrder(OrderBuilder{}.id(2).buy().limit(Price(1'020'000), Quantity(60)).build());

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].price, Price(1'000'000));
    EXPECT_EQ(trades[0].quantity, Quantity(40));

    EXPECT_EQ(book.orderCount(), 1);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), Price(1'020'000));
    EXPECT_FALSE(book.bestAsk().has_value());

    const auto* node2 = book.findOrder(2);
    ASSERT_NE(node2, nullptr);
    EXPECT_EQ(node2->data.quantity, Quantity(20));
}

TEST(OrderBookTest, OrderModifications)
{
    Book book(150);

    // Insert 2 Buy orders at $100.00:
    // Order 1: qty 50
    // Order 2: qty 50
    book.addOrder(OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(50)).build());
    book.addOrder(OrderBuilder{}.id(2).buy().limit(Price(1'000'000), Quantity(50)).build());

    // Modify Order 1 quantity DOWN to 30 (same price)
    // Should preserve FIFO priority
    bool mod1_ok = book.modifyOrder(1, Quantity(30));
    EXPECT_TRUE(mod1_ok);
    const auto* n1 = book.findOrder(1);
    ASSERT_NE(n1, nullptr);
    EXPECT_EQ(n1->data.quantity, Quantity(30));

    // Verify priority: aggressive sell of 30 fills Order 1 completely first
    auto trades = book.addOrder(OrderBuilder{}.id(99).sell().market(Quantity(30)).build());
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].maker_order_id, 1);
    EXPECT_EQ(trades[0].quantity, Quantity(30));
    EXPECT_EQ(book.findOrder(1), nullptr);

    // Modify Order 2 price from $100.00 to $105.00
    bool mod2_ok = book.modifyOrder(2, Price(1'050'000), Quantity(50));
    EXPECT_TRUE(mod2_ok);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), Price(1'050'000));

    // Modify Order 2 to qty 0 -> should cancel
    bool mod_zero = book.modifyOrder(2, Quantity(0));
    EXPECT_TRUE(mod_zero);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, OrderCancellations)
{
    Book book(150);

    // Add 3 orders at $100.00 (ids: 1, 2, 3)
    book.addOrder(OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(10)).build());
    book.addOrder(OrderBuilder{}.id(2).buy().limit(Price(1'000'000), Quantity(20)).build());
    book.addOrder(OrderBuilder{}.id(3).buy().limit(Price(1'000'000), Quantity(30)).build());

    // Add 1 order at $95.00 (id: 4)
    book.addOrder(OrderBuilder{}.id(4).buy().limit(Price(950'000), Quantity(40)).build());

    EXPECT_EQ(book.orderCount(), 4);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), Price(1'000'000));

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
    EXPECT_EQ(book.bestBid().value(), Price(950'000));

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
    EXPECT_FALSE(book.modifyOrder(9999, Quantity(100)));
    EXPECT_FALSE(book.modifyOrder(9999, Price(1'000'000), Quantity(100)));

    // Verify builder catches zero quantity
    auto invalid_res =
        OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(0)).buildChecked();
    EXPECT_FALSE(invalid_res.has_value());
    EXPECT_EQ(invalid_res.error(), OrderValidationError::ZERO_QUANTITY);

    // Add zero-quantity order directly to test book edge case
    OrderData zero_order{
        .order_id = 1,
        .price = Price(1'000'000),
        .quantity = Quantity(0),
        .side = OrderBookUtils::OrderSide::BUY,
        .type = OrderBookUtils::OrderType::LIMIT,
    };
    auto zero_trades = book.addOrder(zero_order);
    EXPECT_TRUE(zero_trades.empty());
    EXPECT_TRUE(book.empty());

    // Market order on completely empty book using OrderBuilder
    auto mkt_empty = book.addOrder(OrderBuilder{}.id(2).buy().market(Quantity(100)).build());
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

// ============================================================================
// 3. ORDER BUILDER, RESERVE ORDERS, AND STOP ORDERS UNIT TESTS
// ============================================================================

TEST(OrderBuilderTest, ValidationAndPresets)
{
    // Valid limit order
    auto valid_limit =
        OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(50)).buildChecked();
    ASSERT_TRUE(valid_limit.has_value());
    EXPECT_EQ(valid_limit->order_id, 1);
    EXPECT_EQ(valid_limit->price, Price(1'000'000));
    EXPECT_EQ(valid_limit->quantity, Quantity(50));
    EXPECT_EQ(valid_limit->side, OrderBookUtils::OrderSide::BUY);
    EXPECT_EQ(valid_limit->type, OrderBookUtils::OrderType::LIMIT);

    // Invalid ID
    auto err_id = OrderBuilder{}.id(0).buy().limit(Price(1'000'000), Quantity(50)).buildChecked();
    ASSERT_FALSE(err_id.has_value());
    EXPECT_EQ(err_id.error(), OrderValidationError::INVALID_ORDER_ID);

    // Zero quantity
    auto err_qty = OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(0)).buildChecked();
    ASSERT_FALSE(err_qty.has_value());
    EXPECT_EQ(err_qty.error(), OrderValidationError::ZERO_QUANTITY);

    // Invalid limit price
    auto err_price = OrderBuilder{}.id(1).buy().limit(Price(0), Quantity(50)).buildChecked();
    ASSERT_FALSE(err_price.has_value());
    EXPECT_EQ(err_price.error(), OrderValidationError::INVALID_LIMIT_PRICE);

    // Invalid stop price
    auto err_stop = OrderBuilder{}.id(1).sell().stopMarket(Price(0), Quantity(50)).buildChecked();
    ASSERT_FALSE(err_stop.has_value());
    EXPECT_EQ(err_stop.error(), OrderValidationError::INVALID_STOP_PRICE);

    // Invalid reserve display quantity > total quantity
    auto err_disp = OrderBuilder{}
                        .id(1)
                        .sell()
                        .limit(Price(1'000'000))
                        .reserve(Quantity(50), Quantity(100))
                        .buildChecked();
    ASSERT_FALSE(err_disp.has_value());
    EXPECT_EQ(err_disp.error(), OrderValidationError::INVALID_DISPLAY_QUANTITY);

    // Valid reserve order
    auto valid_reserve = OrderBuilder{}
                             .id(2)
                             .sell()
                             .limit(Price(1'000'000))
                             .reserve(Quantity(100), Quantity(25))
                             .buildChecked();
    ASSERT_TRUE(valid_reserve.has_value());
    EXPECT_TRUE(valid_reserve->isReserve());
    EXPECT_EQ(valid_reserve->quantity, Quantity(100));
    EXPECT_EQ(valid_reserve->display_quantity, Quantity(25));
}

TEST(OrderBookTest, ReserveOrdersExecutionAndPriority)
{
    Book book(150);

    // 1. Insert Reserve Order #1: Sell 100 @ $100.00 with display quantity 25
    book.addOrder(OrderBuilder{}
                      .id(1)
                      .sell()
                      .limit(Price(1'000'000))
                      .reserve(Quantity(100), Quantity(25))
                      .build());

    // 2. Insert Regular Limit Order #2: Sell 20 @ $100.00
    book.addOrder(OrderBuilder{}.id(2).sell().limit(Price(1'000'000), Quantity(20)).build());

    // Public book depth should show visible slice of Order #1 (25) + Order #2 (20) = 45 units
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(book.bestAsk().value(), Price(1'000'000));
    EXPECT_EQ(book.orderCount(), 2);

    // 3. Incoming Market Buy of 35 units:
    // - Matches full visible slice (25) of Order #1.
    // - Order #1 replenishes 25 from reserve and is re-queued to the TAIL of the price level!
    // - The remaining 10 units of the taker order should match against Order #2 (which now has
    // priority over Order #1's new slice)!
    auto trades = book.addOrder(OrderBuilder{}.id(99).buy().market(Quantity(35)).build());

    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].maker_order_id, 1);
    EXPECT_EQ(trades[0].quantity, Quantity(25));
    EXPECT_EQ(trades[1].maker_order_id, 2);
    EXPECT_EQ(trades[1].quantity, Quantity(10));

    // Order #2 should still have 10 units remaining at the head of the queue
    const auto* node2 = book.findOrder(2);
    ASSERT_NE(node2, nullptr);
    EXPECT_EQ(node2->data.quantity, Quantity(10));

    // Order #1 has 25 visible, and (100 - 25 - 25) = 50 in reserve
    const auto* node1 = book.findOrder(1);
    ASSERT_NE(node1, nullptr);
    EXPECT_EQ(node1->data.quantity, Quantity(25));
    EXPECT_EQ(node1->reserve_quantity, Quantity(50));

    // 4. Sweep remaining book: Buy 85 units (10 from Order #2, 75 from Order #1)
    auto trades2 = book.addOrder(OrderBuilder{}.id(100).buy().market(Quantity(85)).build());
    EXPECT_EQ(book.findOrder(1), nullptr);
    EXPECT_EQ(book.findOrder(2), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, StopOrdersTriggerAndCascade)
{
    Book book(150);

    // Resting Bid: Buy 100 @ $100.00
    book.addOrder(OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(100)).build());

    // Dormant Sell Stop Loss #2: Sell Market 20 when price hits <= $99.00
    book.addOrder(OrderBuilder{}.id(2).sell().stopMarket(Price(990'000), Quantity(20)).build());

    // Dormant Sell Stop Loss #3: Sell Market 10 when price hits <= $98.00
    book.addOrder(OrderBuilder{}.id(3).sell().stopMarket(Price(980'000), Quantity(10)).build());

    EXPECT_EQ(book.orderCount(), 3);
    EXPECT_EQ(book.activeOrderCount(), 1);
    EXPECT_EQ(book.stopOrderCount(), 2);

    // Resting Bid: Buy 50 @ $98.50
    book.addOrder(OrderBuilder{}.id(4).buy().limit(Price(985'000), Quantity(50)).build());
    // Resting Bid: Buy 50 @ $97.00
    book.addOrder(OrderBuilder{}.id(5).buy().limit(Price(970'000), Quantity(50)).build());

    // Trade at $100.00 does not trigger stop orders at $99.00 or $98.00
    auto t1 =
        book.addOrder(OrderBuilder{}.id(6).sell().limit(Price(1'000'000), Quantity(10)).build());
    EXPECT_EQ(t1.size(), 1);
    EXPECT_EQ(book.stopOrderCount(), 2);
    EXPECT_EQ(book.lastTradedPrice(), Price(1'000'000));

    // Now send aggressive sell that sweeps down through $100.00 and hits $98.50:
    // Sells 90 @ $100.00 (exhausts Order #1) and 10 @ $98.50.
    // Last traded price becomes $98.50 (< $99.00).
    // This TRIGGERS Stop Order #2 (Sell Market 20)!
    // Stop Order #2 executes against remaining bids at $98.50 (fills 20 @ $98.50).
    auto t2 = book.addOrder(OrderBuilder{}.id(7).sell().market(Quantity(100)).build());

    // Trades include taker #7 trades + triggered stop #2 trades:
    EXPECT_GT(t2.size(), 2);
    EXPECT_EQ(book.stopOrderCount(), 1); // Stop #3 still dormant ($98.00 not hit yet)
    EXPECT_EQ(book.lastTradedPrice(), Price(985'000));

    // Cancel remaining dormant stop order #3
    EXPECT_TRUE(book.cancelOrder(3));
    EXPECT_EQ(book.stopOrderCount(), 0);
}

TEST(OrderBookTest, ReserveOrderModifications)
{
    Book book(150);

    // Insert Reserve order: Buy 100 @ $100.00 with display quantity 25
    book.addOrder(OrderBuilder{}
                      .id(1)
                      .buy()
                      .limit(Price(1'000'000))
                      .reserve(Quantity(100), Quantity(25))
                      .build());

    const auto* node = book.findOrder(1);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->data.quantity, Quantity(25));
    EXPECT_EQ(node->reserve_quantity, Quantity(75));

    // 1. Modify quantity down to 80 (diff = 20):
    // Deducted from reserve: visible stays 25, reserve becomes 55.
    EXPECT_TRUE(book.modifyOrder(1, Quantity(80)));
    EXPECT_EQ(node->data.quantity, Quantity(25));
    EXPECT_EQ(node->reserve_quantity, Quantity(55));

    // 2. Modify quantity down to 20 (diff = 60):
    // Reserve (55) depleted completely, remaining 5 deducted from visible slice (25 -> 20).
    EXPECT_TRUE(book.modifyOrder(1, Quantity(20)));
    EXPECT_EQ(node->data.quantity, Quantity(20));
    EXPECT_EQ(node->reserve_quantity, Quantity(0));

    // 3. Modify quantity to 0 -> should cancel
    EXPECT_TRUE(book.modifyOrder(1, Quantity(0)));
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, ReserveOrderAggressiveEntry)
{
    Book book(150);

    // Resting ask: 30 @ $100.00
    book.addOrder(OrderBuilder{}.id(1).sell().limit(Price(1'000'000), Quantity(30)).build());

    // Incoming aggressive Reserve Buy: 50 @ $100.00 with display 10
    // - Immediately matches 30 @ $100.00
    // - Remaining 20 rests on bid side: visible = 10, reserve = 10
    auto trades = book.addOrder(OrderBuilder{}
                                    .id(2)
                                    .buy()
                                    .limit(Price(1'000'000))
                                    .reserve(Quantity(50), Quantity(10))
                                    .build());

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, Quantity(30));

    // Maker 1 is fully filled
    EXPECT_EQ(book.findOrder(1), nullptr);

    // Taker 2 rests on bid side
    const auto* node2 = book.findOrder(2);
    ASSERT_NE(node2, nullptr);
    EXPECT_EQ(node2->data.quantity, Quantity(10));
    EXPECT_EQ(node2->reserve_quantity, Quantity(10));
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), Price(1'000'000));
}

TEST(OrderBookTest, ReserveOrderCancellation)
{
    Book book(150);

    book.addOrder(OrderBuilder{}
                      .id(1)
                      .buy()
                      .limit(Price(1'000'000))
                      .reserve(Quantity(100), Quantity(25))
                      .build());

    EXPECT_EQ(book.orderCount(), 1);
    EXPECT_FALSE(book.empty());

    EXPECT_TRUE(book.cancelOrder(1));
    EXPECT_EQ(book.orderCount(), 0);
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.findOrder(1), nullptr);
}

TEST(OrderBookTest, StopLimitExecution)
{
    Book book(150);

    // Resting Ask: 100 @ $105.00
    book.addOrder(OrderBuilder{}.id(1).sell().limit(Price(1'050'000), Quantity(100)).build());

    // Establish last traded price at $100.00 by matching an order
    book.addOrder(OrderBuilder{}.id(2).buy().limit(Price(1'000'000), Quantity(10)).build());
    auto t1 = book.addOrder(OrderBuilder{}.id(3).sell().market(Quantity(10)).build());
    EXPECT_EQ(t1.size(), 1);
    EXPECT_EQ(book.lastTradedPrice(), Price(1'000'000));

    // Submit Buy Stop Limit: stop_price = $102.00, limit_price = $103.00, qty = 40
    book.addOrder(OrderBuilder{}
                      .id(4)
                      .buy()
                      .stopLimit(Price(1'020'000), Price(1'030'000), Quantity(40))
                      .build());

    EXPECT_EQ(book.stopOrderCount(), 1);
    EXPECT_FALSE(book.bestBid().has_value()); // dormant stop does not rest on bid side

    // Trigger trade at $102.00
    book.addOrder(OrderBuilder{}.id(5).buy().limit(Price(1'020'000), Quantity(10)).build());
    auto t2 = book.addOrder(OrderBuilder{}.id(6).sell().market(Quantity(10)).build());
    EXPECT_EQ(t2.size(), 1);
    EXPECT_EQ(book.lastTradedPrice(), Price(1'020'000));

    // Stop order #4 should be triggered, converted to LIMIT order at $103.00!
    // Since best ask is $105.00, it cannot match, so it rests on the bid side at $103.00!
    EXPECT_EQ(book.stopOrderCount(), 0);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(book.bestBid().value(), Price(1'030'000));
    const auto* node4 = book.findOrder(4);
    ASSERT_NE(node4, nullptr);
    EXPECT_EQ(node4->data.quantity, Quantity(40));
    EXPECT_EQ(node4->data.type, OrderBookUtils::OrderType::LIMIT);
}

TEST(OrderBookTest, StopOrderImmediateTrigger)
{
    Book book(150);

    // Resting Bid: 50 @ $100.00
    book.addOrder(OrderBuilder{}.id(1).buy().limit(Price(1'000'000), Quantity(50)).build());

    // Trade happens at $100.00
    auto t1 = book.addOrder(OrderBuilder{}.id(2).sell().market(Quantity(10)).build());
    EXPECT_EQ(t1.size(), 1);
    EXPECT_EQ(book.lastTradedPrice(), Price(1'000'000));

    // Incoming Sell Stop Market order with stop_price $100.50.
    // Since LTP is $100.00, the condition LTP <= stop_price ($100.00 <= $100.50) is ALREADY
    // satisfied! It must trigger immediately as an aggressive market sell order!
    auto t2 = book.addOrder(
        OrderBuilder{}.id(3).sell().stopMarket(Price(1'005'000), Quantity(20)).build());

    ASSERT_EQ(t2.size(), 1);
    EXPECT_EQ(t2[0].taker_order_id, 3);
    EXPECT_EQ(t2[0].maker_order_id, 1);
    EXPECT_EQ(t2[0].quantity, Quantity(20));
    EXPECT_EQ(book.stopOrderCount(), 0); // Not dormant, executed immediately!
}
