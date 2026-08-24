#pragma once

#include "common/flat_hash_map.hpp"
#include "common/object_pool.hpp"
#include "common/utils.hpp"
#include "order.hpp"
#include "price_level.hpp"
#include "trade.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory_resource>
#include <optional>
#include <vector>

namespace MatchingEngine
{

class Book
{
public:
    explicit Book(std::size_t max_orders = 1'000'000);
    ~Book() = default;

    // Non-copyable
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    // Non-movable
    Book(Book&&) noexcept = delete;
    Book& operator=(Book&&) noexcept = delete;

    std::vector<Trade> addOrder(const OrderData& order);
    bool cancelOrder(OrderBookUtils::OrderID order_id);
    bool modifyOrder(OrderBookUtils::OrderID order_id, OrderBookUtils::Quantity new_quantity);
    bool modifyOrder(OrderBookUtils::OrderID order_id, OrderBookUtils::Price new_price,
                     OrderBookUtils::Quantity new_quantity);

    [[nodiscard]] const OrderNode* findOrder(OrderBookUtils::OrderID order_id) const;

    [[nodiscard]] std::optional<OrderBookUtils::Price> bestBid() const;
    [[nodiscard]] std::optional<OrderBookUtils::Price> bestAsk() const;
    [[nodiscard]] std::optional<OrderBookUtils::Price> spread() const;

    [[nodiscard]] std::size_t orderCount() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::vector<Trade> matchBuyOrder(OrderData& order);
    std::vector<Trade> matchSellOrder(OrderData& order);

    void insertRestingOrder(const OrderData& order);

    OrderBookUtils::ObjectPool<OrderNode> order_pool_;
    OrderBookUtils::FlatHashMap<OrderBookUtils::OrderID, OrderNode*> orders_map_;

    std::pmr::unsynchronized_pool_resource pool_resource_;
    std::pmr::map<OrderBookUtils::Price, PriceLevel, std::greater<>> bids_;
    std::pmr::map<OrderBookUtils::Price, PriceLevel, std::less<>> asks_;
};

} // namespace MatchingEngine