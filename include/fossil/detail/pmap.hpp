#pragma once

#include <cstddef>
#include <cstdint>
#include <fossil/detail/serialize.hpp>
#include <fossil/detail/unordered.hpp>
#include <fossil/is_resizable_object.hpp>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fossil::detail {

class pmap_log
{
public:
    pmap_log() = default;
    pmap_log(const pmap_log&) = delete;
    pmap_log(pmap_log&&) = default;
    auto operator=(const pmap_log&) -> pmap_log& = delete;
    auto operator=(pmap_log&&) -> pmap_log& = default;

    template<typename K, typename V>
    void put(const K& key, const V& value)
    {
        log_.reserve(log_.size() + 1 + serialized_size_hint(key) + serialized_size_hint(value));
        log_.push_back(std::byte{4});
        append_serialized_value(log_, key);
        append_serialized_value(log_, value);
    }

    template<typename K>
    void remove(const K& key)
    {
        log_.reserve(log_.size() + 1 + serialized_size_hint(key));
        log_.push_back(std::byte{5});
        append_serialized_value(log_, key);
    }

    void clean() { log_.push_back(std::byte{3}); }

    auto clear() { log_.clear(); }
    auto get_log() const -> const std::vector<std::byte>& { return log_; }

private:
    template<typename T>
    static void append_serialized_value(std::vector<std::byte>& out, const T& value)
    {
        using value_type = std::remove_cvref_t<T>;

        if constexpr(SimpleSerializable<value_type>) {
            append_bytes(out, &value, sizeof(value_type));
        } else if constexpr(std::same_as<value_type, std::string>) {
            const std::uint64_t size = value.size();
            append_serialized_value(out, size);
            append_bytes(out, value.data(), value.size());
        } else {
            const auto serialized = ::serialize(value);
            append_bytes(out, serialized.data(), serialized.size());
        }
    }

    template<typename T>
    static auto serialized_size_hint(const T& value) -> std::size_t
    {
        using value_type = std::remove_cvref_t<T>;

        if constexpr(SimpleSerializable<value_type>) {
            return sizeof(value_type);
        } else if constexpr(std::same_as<value_type, std::string>) {
            return sizeof(std::uint64_t) + value.size();
        } else {
            return 0;
        }
    }

    static void append_bytes(std::vector<std::byte>& out, const void* data, std::size_t size)
    {
        if(size == 0) {
            return;
        }

        const auto* first = static_cast<const std::byte*>(data);
        out.insert(out.end(), first, first + size);
    }

private:
    std::vector<std::byte> log_;
};


template<class K, class V>
class pmap
{
public:
    using log_type = detail::pmap_log;
    using mapped_type = V;
    using key_type = K;

private:
    constexpr static std::size_t INITIAL_SIZE = 1 << 20;
    using cache_type = detail::unordered_map<K, V, std::hash<K>>;

public:
    pmap()
    {
        static_assert(is_resizable_object<pmap>);
        cache_.reserve(INITIAL_SIZE);
    }

    explicit pmap(std::size_t expected_entries)
    {
        static_assert(is_resizable_object<pmap>);
        cache_.reserve(expected_entries);
    }

    pmap(const pmap&) = delete;
    pmap(pmap&&) = default;
    auto operator=(const pmap&) -> pmap& = delete;
    auto operator=(pmap&&) -> pmap& = default;

    void clear()
    {
        cache_.clear();
        log_.clean();
    }

    void put(K key, V value)
    {
        auto [it, _] = cache_.insert_or_assign(std::move(key), std::move(value));
        log_.put(it->first, it->second);
    }

    void remove(const K& key)
    {
        if(auto it = cache_.find(key); it != cache_.end()) {
            log_.remove(it->first);
            cache_.erase(it);
        }
    }

    auto size() const -> std::size_t { return cache_.size(); }
    auto empty() const -> std::size_t { return cache_.empty(); }

    auto get(const K& key) const -> const mapped_type*
    {
        if(auto it = cache_.find(key); it != cache_.end()) {
            return &it->second;
        }

        return nullptr;
    }

    auto contains(const K& key) const -> bool { return get(key) != nullptr; }

    void reserve(std::size_t expected_entries) { cache_.reserve(expected_entries); }

    auto serialize() const noexcept -> std::vector<std::byte> { return {}; }

    auto current_log() -> const std::vector<std::byte>& { return log_.get_log(); }
    auto clean_log() -> void { log_.clear(); }

    static auto recover(std::span<const std::byte> /* state */, std::span<const std::byte> log)
        -> pmap
    {
        cache_type cache;
        std::vector<std::optional<K>> legacy_slots;

        while(not log.empty()) {
            deserialize_op(cache, legacy_slots, log);
        }

        return pmap{std::move(cache)};
    }

private:
    explicit pmap(cache_type&& cache) : cache_(std::move(cache)) {}

    static void deserialize_op(cache_type& self,
                               std::vector<std::optional<K>>& legacy_slots,
                               std::span<const std::byte>& log)
    {
        if(log.empty()) {
            return;
        }

        const auto opcode = ::deserialize<std::byte>(log);

        if(opcode == std::byte{0}) {
            const auto new_size = ::deserialize<std::size_t>(log);
            self.clear();
            legacy_slots.clear();
            legacy_slots.resize(new_size);
        } else if(opcode == std::byte{1}) {
            const auto index = ::deserialize<std::size_t>(log);
            if(index < legacy_slots.size() && legacy_slots[index].has_value()) {
                self.erase(*legacy_slots[index]);
                legacy_slots[index].reset();
            }
        } else if(opcode == std::byte{2}) {
            const auto index = ::deserialize<std::size_t>(log);
            K key = ::deserialize<K>(log);
            V value = ::deserialize<V>(log);

            if(index >= legacy_slots.size()) {
                legacy_slots.resize(index + 1);
            }
            if(legacy_slots[index].has_value() && *legacy_slots[index] != key) {
                self.erase(*legacy_slots[index]);
            }
            legacy_slots[index] = key;
            self.insert_or_assign(std::move(key), std::move(value));
        } else if(opcode == std::byte{3}) {
            self.clear();
            legacy_slots.clear();
        } else if(opcode == std::byte{4}) {
            K key = ::deserialize<K>(log);
            V value = ::deserialize<V>(log);
            self.insert_or_assign(std::move(key), std::move(value));
        } else if(opcode == std::byte{5}) {
            K key = ::deserialize<K>(log);
            self.erase(key);
        } else {
            std::unreachable();
        }
    }

private:
    cache_type cache_;
    log_type log_;
};

} // namespace fossil::detail
