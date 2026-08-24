#pragma once

#include "common/utils.hpp"
#include "order.hpp"

#include <cstddef>

namespace MatchingEngine
{

struct PriceLevel
{
    void append(OrderNode* order)
    {
        order->prev = tail;
        order->next = nullptr;

        if (tail != nullptr)
            tail->next = order;
        else
            head = order;

        tail = order;
        total_volume += order->quantity;
        ++order_count;
        price = order->price;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return head == nullptr;
    }

    void remove(OrderNode* order)
    {
        if (order->prev != nullptr)
            order->prev->next = order->next;
        else
            head = order->next;

        if (order->next != nullptr)
            order->next->prev = order->prev;
        else
            tail = order->prev;

        order->prev = nullptr;
        order->next = nullptr;

        total_volume -= order->quantity;
        --order_count;
    }

    OrderNode* head{nullptr};
    OrderNode* tail{nullptr};

    OrderBookUtils::Price price{};
    OrderBookUtils::Quantity total_volume{};
    std::size_t order_count{};
};

} // namespace MatchingEngine
