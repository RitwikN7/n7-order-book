#pragma once

#include "../common/utils.hpp"

namespace MatchingEngine
{

struct Trade
{
    OrderBookUtils::OrderID maker_order_id{};
    OrderBookUtils::OrderID taker_order_id{};
    OrderBookUtils::Price price{};
    OrderBookUtils::Quantity quantity{};
};

} // namespace MatchingEngine
