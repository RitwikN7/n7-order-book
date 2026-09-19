#pragma once

#include <cstdint>
#include <ostream>

namespace OrderBookUtils
{

using OrderID = std::uint64_t;
class Price
{
public:
    using rep = std::uint32_t;

    constexpr Price() noexcept = default;
    explicit constexpr Price(rep val) noexcept
        : value_(val)
    {
    }

    [[nodiscard]] constexpr rep value() const noexcept
    {
        return value_;
    }

    constexpr auto operator<=>(const Price&) const noexcept = default;

    constexpr Price operator+() const noexcept
    {
        return *this;
    }

    constexpr Price& operator+=(Price other) noexcept
    {
        value_ += other.value_;
        return *this;
    }

    constexpr Price& operator-=(Price other) noexcept
    {
        value_ -= other.value_;
        return *this;
    }

    friend constexpr Price operator+(Price lhs, Price rhs) noexcept
    {
        return Price(lhs.value_ + rhs.value_);
    }

    friend constexpr Price operator-(Price lhs, Price rhs) noexcept
    {
        return Price(lhs.value_ - rhs.value_);
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os,
                                                         Price p)
    {
        return os << p.value_;
    }

private:
    rep value_{0};
};

class Quantity
{
public:
    using rep = std::uint64_t;

    constexpr Quantity() noexcept = default;
    explicit constexpr Quantity(rep val) noexcept
        : value_(val)
    {
    }

    [[nodiscard]] constexpr rep value() const noexcept
    {
        return value_;
    }

    constexpr auto operator<=>(const Quantity&) const noexcept = default;

    constexpr Quantity& operator+=(Quantity other) noexcept
    {
        value_ += other.value_;
        return *this;
    }

    constexpr Quantity& operator-=(Quantity other) noexcept
    {
        value_ -= other.value_;
        return *this;
    }

    friend constexpr Quantity operator+(Quantity lhs, Quantity rhs) noexcept
    {
        return Quantity(lhs.value_ + rhs.value_);
    }

    friend constexpr Quantity operator-(Quantity lhs, Quantity rhs) noexcept
    {
        return Quantity(lhs.value_ - rhs.value_);
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os,
                                                         Quantity q)
    {
        return os << q.value_;
    }

private:
    rep value_{0};
};

enum class OrderSide : std::uint8_t
{
    BUY,
    SELL
};

enum class OrderType : std::uint8_t
{
    MARKET,
    LIMIT,
    STOP_MARKET,
    STOP_LIMIT
};

[[nodiscard]] constexpr bool isStopOrder(OrderType type) noexcept
{
    return type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT;
}

[[nodiscard]] constexpr bool isLimitOrder(OrderType type) noexcept
{
    return type == OrderType::LIMIT || type == OrderType::STOP_LIMIT;
}

[[nodiscard]] constexpr bool isMarketOrder(OrderType type) noexcept
{
    return type == OrderType::MARKET || type == OrderType::STOP_MARKET;
}

} // namespace OrderBookUtils
