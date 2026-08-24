#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace OrderBookUtils
{

template <typename T>
class ObjectPool
{
public:
    explicit ObjectPool(std::size_t capacity)
        : pool_(capacity),
          capacity_(capacity)
    {
        if (capacity_ == 0)
            return;

        for (std::size_t i = 0; i < capacity_ - 1; ++i)
            pool_[i].next = &pool_[i + 1];

        pool_[capacity_ - 1].next = nullptr;
        free_head_ = &(pool_.front());
    }

    ~ObjectPool() = default;

    // Non-copyable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Movable
    ObjectPool(ObjectPool&& other) noexcept
        : pool_(std::move(other.pool_)),
          free_head_(std::exchange(other.free_head_, nullptr)),
          capacity_(std::exchange(other.capacity_, 0)),
          size_(std::exchange(other.size_, 0))
    {
    }

    ObjectPool& operator=(ObjectPool&& other) noexcept
    {
        if (this != &other)
        {
            pool_ = std::move(other.pool_);
            free_head_ = std::exchange(other.free_head_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
            size_ = std::exchange(other.size_, 0);
        }

        return *this;
    }

    template <typename... Args>
    [[nodiscard]] T* acquire(Args&&... args)
    {
        if (!free_head_) [[unlikely]]
            return nullptr;

        Slot* slot = free_head_;
        free_head_ = free_head_->next;
        ++size_;

        T* ptr = reinterpret_cast<T*>(slot->storage.data());
        try
        {
            std::construct_at(ptr, std::forward<Args>(args)...);
        }
        catch (...)
        {
            slot->next = free_head_;
            free_head_ = slot;
            --size_;
            throw;
        }

        return ptr;
    }

    void release(T* ptr) noexcept
    {
        if (ptr == nullptr) [[unlikely]]
            return;

        if constexpr (!std::is_trivially_destructible_v<T>)
            std::destroy_at(ptr);

        Slot* slot =
            reinterpret_cast<Slot*>(reinterpret_cast<std::byte*>(ptr) - offsetof(Slot, storage));
        slot->next = free_head_;
        free_head_ = slot;
        --size_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] std::size_t available() const noexcept
    {
        return capacity_ - size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] bool full() const noexcept
    {
        return free_head_ == nullptr;
    }

    [[nodiscard]] bool contains(const T* ptr) const noexcept
    {
        if (ptr == nullptr || pool_.empty())
            return false;

        const auto* byte_ptr = reinterpret_cast<const std::byte*>(ptr);
        const auto* start = reinterpret_cast<const std::byte*>(pool_.data());
        const auto* end = reinterpret_cast<const std::byte*>(pool_.data() + pool_.size());

        if (byte_ptr < start || byte_ptr >= end)
            return false;

        auto offset = static_cast<std::size_t>(byte_ptr - start);
        return (offset % sizeof(Slot)) == offsetof(Slot, storage);
    }

private:
    struct Slot
    {
        alignas(alignof(T)) std::array<std::byte, sizeof(T)> storage;
        Slot* next{nullptr};
    };

    std::vector<Slot> pool_;
    Slot* free_head_{nullptr};
    std::size_t capacity_{};
    std::size_t size_{};
};

} // namespace OrderBookUtils
