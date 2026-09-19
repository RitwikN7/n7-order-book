#include "book.hpp"

#include "common/flat_hash_map.hpp"
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
      asks_(&pool_resource_),
      stop_bids_(&pool_resource_),
      stop_asks_(&pool_resource_)
{
}

std::vector<Trade> Book::addOrder(const OrderData& order)
{
    if (order.quantity == OrderBookUtils::Quantity(0)) [[unlikely]]
        return {};

    if (order.isStop()) [[unlikely]]
        return handleStopOrder(order);

    OrderData incoming = order;
    std::vector<Trade> trades;

    if (incoming.side == OrderBookUtils::OrderSide::BUY)
        trades = matchBuyOrder(incoming);
    else
        trades = matchSellOrder(incoming);

    if (!trades.empty() && stop_order_count_ > 0) [[unlikely]]
        triggerStopOrders(trades);

    return trades;
}

std::vector<Trade> Book::handleStopOrder(const OrderData& order)
{
    bool should_trigger = false;
    if (last_traded_price_.has_value())
    {
        if (order.side == OrderBookUtils::OrderSide::BUY && *last_traded_price_ >= order.stop_price)
            should_trigger = true;
        else if (order.side == OrderBookUtils::OrderSide::SELL &&
                 *last_traded_price_ <= order.stop_price)
            should_trigger = true;
    }

    if (should_trigger)
    {
        OrderData triggered_order = order;
        if (triggered_order.type == OrderBookUtils::OrderType::STOP_MARKET)
            triggered_order.type = OrderBookUtils::OrderType::MARKET;
        else if (triggered_order.type == OrderBookUtils::OrderType::STOP_LIMIT)
            triggered_order.type = OrderBookUtils::OrderType::LIMIT;

        std::vector<Trade> trades;
        if (triggered_order.side == OrderBookUtils::OrderSide::BUY)
            trades = matchBuyOrder(triggered_order);
        else
            trades = matchSellOrder(triggered_order);

        if (!trades.empty() && stop_order_count_ > 0) [[unlikely]]
            triggerStopOrders(trades);

        return trades;
    }

    OrderNode* node = order_pool_.acquire();
    if (node == nullptr) [[unlikely]]
        return {};

    node->data = order;
    node->prev = nullptr;
    node->next = nullptr;
    node->reserve_quantity = OrderBookUtils::Quantity(0);

    orders_map_.insert(node->data.order_id, node);

    if (node->data.side == OrderBookUtils::OrderSide::BUY)
        stop_bids_[node->data.stop_price].append(node);
    else
        stop_asks_[node->data.stop_price].append(node);

    ++stop_order_count_;
    return {};
}

void Book::triggerStopOrders(std::vector<Trade>& trades)
{
    if (stop_order_count_ == 0) [[likely]]
        return;

    if (!last_traded_price_.has_value()) [[unlikely]]
        return;

    bool triggered_any = true;
    while (triggered_any)
    {
        triggered_any = false;
        OrderBookUtils::Price current_ltp = *last_traded_price_;

        // 1. Process Buy Stops (trigger when market price >= stop_price)
        // stop_bids_ is sorted ascending (std::less<>), so lowest stop_price is at begin()
        while (!stop_bids_.empty())
        {
            auto it = stop_bids_.begin();
            if (current_ltp < it->first)
                break;

            PriceLevel& level = it->second;
            while (!level.empty())
            {
                OrderNode* node = level.head;
                OrderData triggered_order = node->data;

                level.remove(node);
                --stop_order_count_;

                if (triggered_order.type == OrderBookUtils::OrderType::STOP_MARKET)
                    triggered_order.type = OrderBookUtils::OrderType::MARKET;
                else if (triggered_order.type == OrderBookUtils::OrderType::STOP_LIMIT)
                    triggered_order.type = OrderBookUtils::OrderType::LIMIT;

                orders_map_.erase(node->data.order_id);
                order_pool_.release(node);

                std::vector<Trade> new_trades = matchBuyOrder(triggered_order);
                trades.insert(trades.end(), new_trades.begin(), new_trades.end());
                triggered_any = true;
            }

            if (level.empty())
                stop_bids_.erase(it);
        }

        current_ltp = *last_traded_price_;

        // 2. Process Sell Stops (trigger when market price <= stop_price)
        // stop_asks_ is sorted descending (std::greater<>), so highest stop_price is at begin()
        while (!stop_asks_.empty())
        {
            auto it = stop_asks_.begin();
            if (current_ltp > it->first)
                break;

            PriceLevel& level = it->second;
            while (!level.empty())
            {
                OrderNode* node = level.head;
                OrderData triggered_order = node->data;

                level.remove(node);
                --stop_order_count_;

                if (triggered_order.type == OrderBookUtils::OrderType::STOP_MARKET)
                    triggered_order.type = OrderBookUtils::OrderType::MARKET;
                else if (triggered_order.type == OrderBookUtils::OrderType::STOP_LIMIT)
                    triggered_order.type = OrderBookUtils::OrderType::LIMIT;

                orders_map_.erase(node->data.order_id);
                order_pool_.release(node);

                std::vector<Trade> new_trades = matchSellOrder(triggered_order);
                trades.insert(trades.end(), new_trades.begin(), new_trades.end());
                triggered_any = true;
            }

            if (level.empty())
                stop_asks_.erase(it);
        }
    }
}

std::vector<Trade> Book::matchBuyOrder(OrderData& order)
{
    std::vector<Trade> trades;
    trades.reserve(8);

    while (order.quantity > OrderBookUtils::Quantity(0) && !asks_.empty())
    {
        auto best_ask_it = asks_.begin();

        if (order.type == OrderBookUtils::OrderType::LIMIT && order.price < best_ask_it->first)
            break;

        PriceLevel& level = best_ask_it->second;

        while (order.quantity > OrderBookUtils::Quantity(0) && !level.empty())
        {
            OrderNode* maker = level.head;
            OrderBookUtils::Quantity fill_qty = std::min(order.quantity, maker->data.quantity);

            trades.push_back(Trade{
                .maker_order_id = maker->data.order_id,
                .taker_order_id = order.order_id,
                .price = maker->data.price,
                .quantity = fill_qty,
            });

            last_traded_price_ = maker->data.price;

            order.quantity -= fill_qty;
            maker->data.quantity -= fill_qty;
            level.total_volume -= fill_qty;

            if (maker->data.quantity == OrderBookUtils::Quantity(0))
            {
                if (maker->hasReserve()) [[unlikely]]
                {
                    level.remove(maker);
                    OrderBookUtils::Quantity slice =
                        std::min(maker->data.display_quantity, maker->reserve_quantity);
                    maker->data.quantity = slice;
                    maker->reserve_quantity -= slice;
                    level.append(maker); // Re-queue to tail of price level to reset time priority
                }
                else [[likely]]
                {
                    level.remove(maker);
                    orders_map_.erase(maker->data.order_id);
                    order_pool_.release(maker);
                }
            }
        }

        if (level.empty())
            asks_.erase(best_ask_it);
    }

    if (order.quantity > OrderBookUtils::Quantity(0) &&
        order.type == OrderBookUtils::OrderType::LIMIT)
        insertRestingOrder(order);

    return trades;
}

std::vector<Trade> Book::matchSellOrder(OrderData& order)
{
    std::vector<Trade> trades;
    trades.reserve(8);

    while (order.quantity > OrderBookUtils::Quantity(0) && !bids_.empty())
    {
        auto best_bid_it = bids_.begin();

        if (order.type == OrderBookUtils::OrderType::LIMIT && order.price > best_bid_it->first)
            break;

        PriceLevel& level = best_bid_it->second;

        while (order.quantity > OrderBookUtils::Quantity(0) && !level.empty())
        {
            OrderNode* maker = level.head;
            OrderBookUtils::Quantity fill_qty = std::min(order.quantity, maker->data.quantity);

            trades.push_back(Trade{
                .maker_order_id = maker->data.order_id,
                .taker_order_id = order.order_id,
                .price = maker->data.price,
                .quantity = fill_qty,
            });

            last_traded_price_ = maker->data.price;

            order.quantity -= fill_qty;
            maker->data.quantity -= fill_qty;
            level.total_volume -= fill_qty;

            if (maker->data.quantity == OrderBookUtils::Quantity(0))
            {
                if (maker->hasReserve()) [[unlikely]]
                {
                    level.remove(maker);
                    OrderBookUtils::Quantity slice =
                        std::min(maker->data.display_quantity, maker->reserve_quantity);
                    maker->data.quantity = slice;
                    maker->reserve_quantity -= slice;
                    level.append(maker); // Re-queue to tail of price level to reset time priority
                }
                else [[likely]]
                {
                    level.remove(maker);
                    orders_map_.erase(maker->data.order_id);
                    order_pool_.release(maker);
                }
            }
        }

        if (level.empty())
            bids_.erase(best_bid_it);
    }

    if (order.quantity > OrderBookUtils::Quantity(0) &&
        order.type == OrderBookUtils::OrderType::LIMIT)
        insertRestingOrder(order);

    return trades;
}

void Book::insertRestingOrder(const OrderData& order)
{
    OrderNode* node = order_pool_.acquire();
    if (node == nullptr) [[unlikely]]
        return;

    node->data = order;
    node->prev = nullptr;
    node->next = nullptr;

    if (order.isReserve())
    {
        OrderBookUtils::Quantity visible_qty = std::min(order.display_quantity, order.quantity);
        node->data.quantity = visible_qty;
        node->reserve_quantity = order.quantity - visible_qty;
    }
    else
    {
        node->reserve_quantity = OrderBookUtils::Quantity(0);
    }

    orders_map_.insert(node->data.order_id, node);

    if (node->data.side == OrderBookUtils::OrderSide::BUY)
        bids_[node->data.price].append(node);
    else
        asks_[node->data.price].append(node);
}

bool Book::cancelOrder(OrderBookUtils::OrderID order_id)
{
    std::size_t idx = orders_map_.findIndex(order_id);
    if (idx == OrderBookUtils::FlatHashMap<OrderBookUtils::OrderID, OrderNode*>::INVALID_INDEX)
        [[unlikely]]
        return false;

    OrderNode* node = orders_map_.valueAt(idx);

    if (node->isStop()) [[unlikely]]
    {
        if (node->data.side == OrderBookUtils::OrderSide::BUY)
        {
            auto level_it = stop_bids_.find(node->data.stop_price);
            if (level_it != stop_bids_.end())
            {
                level_it->second.remove(node);
                if (level_it->second.empty())
                    stop_bids_.erase(level_it);
            }
        }
        else
        {
            auto level_it = stop_asks_.find(node->data.stop_price);
            if (level_it != stop_asks_.end())
            {
                level_it->second.remove(node);
                if (level_it->second.empty())
                    stop_asks_.erase(level_it);
            }
        }
        --stop_order_count_;
    }
    else
    {
        if (node->data.side == OrderBookUtils::OrderSide::BUY)
        {
            auto level_it = bids_.find(node->data.price);
            if (level_it != bids_.end())
            {
                level_it->second.remove(node);
                if (level_it->second.empty())
                    bids_.erase(level_it);
            }
        }
        else
        {
            auto level_it = asks_.find(node->data.price);
            if (level_it != asks_.end())
            {
                level_it->second.remove(node);
                if (level_it->second.empty())
                    asks_.erase(level_it);
            }
        }
    }

    orders_map_.eraseAt(idx);
    order_pool_.release(node);
    return true;
}

bool Book::modifyOrder(OrderBookUtils::OrderID order_id, OrderBookUtils::Price new_price,
                       OrderBookUtils::Quantity new_quantity)
{
    if (new_quantity == OrderBookUtils::Quantity(0)) [[unlikely]]
        return cancelOrder(order_id);

    auto* node_ptr = orders_map_.find(order_id);
    if (node_ptr == nullptr) [[unlikely]]
        return false;

    OrderNode* node = *node_ptr;

    if (node->isStop()) [[unlikely]]
    {
        OrderData updated_order = node->data;
        updated_order.stop_price = new_price;
        updated_order.quantity = new_quantity;

        cancelOrder(order_id);
        addOrder(updated_order);
        return true;
    }

    OrderBookUtils::Quantity total_node_qty = node->data.quantity + node->reserve_quantity;

    if (new_price == node->data.price && new_quantity <= total_node_qty)
    {
        OrderBookUtils::Quantity diff = total_node_qty - new_quantity;

        if (node->reserve_quantity > OrderBookUtils::Quantity(0))
        {
            OrderBookUtils::Quantity reserve_deduct = std::min(node->reserve_quantity, diff);
            node->reserve_quantity -= reserve_deduct;
            diff -= reserve_deduct;
        }

        if (diff > OrderBookUtils::Quantity(0))
        {
            node->data.quantity -= diff;
            if (node->data.side == OrderBookUtils::OrderSide::BUY)
            {
                auto level_it = bids_.find(node->data.price);
                if (level_it != bids_.end())
                    level_it->second.total_volume -= diff;
            }
            else
            {
                auto level_it = asks_.find(node->data.price);
                if (level_it != asks_.end())
                    level_it->second.total_volume -= diff;
            }
        }

        return true;
    }
    else
    {
        OrderData updated_order = node->data;
        updated_order.price = new_price;
        updated_order.quantity = new_quantity;

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

    return modifyOrder(order_id, (*node_ptr)->data.price, new_quantity);
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

std::optional<OrderBookUtils::Price> Book::lastTradedPrice() const noexcept
{
    return last_traded_price_;
}

std::size_t Book::orderCount() const noexcept
{
    return orders_map_.size();
}

std::size_t Book::activeOrderCount() const noexcept
{
    return orders_map_.size() - stop_order_count_;
}

std::size_t Book::stopOrderCount() const noexcept
{
    return stop_order_count_;
}

bool Book::empty() const noexcept
{
    return orders_map_.empty();
}

} // namespace MatchingEngine
