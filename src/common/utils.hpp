#pragma once

#include <cstdint>

namespace OrderBookUtils
{

using OrderID = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;

enum class OrderSide : std::uint8_t
{
    BUY,
    SELL
};

enum class OrderType : std::uint8_t
{
    MARKET,
    LIMIT
};

} // namespace OrderBookUtils
