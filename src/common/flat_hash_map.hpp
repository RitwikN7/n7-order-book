#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace OrderBookUtils
{

template <typename Key, typename Value>
class FlatHashMap
{
public:
    static constexpr std::size_t INVALID_INDEX = std::numeric_limits<std::size_t>::max();

    struct Entry
    {
        Key key{};
        Value value{};
        bool occupied{false};
    };

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

    [[nodiscard]] std::size_t findIndex(const Key& key) const noexcept
    {
        if (capacity_ == 0 || size_ == 0) [[unlikely]]
            return INVALID_INDEX;

        std::size_t idx = hashKey(key) & mask_;
        while (entries_[idx].occupied)
        {
            if (entries_[idx].key == key)
                return idx;
            idx = (idx + 1) & mask_;
        }
        return INVALID_INDEX;
    }

    [[nodiscard]] Value* find(const Key& key) noexcept
    {
        std::size_t idx = findIndex(key);
        return (idx != INVALID_INDEX) ? &entries_[idx].value : nullptr;
    }

    [[nodiscard]] const Value* find(const Key& key) const noexcept
    {
        std::size_t idx = findIndex(key);
        return (idx != INVALID_INDEX) ? &entries_[idx].value : nullptr;
    }

    [[nodiscard]] Value& valueAt(std::size_t idx) noexcept
    {
        return entries_[idx].value;
    }

    [[nodiscard]] const Value& valueAt(std::size_t idx) const noexcept
    {
        return entries_[idx].value;
    }

    bool insert(const Key& key, const Value& value)
    {
        if ((size_ + 1) * 10 >= capacity_ * 7) [[unlikely]]
        {
            reserve(capacity_ * 2);
        }

        std::size_t idx = hashKey(key) & mask_;
        while (entries_[idx].occupied)
        {
            if (entries_[idx].key == key)
            {
                entries_[idx].value = value;
                return false; // updated existing
            }
            idx = (idx + 1) & mask_;
        }

        entries_[idx] = Entry{key, value, true};
        ++size_;
        return true;
    }

    void eraseAt(std::size_t idx) noexcept
    {
        if (capacity_ == 0 || size_ == 0 || idx >= capacity_ || !entries_[idx].occupied)
            [[unlikely]]
            return;

        // Backward-shift deletion: moves shifted elements back,
        // completely eliminating tombstones and keeping probe sequences optimal
        std::size_t curr = idx;
        std::size_t next = (curr + 1) & mask_;

        while (entries_[next].occupied)
        {
            std::size_t natural_idx = hashKey(entries_[next].key) & mask_;
            // Check if 'next' element belongs before or at 'curr'
            bool belongs_before = (curr < next) ? (natural_idx <= curr || natural_idx > next)
                                                : (natural_idx <= curr && natural_idx > next);

            if (belongs_before)
            {
                entries_[curr] = entries_[next];
                curr = next;
            }
            next = (next + 1) & mask_;
        }

        entries_[curr].occupied = false;
        --size_;
    }

    bool erase(const Key& key) noexcept
    {
        std::size_t idx = findIndex(key);
        if (idx == INVALID_INDEX)
            return false;

        eraseAt(idx);
        return true;
    }

    void clear() noexcept
    {
        if (capacity_ > 0)
        {
            for (auto& entry : entries_)
            {
                entry.occupied = false;
            }
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
        std::vector<Entry> old_entries = std::move(entries_);
        std::size_t old_capacity = capacity_;

        capacity_ = new_capacity;
        mask_ = new_capacity - 1;
        size_ = 0;

        entries_.assign(capacity_, Entry{});

        for (std::size_t i = 0; i < old_capacity; ++i)
        {
            if (old_entries[i].occupied)
            {
                insert(old_entries[i].key, old_entries[i].value);
            }
        }
    }

    std::vector<Entry> entries_;
    std::size_t capacity_{};
    std::size_t mask_{};
    std::size_t size_{};
};

} // namespace OrderBookUtils
