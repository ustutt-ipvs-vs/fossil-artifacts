#pragma once

#include <cstddef>
#include <fossil/detail/serialize.hpp>
#include <fossil/detail/tx_scope.hpp>
#include <fossil/is_resizable_object.hpp>
#include <map>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace fossil::detail {

template<class K>
class pRBTree_log
{
public:
    struct dirty
    {
        K key;
    };

    struct reset_op
    {
    };

    struct delete_op
    {
        K key;
    };

    using log_op = std::variant<dirty, reset_op, delete_op>;

    pRBTree_log() = default;
    pRBTree_log(const pRBTree_log&) = delete;
    pRBTree_log(pRBTree_log&&) = default;
    auto operator=(const pRBTree_log&) -> pRBTree_log& = delete;
    auto operator=(pRBTree_log&&) -> pRBTree_log& = default;

    void remove(const K& key) { log_.emplace_back(delete_op{key}); }
    void modified(const K& key) { log_.emplace_back(dirty{key}); }
    void clean() { log_.emplace_back(reset_op{}); }

    void clear() noexcept
    {
        log_.clear();
        serialized_log_.clear();
    }

    auto get_log() const -> const std::vector<std::byte>& { return serialized_log_; }

private:
    template<typename T, class V>
    friend class pRBTree;

    std::vector<log_op> log_;
    std::vector<std::byte> serialized_log_;
};

template<class K, class V>
class pRBTree
{
public:
    using log_type = detail::pRBTree_log<K>;
    using mapped_type = V;
    using key_type = K;

    constexpr pRBTree() noexcept { static_assert(is_resizable_object<pRBTree>); }

    pRBTree(const pRBTree&) = delete;
    pRBTree(pRBTree&&) = default;
    auto operator=(const pRBTree&) -> pRBTree& = delete;
    auto operator=(pRBTree&&) -> pRBTree& = default;

    void clear()
    {
        cache_.clear();
        log_.clean();
    }

    void put(K key, V value)
    {
        auto [it, _] = cache_.insert_or_assign(std::move(key), std::move(value));
        log_.modified(it->first);
    }

    auto get(const K& key) -> mapped_type*
    {
        auto it = cache_.find(key);
        if(it == cache_.end()) {
            return nullptr;
        }

        log_.modified(it->first);
        return &it->second;
    }

    void remove(const K& key)
    {
        auto it = cache_.find(key);
        if(it == cache_.end()) {
            return;
        }

        log_.remove(it->first);
        cache_.erase(it);
    }

    auto size() const -> std::size_t { return cache_.size(); }
    auto empty() const -> std::size_t { return cache_.empty(); }

    auto get(const K& key) const -> const mapped_type*
    {
        auto it = cache_.find(key);
        if(it == cache_.end()) {
            return nullptr;
        }

        return &it->second;
    }

    auto contains(const K& key) const -> bool { return get(key) != nullptr; }

    auto serialize() const noexcept -> std::vector<std::byte> { return {}; }

    auto current_log() -> const std::vector<std::byte>&
    {
        log_.serialized_log_.clear();
        for(const auto& entry : log_.log_) {
            auto bytes = serialize_op(entry);
            auto span = std::span(bytes);
            auto range = std::as_bytes(span);
            log_.serialized_log_.insert(log_.serialized_log_.end(), range.begin(), range.end());
        }

        return log_.get_log();
    }

    auto clean_log() -> void { log_.clear(); }

    static auto recover(std::span<const std::byte> /* state */, std::span<const std::byte> log)
        -> pRBTree
    {
        cache_type cache;

        while(not log.empty()) {
            deserialize_op(cache, log);
        }

        return pRBTree{std::move(cache)};
    }

private:
    using cache_type = std::map<K, V>;

    explicit pRBTree(cache_type&& other) : cache_(std::move(other)) {}

    static void deserialize_op(cache_type& self, std::span<const std::byte>& log)
    {
        if(log.empty()) {
            return;
        }

        const auto opcode = ::deserialize<std::byte>(log);

        if(opcode == std::byte{0}) {
            K key = ::deserialize<K>(log);
            V value = ::deserialize<V>(log);
            self.insert_or_assign(std::move(key), std::move(value));
        } else if(opcode == std::byte{1}) {
            K key = ::deserialize<K>(log);
            self.erase(key);
        } else if(opcode == std::byte{2}) {
            self.clear();
        } else {
            std::unreachable();
        }
    }

    auto serialize_op(const typename log_type::log_op& entry) const -> std::vector<std::byte>
    {
        if(std::holds_alternative<typename log_type::dirty>(entry)) {
            const auto& e = std::get<typename log_type::dirty>(entry);
            auto it = cache_.find(e.key);
            if(it == cache_.end()) {
                return {};
            }

            std::vector out = {std::byte{0}};
            auto serialized_key = ::serialize(e.key);
            auto serialized_value = ::serialize(it->second);

            return concat(out, serialized_key, serialized_value);
        }
        if(std::holds_alternative<typename log_type::delete_op>(entry)) {
            const auto& e = std::get<typename log_type::delete_op>(entry);

            std::vector out = {std::byte{1}};
            auto serialized_key = ::serialize(e.key);

            return concat(out, serialized_key);
        }
        if(std::holds_alternative<typename log_type::reset_op>(entry)) {
            return std::vector{std::byte{2}};
        }

        std::unreachable();
    }

    cache_type cache_;
    log_type log_;
};

} // namespace fossil::detail
