#pragma once


#include <array>
#include <cstddef>
#include <fossil/detail/serialize.hpp>
#include <fossil/detail/tx_scope.hpp>
#include <fossil/is_resizable_object.hpp>
#include <fossil/repository.hpp>
#include <span>
#include <sys/types.h>
#include <utility>
#include <variant>
#include <vector>


namespace fossil::detail {

class pvec_log
{
public:
    struct swap_op
    {
        std::size_t i;
        std::size_t j;
    };
    struct resize_op
    {
        std::size_t size;
    };
    struct dirty
    {
        std::size_t index;
    };

    using log_op = std::variant<swap_op, resize_op, dirty>;

    pvec_log() = default;
    pvec_log(const pvec_log&) = delete;
    pvec_log(pvec_log&&) = default;
    auto operator=(const pvec_log&) -> pvec_log& = delete;
    auto operator=(pvec_log&&) -> pvec_log& = default;

    void resize(std::size_t size) { log_.emplace_back(resize_op{size}); }
    void swap(std::size_t i, std::size_t j) { log_.emplace_back(swap_op{.i = i, .j = j}); }

    void modified(std::size_t index) { log_.emplace_back(dirty{index}); }

    void clear() noexcept
    {
        log_.clear();
        serialized_log_.clear();
    }

    auto get_log() const -> const std::vector<std::byte>& { return serialized_log_; }


private:
    template<typename T>
    friend class pvec;

    std::vector<log_op> log_;
    std::vector<std::byte> serialized_log_;
};


template<typename T>
class pvec
{
public:
    using log_type = detail::pvec_log;
    using value_type = T;

private:
public:
    constexpr pvec() noexcept { static_assert(is_resizable_object<pvec>); }

    pvec(const pvec&) = delete;
    pvec(pvec&&) = default;
    auto operator=(const pvec&) -> pvec& = delete;
    auto operator=(pvec&&) -> pvec& = default;

    // LOGGED MEMBERS

    auto operator[](std::size_t index) -> value_type&
    {
        log_.modified(index);
        return cache_[index];
    }
    auto front() -> value_type&
    {
        log_.modified(0);
        return cache_[0];
    }
    auto back() -> value_type&
    {
        log_.modified(size() - 1);
        return cache_[size() - 1];
    }

    void clear() { log_.resize(0); }

    template<typename TT>
    void push_back(TT&& value)
    {
        log_.resize(size() + 1);
        log_.modified(size());

        cache_.push_back(value);
    }

    template<typename... Args>
    void emplace_back(Args&&... args)
    {
        log_.resize(size() + 1);
        log_.modified(size());

        cache_.emplace_back(std::forward<Args>(args)...);
    }

    void pop_back()
    {
        log_.resize(size() - 1);
        cache_.pop_back();
    }

    void swap(std::size_t i, std::size_t j)
    {
        log_.swap(i, j);
        std::swap(cache_[i], cache_[j]);
    }


    // UNLOGGED MEMBERS

    auto operator[](std::size_t index) const -> const value_type& { return cache_[index]; }
    auto front() const -> const value_type& { return cache_.front(); }
    auto back() const -> const value_type& { return cache_.back(); }

    auto size() const -> std::size_t { return cache_.size(); }
    auto empty() const -> std::size_t { return cache_.empty(); }


    // serializer
    auto serialize() const noexcept -> std::vector<std::byte>
    {
        // FIXME, a conditional commit must not delete the old version from PMEM until the condition
        // becomes valid
        // TODO: is this a problem? or can Concurrency Control manage this outside?


        // I don't think we need anything here..
        return {};
    }

    auto current_log() -> const std::vector<std::byte>&
    {
        log_.serialized_log_.clear();
        for(const auto& entry : log_.log_) {
            auto arr = serialize_op(entry);
            auto span = std::span(arr);

            auto range = std::as_bytes(span);
            log_.serialized_log_.insert(log_.serialized_log_.end(), range.begin(), range.end());
        }

        return log_.get_log();
    }

    auto clean_log() -> void
    {
        log_.clear();
    }

    static pvec recover(std::span<const std::byte> _, std::span<const std::byte> log)
    {
        std::vector<T> something;

        while(not log.empty()) {
            deserialize_op(something, log);
        }

        return pvec{std::move(something)};
    }
    // auto to_log() const noexcept -> std::vector<std::byte>
    // {
    //     using std::begin, std::end;
    //     std::vector<std::byte> log = serialize_op(pvec_log::resize_op{cache_.size()});

    //     for(std::size_t i = 0; i < cache_.size(); ++i) {
    //         auto arr = serialize_op(pvec_log::dirty{i});
    //         auto span = std::span(arr);

    //         auto range = std::as_bytes(span);
    //         log.insert(log.end(), range.begin(), range.end());
    //     }
    //     return log;
    // }


private:
    explicit pvec(std::vector<T>&& other) : cache_(other) {}

    static auto deserialize_op(std::vector<T>& self, std::span<const std::byte>& log)
    {
        if(log.empty()) {
            return;
        }

        std::byte opcode = ::deserialize<std::byte>(log);

        if(opcode == std::byte{0}) {
            // swap operation
            std::size_t i = ::deserialize<std::size_t>(log);
            std::size_t j = ::deserialize<std::size_t>(log);

            auto tmp = self[i];
            self[i] = self[j];
            self[j] = tmp;
        } else if(opcode == std::byte{1}) {
            // resize operation
            std::size_t size = ::deserialize<std::size_t>(log);

            self.resize(size);
        } else if(opcode == std::byte{2}) {
            // modification
            std::size_t i = ::deserialize<std::size_t>(log);
            T v = ::deserialize<T>(log);

            self[i] = v;
        } else {
            std::unreachable();
        }
    }

    auto serialize_op(const typename log_type::log_op& entry) const -> std::vector<std::byte>
    {
        using std::begin, std::end;

        if(std::holds_alternative<pvec_log::swap_op>(entry)) {
            auto e = std::get<pvec_log::swap_op>(entry);

            std::vector out = {std::byte{0}};
            std::array idx0 = ::serialize(e.i);
            std::array idx1 = ::serialize(e.j);

            return concat(out, idx0, idx1);
        }
        if(std::holds_alternative<pvec_log::resize_op>(entry)) {
            auto e = std::get<pvec_log::resize_op>(entry);

            std::vector out = {std::byte{1}};
            std::array size = ::serialize(e.size);

            return concat(out, size);
        }
        if(std::holds_alternative<pvec_log::dirty>(entry)) {
            auto e = std::get<pvec_log::dirty>(entry);
            const auto& element = cache_[e.index];

            std::vector out = {std::byte{2}};

            std::array index = ::serialize(e.index);
            auto serialized_element = ::serialize(element);

            return concat(out, index, serialized_element);
        }

        std::unreachable();
    }

private:
    std::vector<T> cache_;
    log_type log_;
    // std::optional<log_reference<log_type>> log_ref_;
};
} // namespace fossil::detail
