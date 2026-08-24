#include "book.hpp"

#include "common/utils.hpp"
#include "order.hpp"
#include "price_level.hpp"
#include "trade.hpp"

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <vector>

namespace MatchingEngine
{

Book::Book(std::size_t max_orders)
    : order_pool_(max_orders),
      orders_map_(max_orders),
      pool_resource_(std::pmr::pool_options{.max_blocks_per_chunk = 65536,
                                            .largest_required_pool_block = 256}),
      bids_(&pool_resource_),
      asks_(&pool_resource_)
{
}

std::vector<Trade> Book::addOrder(const OrderData& order)
{
    if (order.quantity == 0) [[unlikely]]
        return {};

    OrderData incoming = order;

    if (incoming.side == OrderBookUtils::OrderSide::BUY)
        return matchBuyOrder(incoming);
    else
        return matchSellOrder(incoming);
}

std::vector<Trade> Book::matchBuyOrder(OrderData& order)
{
    std::vector<Trade> trades;
    trades.reserve(8);

    while (order.quantity > 0 && !asks_.empty())
    {
        auto best_ask_it = asks_.begin();

        if (order.type == OrderBookUtils::OrderType::LIMIT && order.price < best_ask_it->first)
            break;

        PriceLevel& level = best_ask_it->second;

        while (order.quantity > 0 && !level.empty())
        {
            OrderNode* maker = level.head;
            OrderBookUtils::Quantity fill_qty = std::min(order.quantity, maker->quantity);

            trades.push_back(Trade{
                .maker_order_id = maker->order_id,
                .taker_order_id = order.order_id,
                .price = maker->price,
                .quantity = fill_qty,
            });

            order.quantity -= fill_qty;
            maker->quantity -= fill_qty;
            level.total_volume -= fill_qty;

            if (maker->quantity == 0)
            {
                level.remove(maker);
                orders_map_.erase(maker->order_id);
                order_pool_.release(maker);
            }
        }

        if (level.empty())
            asks_.erase(best_ask_it);
    }

    if (order.quantity > 0 && order.type == OrderBookUtils::OrderType::LIMIT)
        insertRestingOrder(order);

    return trades;
}

std::vector<Trade> Book::matchSellOrder(OrderData& order)
{
    std::vector<Trade> trades;
    trades.reserve(8);

    while (order.quantity > 0 && !bids_.empty())
    {
        auto best_bid_it = bids_.begin();

        if (order.type == OrderBookUtils::OrderType::LIMIT && order.price > best_bid_it->first)
            break;

        PriceLevel& level = best_bid_it->second;

        while (order.quantity > 0 && !level.empty())
        {
            OrderNode* maker = level.head;
            OrderBookUtils::Quantity fill_qty = std::min(order.quantity, maker->quantity);

            trades.push_back(Trade{
                .maker_order_id = maker->order_id,
                .taker_order_id = order.order_id,
                .price = maker->price,
                .quantity = fill_qty,
            });

            order.quantity -= fill_qty;
            maker->quantity -= fill_qty;
            level.total_volume -= fill_qty;

            if (maker->quantity == 0)
            {
                level.remove(maker);
                orders_map_.erase(maker->order_id);
                order_pool_.release(maker);
            }
        }

        if (level.empty())
            bids_.erase(best_bid_it);
    }

    if (order.quantity > 0 && order.type == OrderBookUtils::OrderType::LIMIT)
        insertRestingOrder(order);

    return trades;
}

void Book::insertRestingOrder(const OrderData& order)
{
    OrderNode* node = order_pool_.acquire();
    if (node == nullptr) [[unlikely]]
        return;

    node->order_id = order.order_id;
    node->price = order.price;
    node->quantity = order.quantity;
    node->side = order.side;
    node->type = order.type;

    orders_map_.insert(node->order_id, node);

    if (node->side == OrderBookUtils::OrderSide::BUY)
        bids_[node->price].append(node);
    else
        asks_[node->price].append(node);
}

bool Book::cancelOrder(OrderBookUtils::OrderID order_id)
{
    auto* node_ptr = orders_map_.find(order_id);
    if (node_ptr == nullptr)
        return false;

    OrderNode* node = *node_ptr;

    if (node->side == OrderBookUtils::OrderSide::BUY)
    {
        auto level_it = bids_.find(node->price);
        if (level_it != bids_.end())
        {
            level_it->second.remove(node);
            if (level_it->second.empty())
                bids_.erase(level_it);
        }
    }
    else
    {
        auto level_it = asks_.find(node->price);
        if (level_it != asks_.end())
        {
            level_it->second.remove(node);
            if (level_it->second.empty())
                asks_.erase(level_it);
        }
    }

    orders_map_.erase(order_id);
    order_pool_.release(node);
    return true;
}

bool Book::modifyOrder(OrderBookUtils::OrderID order_id, OrderBookUtils::Price new_price,
                       OrderBookUtils::Quantity new_quantity)
{
    if (new_quantity == 0)
        return cancelOrder(order_id);

    auto* node_ptr = orders_map_.find(order_id);
    if (node_ptr == nullptr)
        return false;

    OrderNode* node = *node_ptr;

    if (new_price == node->price && new_quantity <= node->quantity)
    {
        OrderBookUtils::Quantity diff = node->quantity - new_quantity;
        node->quantity = new_quantity;

        if (node->side == OrderBookUtils::OrderSide::BUY)
        {
            auto level_it = bids_.find(node->price);
            if (level_it != bids_.end())
                level_it->second.total_volume -= diff;
        }
        else
        {
            auto level_it = asks_.find(node->price);
            if (level_it != asks_.end())
                level_it->second.total_volume -= diff;
        }

        return true;
    }
    else
    {
        OrderData updated_order{
            .order_id = node->order_id,
            .price = new_price,
            .quantity = new_quantity,
            .side = node->side,
            .type = node->type,
        };

        cancelOrder(order_id);
        addOrder(updated_order);
        return true;
    }
}

bool Book::modifyOrder(OrderBookUtils::OrderID order_id, OrderBookUtils::Quantity new_quantity)
{
    auto* node_ptr = orders_map_.find(order_id);
    if (node_ptr == nullptr)
        return false;

    return modifyOrder(order_id, (*node_ptr)->price, new_quantity);
}

const OrderNode* Book::findOrder(OrderBookUtils::OrderID order_id) const
{
    const auto* node_ptr = orders_map_.find(order_id);
    if (node_ptr == nullptr)
        return nullptr;

    return *node_ptr;
}

std::optional<OrderBookUtils::Price> Book::bestBid() const
{
    if (bids_.empty())
        return std::nullopt;

    return bids_.begin()->first;
}

std::optional<OrderBookUtils::Price> Book::bestAsk() const
{
    if (asks_.empty())
        return std::nullopt;

    return asks_.begin()->first;
}

std::optional<OrderBookUtils::Price> Book::spread() const
{
    if (bids_.empty() || asks_.empty())
        return std::nullopt;

    return asks_.begin()->first - bids_.begin()->first;
}

std::size_t Book::orderCount() const noexcept
{
    return orders_map_.size();
}

bool Book::empty() const noexcept
{
    return orders_map_.empty();
}

} // namespace MatchingEngine
