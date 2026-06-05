#pragma once

#include <atomic>
#include <fossil/concepts.hpp>
#include <fossil/detail/pmem.hpp>
#include <fossil/detail/rw_spin_lock.hpp>
#include <fossil/detail/sharded_counter.hpp>
#include <fossil/detail/tx_scope.hpp>
#include <fossil/detail/unordered.hpp>
#include <fossil/is_resizable_object.hpp>
#include <fossil/reference.hpp>
#include <fossil/transaction_header.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace fossil {

template<std::size_t SHARDS>
class sharded_repository
{
    inline static thread_local std::optional<std::uint64_t> this_thread_transaction = std::nullopt;
    inline static thread_local bool this_thread_has_nested_write = false;
    inline static thread_local std::optional<std::uint64_t> this_thread_idx = std::nullopt;
    inline static thread_local std::vector<std::pair<std::uint64_t, bool>> this_thread_to_unlock{};

public:
    sharded_repository() : pmems_(make_pmems()), next_tx_id_(recover()) {}

    template<typename T, typename Tx>
    auto single_read_transaction(reference<T>& reference, Tx&& closure)
        -> std::invoke_result_t<Tx, const T&>
    {
        using return_t = std::invoke_result_t<Tx, const T&>;

        const auto shard = shard_of(reference);
        auto& lock = object_locks_[shard];
        lock.lock_shared();

        auto* object = ensure_object_in_cache(reference, shard);
        assert(object != nullptr && "Cannot access unknown object");

        if constexpr(std::is_void_v<return_t>) {
            closure(*object);
            lock.unlock_shared();
        } else {
            auto result = closure(*object);
            lock.unlock_shared();
            return result;
        }
    }

    template<typename T, typename Tx>
    auto single_write_transaction(reference<T>& reference, Tx&& closure)
        -> std::invoke_result_t<Tx, T&>
    {
        using return_t = std::invoke_result_t<Tx, T&>;

        const auto shard = shard_of(reference);
        auto& lock = object_locks_[shard];
        lock.lock();

        auto* object = ensure_object_in_cache(reference, shard);
        assert(object != nullptr && "Cannot access unknown object");

        const auto tx_id = next_tx_id_.increment(reference.id);

        if constexpr(std::is_void_v<return_t>) {
            closure(*object);
            persist_single_write(reference, shard, *object, tx_id);
            lock.unlock();
        } else {
            auto result = closure(*object);
            persist_single_write(reference, shard, *object, tx_id);
            lock.unlock();
            return result;
        }
    }

    // resolves a transaction's modified object to a reference in volatile memory
    template<typename T>
    auto read_access(tx_scope<T>& transaction) -> const T&
    {
        lock_shard(transaction, true);

        ensure_object_in_cache(transaction);
        assert(transaction.object_ptr != nullptr && "Cannot access unknown object");

        return *transaction.object_ptr;
    }

    template<typename T>
    auto write_access(tx_scope<T>& transaction) -> T&
    {
        lock_shard(transaction, false);

        ensure_object_in_cache(transaction);
        assert(transaction.object_ptr != nullptr && "Cannot access unknown object");

        return *transaction.object_ptr;
    }

    template<typename T, typename... Args>
    auto create(Args&&... args) -> reference<T>
    {
        auto reference = new_reference<T>();

        auto scope = create_scope(reference, true);

        create_impl(scope, reference, std::forward<Args>(args)...);

        if(not scope.is_nested()) {
            this_thread_transaction.reset();
        }

        return std::move(reference);
    }

    template<typename T, typename O, typename... Args>
    auto create(tx_scope<O> outer_scope, Args&&... args) -> reference<T>
    {
        auto reference = new_reference<T>();

        // TODO: i think this method is missing?
        auto scope = create_scope(reference, outer_scope, true);

        create_impl(scope, reference, std::forward<Args>(args)...);

        if(not scope.is_nested()) {
            this_thread_transaction.reset();
        }

        return reference;
    }

    template<typename T>
    auto create_scope(reference<T> reference, bool is_write) -> tx_scope<T>
    {
        const auto existing_tx = this_thread_transaction;
        const auto tx_id = (is_write || existing_tx.has_value())
            ? next_tx_id_.increment(reference.id)
            : detail::transaction_header::NO_CONDITON_TX;

        auto scope = tx_scope<T>{.tx_id = tx_id,
                                 .object = reference,
                                 .shard = shard_of(reference),
                                 .outer_tx = existing_tx};

        if(not existing_tx.has_value()) {
            this_thread_transaction = scope.tx_id;
        } else if(is_write) {
            this_thread_has_nested_write = true;
        }

        return scope;
    }

    template<typename T>
    void commit_transaction(tx_scope<T> scope, bool is_read_only)
    {
        if(is_read_only and not this_thread_has_nested_write) {
            unlock_object(scope, is_read_only);

            if(not scope.is_nested()) {
                this_thread_transaction.reset();
            }

            return;
        }

        assert(scope.object_ptr != nullptr && "Cannot commit unknown object");
        const auto& proxy = *scope.object_ptr;

        detail::transaction_header header{.tx_id = scope.tx_id,
                                          .condition_tx_id = scope.outer_tx.value_or(
                                              detail::transaction_header::NO_CONDITON_TX)};
        auto header_ser = header.serialize();

        auto& pmem = pmems_[scope.shard];

        auto serialized_constant_size = proxy.serialize();
        if(serialized_constant_size.empty()) {
            pmem.update(std::span<const std::byte>(header_ser), scope.object);
        } else {
            std::vector<std::byte> update;
            update.reserve(header_ser.size() + serialized_constant_size.size());
            update.insert(update.end(), header_ser.begin(), header_ser.end());
            update.insert(update.end(),
                          serialized_constant_size.begin(),
                          serialized_constant_size.end());

            pmem.update(update, scope.object);
        }

        // free old allocations, if available
        if(not scope.is_nested()) {
            pmem.free_old_versions(scope.object);
        }

        unlock_object(scope, is_read_only);

        if(not scope.is_nested()) {
            this_thread_transaction.reset();
            this_thread_to_unlock.clear();
        }
    }


    template<is_resizable_object T>
    void commit_transaction(tx_scope<T> scope, bool is_read_only)
    {
        if(is_read_only and not this_thread_has_nested_write) {
            unlock_object(scope, is_read_only);

            if(not scope.is_nested()) {
                this_thread_transaction.reset();
            }

            return;
        }

        // NOTE: we need write access, because we reset the object's current log
        assert(scope.object_ptr != nullptr && "Cannot commit unknown object");
        auto& proxy = *scope.object_ptr;

        detail::transaction_header header{.tx_id = scope.tx_id,
                                          .condition_tx_id = scope.outer_tx.value_or(
                                              detail::transaction_header::NO_CONDITON_TX)};
        auto header_ser = header.serialize();
        auto serialized_constant_size = proxy.serialize();
        auto& pmem = pmems_[scope.shard];

        if(is_read_only) {
            if(serialized_constant_size.empty()) {
                pmem.update(std::span<const std::byte>(header_ser), {}, scope.object);
            } else {
                std::vector<std::byte> update;
                update.reserve(header_ser.size() + serialized_constant_size.size());
                update.insert(update.end(), header_ser.begin(), header_ser.end());
                update.insert(update.end(),
                              serialized_constant_size.begin(),
                              serialized_constant_size.end());
                pmem.update(update, {}, scope.object);
            }
        } else {
            const auto& log = proxy.current_log();
            if(serialized_constant_size.empty()) {
                pmem.update(std::span<const std::byte>(header_ser), log, scope.object);
            } else {
                std::vector<std::byte> update;
                update.reserve(header_ser.size() + serialized_constant_size.size());
                update.insert(update.end(), header_ser.begin(), header_ser.end());
                update.insert(update.end(),
                              serialized_constant_size.begin(),
                              serialized_constant_size.end());
                pmem.update(update, log, scope.object);
            }
            proxy.clean_log();
        }

        // free old allocations, if available
        if(not scope.is_nested()) {
            pmem.free_old_versions(scope.object);
        }

        unlock_object(scope, is_read_only);

        if(not scope.is_nested()) {
            this_thread_transaction.reset();
            this_thread_to_unlock.clear();
            this_thread_has_nested_write = false;
        }
    }

private:
    template<typename T>
    void persist_single_write(reference<T> reference,
                              std::uint64_t shard,
                              const T& proxy,
                              std::uint64_t tx_id)
    {
        detail::transaction_header header{
            .tx_id = tx_id,
            .condition_tx_id = detail::transaction_header::NO_CONDITON_TX};
        auto header_ser = header.serialize();
        auto serialized_constant_size = proxy.serialize();
        auto& pmem = pmems_[shard];

        if(serialized_constant_size.empty()) {
            pmem.update(std::span<const std::byte>(header_ser), reference);
        } else {
            std::vector<std::byte> update;
            update.reserve(header_ser.size() + serialized_constant_size.size());
            update.insert(update.end(), header_ser.begin(), header_ser.end());
            update.insert(update.end(),
                          serialized_constant_size.begin(),
                          serialized_constant_size.end());
            pmem.update(update, reference);
        }

        pmem.free_old_versions(reference);
    }

    template<is_resizable_object T>
    void persist_single_write(reference<T> reference,
                              std::uint64_t shard,
                              T& proxy,
                              std::uint64_t tx_id)
    {
        detail::transaction_header header{
            .tx_id = tx_id,
            .condition_tx_id = detail::transaction_header::NO_CONDITON_TX};
        auto header_ser = header.serialize();
        auto serialized_constant_size = proxy.serialize();
        const auto& log = proxy.current_log();
        auto& pmem = pmems_[shard];

        if(serialized_constant_size.empty()) {
            pmem.update(std::span<const std::byte>(header_ser), log, reference);
        } else {
            std::vector<std::byte> update;
            update.reserve(header_ser.size() + serialized_constant_size.size());
            update.insert(update.end(), header_ser.begin(), header_ser.end());
            update.insert(update.end(),
                          serialized_constant_size.begin(),
                          serialized_constant_size.end());
            pmem.update(update, log, reference);
        }

        proxy.clean_log();

        pmem.free_old_versions(reference);
    }

    template<typename T>
    void lock_shard(tx_scope<T> scope, bool is_read_only)
    {
        auto& lock = object_locks_[scope.shard];

        if(is_read_only) {
            lock.lock_shared();
        } else {
            lock.lock();
        }
    }
    template<typename T>
    void unlock_object(tx_scope<T> scope, bool is_read_only)
    {
        if(scope.is_nested()) {
            this_thread_to_unlock.emplace_back(scope.object.id, is_read_only);
            return;
        }

        for(auto [id, read_only] : this_thread_to_unlock) {
            auto& lock = object_locks_[shard_of(id)];

            if(read_only) {
                lock.unlock_shared();
            } else {
                lock.unlock();
            }
        }

        auto& lock = object_locks_[scope.shard];

        if(is_read_only) {
            lock.unlock_shared();
        } else {
            lock.unlock();
        }
    }

    template<typename T, typename... Args>
    void create_impl(tx_scope<T> scope, reference<T> reference, Args&&... args)
    {
        auto obj = std::make_unique<T>(std::forward<Args>(args)...);

        auto pair = cache_[scope.shard].emplace(reference.id, std::move(obj));
        T& proxy = *reinterpret_cast<T*>((*pair.first).second.get());
        scope.object_ptr = &proxy;

        auto& pmem = pmems_[scope.shard];

        if constexpr(is_resizable_object<T>) {
            detail::transaction_header header{.tx_id = scope.tx_id,
                                              .condition_tx_id = scope.outer_tx.value_or(
                                                  detail::transaction_header::NO_CONDITON_TX)};
            auto header_ser = header.serialize();
            auto serialized_constant_size = proxy.serialize();
            const auto& log = proxy.current_log();
            if(serialized_constant_size.empty()) {
                pmem.emplace(std::span<const std::byte>(header_ser), log, reference);
            } else {
                std::vector<std::byte> update;
                update.reserve(header_ser.size() + serialized_constant_size.size());
                update.insert(update.end(), header_ser.begin(), header_ser.end());
                update.insert(update.end(),
                              serialized_constant_size.begin(),
                              serialized_constant_size.end());

                pmem.emplace(update, log, reference);
            }

            proxy.clean_log();

        } else {
            detail::transaction_header header{.tx_id = scope.tx_id,
                                              .condition_tx_id = scope.outer_tx.value_or(
                                                  detail::transaction_header::NO_CONDITON_TX)};
            auto header_ser = header.serialize();
            std::vector<std::byte> update(header_ser.begin(), header_ser.end());

            auto serialized_constant_size = proxy.serialize();
            update.insert(update.end(),
                          serialized_constant_size.begin(),
                          serialized_constant_size.end());

            pmem.emplace(serialized_constant_size, reference);
        }
    }

    template<typename T>
    void ensure_object_in_cache(tx_scope<T>& transaction)
    {
        transaction.object_ptr = ensure_object_in_cache(transaction.object, transaction.shard);
    }

    template<typename T>
    auto ensure_object_in_cache(reference<T>& reference, std::uint64_t shard)
        -> std::remove_const_t<T>*
    {
        if(reference.raw.load(std::memory_order_relaxed) != nullptr) {
            return reference.raw.load(std::memory_order_relaxed);
        }
        // std::println("not good");

        auto& cache = cache_[shard];
        auto it = cache.find(reference.id);

        if(it == cache.end()) {
            auto& pmem = pmems_[shard];
            auto span = pmem.read(reference);

            if constexpr(is_resizable_object<T>) {
                std::span<const std::byte> log = pmem.log_of(reference);

                T v = T::recover(span, log);
                it = cache.emplace(reference.id, std::make_unique<T>(std::move(v))).first;
            } else {
                T v = ::deserialize<T>(span);
                it = cache.emplace(reference.id, std::make_unique<T>(std::move(v))).first;
            }
        }

        reference.raw.store(reinterpret_cast<std::remove_const_t<T>*>(it->second.get()),
                            std::memory_order_relaxed);
        return reference.raw;
    }

private:
    template<class T>
    constexpr auto shard_of(const reference<T>& ref) -> std::uint64_t
    {
        return shard_of(ref.id);
    }

    constexpr auto shard_of(std::uint64_t id) -> std::uint64_t { return id % SHARDS; }

    template<class T>
    constexpr auto new_reference() -> reference<T>
    {
        return reference<T>{next_tx_id_.increment(0)};
    }

    template<std::size_t... Is>
    constexpr auto make_pmems_impl(std::index_sequence<Is...>)
    {
        return std::array{detail::pmem{Is}...};
    }

    constexpr auto make_pmems() { return make_pmems_impl(std::make_index_sequence<SHARDS>{}); }

    auto recover()
    {
        auto l = [this]<std::size_t... Is>(std::index_sequence<Is...> s) {
            return std::max({pmems_[Is].recover()...});
        };
        return sharded_counter<SHARDS>{l(std::make_index_sequence<SHARDS>())};
    }

private:
    using obj_id = std::uint64_t;
    using tx_id = std::uint64_t;

    std::array<detail::rw_spin_lock, SHARDS> object_locks_;
    std::array<detail::pmem, SHARDS> pmems_;
    std::array<detail::unordered_map<obj_id, std::shared_ptr<void>>, SHARDS> cache_;


    sharded_counter<SHARDS> next_tx_id_;
};

using repository = sharded_repository<64>;

constexpr static auto object_repo() -> repository&
{
    static repository repo;
    return (repo);
}

} // namespace fossil
