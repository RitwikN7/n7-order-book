#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace OrderBookUtils
{

template <typename Key, typename Value>
class FlatHashMap
{
public:
    explicit FlatHashMap(std::size_t min_capacity = 64)
    {
        reserve(min_capacity);
    }

    void reserve(std::size_t capacity)
    {
        // Require power of two capacity for fast bitmasking
        // Keep maximum load factor <= 0.70
        std::size_t required_slots = ((capacity * 10) / 7) + 16;
        std::size_t power_of_two = std::bit_ceil(required_slots);
        power_of_two = std::max<std::size_t>(power_of_two, 16);

        if (power_of_two <= capacity_)
            return;

        rehash(power_of_two);
    }

    [[nodiscard]] Value* find(const Key& key) noexcept
    {
        if (capacity_ == 0 || size_ == 0) [[unlikely]]
            return nullptr;

        std::size_t idx = hashKey(key) & mask_;
        while (occupied_[idx])
        {
            if (keys_[idx] == key)
                return &values_[idx];
            idx = (idx + 1) & mask_;
        }
        return nullptr;
    }

    [[nodiscard]] const Value* find(const Key& key) const noexcept
    {
        if (capacity_ == 0 || size_ == 0) [[unlikely]]
            return nullptr;

        std::size_t idx = hashKey(key) & mask_;
        while (occupied_[idx])
        {
            if (keys_[idx] == key)
                return &values_[idx];
            idx = (idx + 1) & mask_;
        }
        return nullptr;
    }

    bool insert(const Key& key, const Value& value)
    {
        if ((size_ + 1) * 10 >= capacity_ * 7) [[unlikely]]
        {
            reserve(capacity_ * 2);
        }

        std::size_t idx = hashKey(key) & mask_;
        while (occupied_[idx])
        {
            if (keys_[idx] == key)
            {
                values_[idx] = value;
                return false; // updated existing
            }
            idx = (idx + 1) & mask_;
        }

        occupied_[idx] = 1;
        keys_[idx] = key;
        values_[idx] = value;
        ++size_;
        return true;
    }

    bool erase(const Key& key) noexcept
    {
        if (capacity_ == 0 || size_ == 0) [[unlikely]]
            return false;

        std::size_t idx = hashKey(key) & mask_;
        while (occupied_[idx])
        {
            if (keys_[idx] == key)
            {
                // Backward-shift deletion: moves shifted elements back,
                // completely eliminating tombstones and keeping probe sequences optimal
                std::size_t curr = idx;
                std::size_t next = (curr + 1) & mask_;

                while (occupied_[next])
                {
                    std::size_t natural_idx = hashKey(keys_[next]) & mask_;
                    // Check if 'next' element belongs before or at 'curr'
                    bool belongs_before = (curr < next)
                                              ? (natural_idx <= curr || natural_idx > next)
                                              : (natural_idx <= curr && natural_idx > next);

                    if (belongs_before)
                    {
                        keys_[curr] = keys_[next];
                        values_[curr] = values_[next];
                        curr = next;
                    }
                    next = (next + 1) & mask_;
                }

                occupied_[curr] = 0;
                --size_;
                return true;
            }
            idx = (idx + 1) & mask_;
        }

        return false;
    }

    void clear() noexcept
    {
        if (capacity_ > 0)
        {
            std::ranges::fill(occupied_, static_cast<std::uint8_t>(0));
            size_ = 0;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    static std::size_t hashKey(const Key& k) noexcept
    {
        // SplitMix64-based high entropy hash function
        auto x = static_cast<std::uint64_t>(k);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x);
    }

    void rehash(std::size_t new_capacity)
    {
        std::vector<std::uint8_t> old_occupied = std::move(occupied_);
        std::vector<Key> old_keys = std::move(keys_);
        std::vector<Value> old_values = std::move(values_);
        std::size_t old_capacity = capacity_;

        capacity_ = new_capacity;
        mask_ = new_capacity - 1;
        size_ = 0;

        occupied_.assign(capacity_, 0);
        keys_.resize(capacity_);
        values_.resize(capacity_);

        for (std::size_t i = 0; i < old_capacity; ++i)
        {
            if (old_occupied[i])
            {
                insert(old_keys[i], old_values[i]);
            }
        }
    }

    std::vector<std::uint8_t> occupied_;
    std::vector<Key> keys_;
    std::vector<Value> values_;
    std::size_t capacity_{};
    std::size_t mask_{};
    std::size_t size_{};
};

} // namespace OrderBookUtils
