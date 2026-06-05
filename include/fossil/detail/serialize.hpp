#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <fossil/concepts.hpp>
#include <immintrin.h>
#include <span>
#include <vector>


// clang-format off
template<class T, auto N = sizeof(T)>
requires std::is_trivially_copyable_v<T> and (N == sizeof(T))
constexpr auto to(std::span<const std::byte, N> buffer) noexcept -> T
// clang-format on
{
    std::array<std::byte, N> array;
    std::copy(std::begin(buffer), std::end(buffer), std::begin(array));

    return std::bit_cast<T>(array);
}


template<class T, auto N = sizeof(T)>
constexpr auto from(T&& t) noexcept -> std::array<const std::byte, N>
{
    return std::bit_cast<std::array<const std::byte, N>>(t);
}

static_assert(
    std::is_same_v<decltype(from('A')), std::array<const std::byte, sizeof(decltype('A'))>>);
static_assert(std::is_same_v<decltype(from(1)), std::array<const std::byte, sizeof(decltype(1))>>);
static_assert(
    std::is_same_v<decltype(from(1ul)), std::array<const std::byte, sizeof(decltype(1ul))>>);
static_assert(
    std::is_same_v<decltype(from(1ull)), std::array<const std::byte, sizeof(decltype(1ull))>>);

template<auto N, class Span>
[[nodiscard]] constexpr auto head_span(const Span& span) noexcept
    -> std::span<typename Span::element_type, N>
{
    return std::span<typename Span::element_type, N>{span.begin(), N};
}


// clang-format off
template<range_of<std::byte>... Spans>
requires (sizeof...(Spans) > 1)
[[nodiscard]] constexpr auto concat(Spans... spans) noexcept -> std::vector<std::byte>
// clang-format on
{
    const auto size = (spans.size() + ...);

    std::vector<std::byte> vec;
    vec.reserve(size);

    (std::copy(std::begin(spans), std::end(spans), std::back_inserter(vec)), ...);

    return vec; // hope for nrvo
}

template<range_of<std::byte> Span>
[[nodiscard]] constexpr auto concat(Span span) noexcept -> Span
{
    return span;
}


template<Serializable T>
auto serialize(const std::vector<T>& v) noexcept -> std::vector<std::byte>;

template<Serializable T>
auto serialize(const std::optional<T>& v) noexcept -> std::vector<std::byte>;

template<class... Args>
auto serialize(const std::tuple<Args...>& tuple) noexcept;


template<CustomSerializable T>
constexpr auto serialize(const T& value) noexcept
{
    return T::serialize(value);
}

template<SimpleSerializable T>
constexpr auto serialize(const T& v) noexcept
{
    return from(v);
}

template<std::same_as<std::string> T>
auto serialize(const T& s) noexcept
{
    const std::uint64_t size = s.size();
    const auto size_ser = serialize(size);

    std::vector buffer(size_ser.begin(), size_ser.end());

    const auto buffer_size = buffer.size();
    const auto copy_size = sizeof(std::byte) * size;

    buffer.resize(buffer.size() + copy_size);
    std::memcpy(&buffer[buffer_size], s.data(), copy_size);


    return buffer;
}

template<Serializable T>
auto serialize(const std::vector<T>& v) noexcept -> std::vector<std::byte>
{
    const std::uint64_t size = v.size();
    const auto size_ser = serialize(size);
    std::vector vec(std::begin(size_ser), std::end(size_ser));

    if constexpr(SimpleSerializable<T>) {
        const auto write_size = size * sizeof(T);
        const auto vec_size = vec.size();
        vec.resize(vec_size + write_size);
        std::memcpy(&vec[vec_size], v.data(), write_size);
    } else {
        vec.reserve(size * sizeof(T) + size_ser.size());
        for(const auto& elem : v) {
            const auto ser = serialize(elem);
            const auto vec_size = std::size(vec);
            const auto write_size = ser.size() * sizeof(ser.front());

            vec.resize(write_size + vec_size);
            std::memcpy(&vec[vec_size], &ser[0], write_size);
        }
    }

    return vec;
}

template<Serializable T>
auto serialize(const std::optional<T>& v) noexcept -> std::vector<std::byte>
{
    if(not v.has_value()) {
        return std::vector{std::byte{0}};
    }

    const auto ser = serialize(v.value());
    std::vector buffer{std::byte{0xFF}};

    buffer.resize(1 + ser.size());
    std::memcpy(&buffer[1], ser.data(), ser.size());

    return buffer;
}

template<class... Args>
auto serialize(const std::tuple<Args...>& tuple) noexcept
{
    static_assert((Serializable<Args> && ...), "all types of a tuple need to be serializable");

    constexpr auto helper = [](const auto&... args) constexpr {
        return concat(serialize(args)...);
    };

    return std::apply(helper, tuple);
}

// clang-format off
template<Serializable... Args>
requires(sizeof...(Args) > 1)
auto serialize(Args... args) noexcept
{
    return concat(serialize(args)...);
}
// clang-format on


template<class T>
requires is_vector<T> and Serializable<typename T::value_type>
auto deserialize(std::span<const std::byte>& buffer) noexcept
    -> std::vector<typename T::value_type>;

template<class T>
requires is_optional<T> and Serializable<typename T::value_type>
auto deserialize(std::span<const std::byte>& buffer) noexcept
    -> std::optional<typename T::value_type>;

template<class T>
requires is_tuple<T>
auto deserialize(std::span<const std::byte>& buffer) noexcept
    -> std::optional<typename T::value_type>;

template<std::same_as<std::string> T>
auto deserialize(std::span<const std::byte>& buffer) noexcept -> std::string;

// clang-format off
template<class... Args>
requires(sizeof...(Args) > 1)
auto deserialize(std::span<const std::byte>& buffer) noexcept
  -> std::tuple<Args...>;
// clang-format on

template<CustomSerializable T>
constexpr auto deserialize(std::span<const std::byte>& buffer) noexcept
{
    return T::deserialize(buffer);
}

template<SimpleSerializable T>
constexpr auto deserialize(std::span<const std::byte>& buffer) noexcept
{
    const auto value_span = head_span<sizeof(T)>(buffer);
    auto value = to<T>(value_span);

    buffer = buffer.subspan(sizeof(T));
    return value;
}

template<class T>
requires is_vector<T> and Serializable<typename T::value_type>
auto deserialize(std::span<const std::byte>& buffer) noexcept -> std::vector<typename T::value_type>
{
    const auto size_span = head_span<sizeof(std::uint64_t)>(buffer);
    const auto size = to<std::uint64_t>(size_span);

    // cut of leading size
    buffer = buffer.subspan(sizeof(std::uint64_t));

    std::vector<typename T::value_type> elements;
    elements.resize(size);

    for(std::uint64_t i = 0; i < size; i++) {
        elements[i] = deserialize<typename T::value_type>(buffer);
    }

    return elements;
}

template<class T>
requires is_optional<T> and Serializable<typename T::value_type>
auto deserialize(std::span<const std::byte>& buffer) noexcept
    -> std::optional<typename T::value_type>
{
    const auto flag = buffer[0];
    // cut of the flag
    buffer = buffer.subspan(1);

    if(flag == std::byte{0}) {
        return std::nullopt;
    }

    return deserialize<typename T::value_type>(buffer);
}

template<std::same_as<std::string> T>
auto deserialize(std::span<const std::byte>& buffer) noexcept -> std::string
{
    const auto size_span = head_span<sizeof(std::uint64_t)>(buffer);
    const auto size = to<std::uint64_t>(size_span);
    const char* data = reinterpret_cast<const char*>(buffer.data() + sizeof(std::uint64_t));

    std::string str(data, size);

    buffer = buffer.subspan(size + sizeof(std::uint64_t));

    return str;
}


namespace impl {

template<typename Tuple>
struct TupleHelper;

template<typename... Args>
struct TupleHelper<std::tuple<Args...>>
{
    static auto deser(std::span<const std::byte>& buffer) -> std::tuple<Args...>
    {
        return std::tuple{deserialize<Args>(buffer)...};
    }
};

} // namespace impl


template<class T>
requires is_tuple<T>
auto deserialize(std::span<const std::byte>& buffer) noexcept -> T
{
    return impl::TupleHelper<T>::deser(buffer);
}

// clang-format off
template<class... Args>
requires(sizeof...(Args) > 1)
 auto deserialize(std::span<const std::byte>& buffer) noexcept
{
    return std::tuple{deserialize<Args>(buffer)...};
}
// clang-format on
