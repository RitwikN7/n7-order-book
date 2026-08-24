#pragma once

#include "common/utils.hpp"

namespace MatchingEngine
{

struct OrderData
{
    OrderBookUtils::OrderID order_id{};
    OrderBookUtils::Price price{}; // price * 10 ^ 4
    OrderBookUtils::Quantity quantity{};
    OrderBookUtils::OrderSide side{};
    OrderBookUtils::OrderType type{};
};

struct OrderNode : public OrderData
{
    OrderNode* prev{nullptr};
    OrderNode* next{nullptr};
};

} // namespace MatchingEngine
