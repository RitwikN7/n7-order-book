#pragma once

#include "common/utils.hpp"
#include "order.hpp"

#include <cassert>
#include <cstdint>
#include <expected>
#include <string_view>

namespace MatchingEngine
{

enum class OrderValidationError : std::uint8_t
{
    NONE = 0,
    INVALID_ORDER_ID,
    ZERO_QUANTITY,
    INVALID_LIMIT_PRICE,
    INVALID_STOP_PRICE,
    INVALID_DISPLAY_QUANTITY
};

[[nodiscard]] constexpr std::string_view to_string(OrderValidationError error) noexcept
{
    switch (error)
    {
    case OrderValidationError::NONE:
        return "None";
    case OrderValidationError::INVALID_ORDER_ID:
        return "Order ID must be non-zero";
    case OrderValidationError::ZERO_QUANTITY:
        return "Order quantity must be greater than zero";
    case OrderValidationError::INVALID_LIMIT_PRICE:
        return "Limit order price must be greater than zero";
    case OrderValidationError::INVALID_STOP_PRICE:
        return "Stop order price must be greater than zero";
    case OrderValidationError::INVALID_DISPLAY_QUANTITY:
        return "Display quantity must not exceed total quantity";
    }
    return "Unknown validation error";
}

class OrderDataBuilder
{
public:
    constexpr OrderDataBuilder() noexcept = default;

    constexpr OrderDataBuilder& id(OrderBookUtils::OrderID order_id) noexcept
    {
        order_.order_id = order_id;
        return *this;
    }

    constexpr OrderDataBuilder& side(OrderBookUtils::OrderSide side) noexcept
    {
        order_.side = side;
        return *this;
    }

    constexpr OrderDataBuilder& buy() noexcept
    {
        order_.side = OrderBookUtils::OrderSide::BUY;
        return *this;
    }

    constexpr OrderDataBuilder& sell() noexcept
    {
        order_.side = OrderBookUtils::OrderSide::SELL;
        return *this;
    }

    constexpr OrderDataBuilder& type(OrderBookUtils::OrderType type) noexcept
    {
        order_.type = type;
        return *this;
    }

    constexpr OrderDataBuilder& price(OrderBookUtils::Price price) noexcept
    {
        order_.price = price;
        return *this;
    }

    constexpr OrderDataBuilder& stopPrice(OrderBookUtils::Price stop_price) noexcept
    {
        order_.stop_price = stop_price;
        return *this;
    }

    constexpr OrderDataBuilder& quantity(OrderBookUtils::Quantity quantity) noexcept
    {
        order_.quantity = quantity;
        return *this;
    }

    constexpr OrderDataBuilder& displayQuantity(OrderBookUtils::Quantity display_quantity) noexcept
    {
        order_.display_quantity = display_quantity;
        return *this;
    }

    // Convenience preset: Limit order
    constexpr OrderDataBuilder& limit(OrderBookUtils::Price price) noexcept
    {
        order_.type = OrderBookUtils::OrderType::LIMIT;
        order_.price = price;
        return *this;
    }

    constexpr OrderDataBuilder& limit(OrderBookUtils::Price price,
                                      OrderBookUtils::Quantity quantity) noexcept
    {
        order_.type = OrderBookUtils::OrderType::LIMIT;
        order_.price = price;
        order_.quantity = quantity;
        return *this;
    }

    // Convenience preset: Market order
    constexpr OrderDataBuilder& market() noexcept
    {
        order_.type = OrderBookUtils::OrderType::MARKET;
        order_.price = OrderBookUtils::Price(0);
        return *this;
    }

    constexpr OrderDataBuilder& market(OrderBookUtils::Quantity quantity) noexcept
    {
        order_.type = OrderBookUtils::OrderType::MARKET;
        order_.price = OrderBookUtils::Price(0);
        order_.quantity = quantity;
        return *this;
    }

    // Convenience preset: Stop Market order
    constexpr OrderDataBuilder& stopMarket(OrderBookUtils::Price stop_price) noexcept
    {
        order_.type = OrderBookUtils::OrderType::STOP_MARKET;
        order_.stop_price = stop_price;
        order_.price = OrderBookUtils::Price(0);
        return *this;
    }

    constexpr OrderDataBuilder& stopMarket(OrderBookUtils::Price stop_price,
                                           OrderBookUtils::Quantity quantity) noexcept
    {
        order_.type = OrderBookUtils::OrderType::STOP_MARKET;
        order_.stop_price = stop_price;
        order_.price = OrderBookUtils::Price(0);
        order_.quantity = quantity;
        return *this;
    }

    // Convenience preset: Stop Limit order
    constexpr OrderDataBuilder& stopLimit(OrderBookUtils::Price stop_price,
                                          OrderBookUtils::Price limit_price) noexcept
    {
        order_.type = OrderBookUtils::OrderType::STOP_LIMIT;
        order_.stop_price = stop_price;
        order_.price = limit_price;
        return *this;
    }

    constexpr OrderDataBuilder& stopLimit(OrderBookUtils::Price stop_price,
                                          OrderBookUtils::Price limit_price,
                                          OrderBookUtils::Quantity quantity) noexcept
    {
        order_.type = OrderBookUtils::OrderType::STOP_LIMIT;
        order_.stop_price = stop_price;
        order_.price = limit_price;
        order_.quantity = quantity;
        return *this;
    }

    // Convenience preset: Reserve order (total quantity + visible display quantity)
    constexpr OrderDataBuilder& reserve(OrderBookUtils::Quantity total_quantity,
                                        OrderBookUtils::Quantity display_quantity) noexcept
    {
        order_.quantity = total_quantity;
        order_.display_quantity = display_quantity;
        return *this;
    }

    [[nodiscard]] constexpr OrderValidationError validate() const noexcept
    {
        if (order_.order_id == 0)
            return OrderValidationError::INVALID_ORDER_ID;

        if (order_.quantity == OrderBookUtils::Quantity(0))
            return OrderValidationError::ZERO_QUANTITY;

        if (order_.type == OrderBookUtils::OrderType::LIMIT &&
            order_.price == OrderBookUtils::Price(0))
            return OrderValidationError::INVALID_LIMIT_PRICE;

        if (order_.type == OrderBookUtils::OrderType::STOP_MARKET &&
            order_.stop_price == OrderBookUtils::Price(0))
            return OrderValidationError::INVALID_STOP_PRICE;

        if (order_.type == OrderBookUtils::OrderType::STOP_LIMIT &&
            (order_.stop_price == OrderBookUtils::Price(0) ||
             order_.price == OrderBookUtils::Price(0)))
            return order_.stop_price == OrderBookUtils::Price(0)
                       ? OrderValidationError::INVALID_STOP_PRICE
                       : OrderValidationError::INVALID_LIMIT_PRICE;

        if (order_.display_quantity > order_.quantity)
            return OrderValidationError::INVALID_DISPLAY_QUANTITY;

        return OrderValidationError::NONE;
    }

    [[nodiscard]] constexpr std::expected<OrderData, OrderValidationError>
    buildChecked() const noexcept
    {
        auto err = validate();
        if (err != OrderValidationError::NONE)
            return std::unexpected(err);

        return order_;
    }

    [[nodiscard]] constexpr OrderData build() const noexcept
    {
        assert(validate() == OrderValidationError::NONE);
        return order_;
    }

private:
    OrderData order_{};
};

using OrderBuilder = OrderDataBuilder;

} // namespace MatchingEngine
