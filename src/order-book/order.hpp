#pragma once

#include "common/utils.hpp"

namespace MatchingEngine
{

struct OrderData
{
    OrderBookUtils::OrderID order_id{};
    OrderBookUtils::Price price;       // price * 10 ^ 4
    OrderBookUtils::Price stop_price;  // trigger price for stop orders
    OrderBookUtils::Quantity quantity; // total order quantity
    OrderBookUtils::Quantity
        display_quantity; // visible quantity for reserve orders (0 = fully visible)
    OrderBookUtils::OrderSide side{};
    OrderBookUtils::OrderType type{};

    [[nodiscard]] constexpr bool isReserve() const noexcept
    {
        return display_quantity > OrderBookUtils::Quantity(0) && display_quantity < quantity;
    }

    [[nodiscard]] constexpr bool isStop() const noexcept
    {
        return OrderBookUtils::isStopOrder(type);
    }
};

struct alignas(64) OrderNode
{
    OrderData data{};
    OrderBookUtils::Quantity reserve_quantity; // remaining hidden quantity for reserve orders
    OrderNode* prev{nullptr};
    OrderNode* next{nullptr};

    [[nodiscard]] constexpr bool hasReserve() const noexcept
    {
        return reserve_quantity > OrderBookUtils::Quantity(0);
    }

    [[nodiscard]] constexpr bool isReserve() const noexcept
    {
        return data.isReserve();
    }

    [[nodiscard]] constexpr bool isStop() const noexcept
    {
        return data.isStop();
    }
};

static_assert(sizeof(OrderNode) == 64,
              "OrderNode must be exactly 64 bytes to match cache line size");
static_assert(alignof(OrderNode) == 64,
              "OrderNode must be 64-byte aligned for cache line efficiency");

} // namespace MatchingEngine
