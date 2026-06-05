#pragma once

#include <fossil/detail/tx_scope.hpp>
#include <fossil/reference.hpp>
#include <fossil/repository.hpp>
#include <fossil/transaction_traits.hpp>
#include <type_traits>

namespace fossil {

template<typename T, typename Tx>
constexpr static auto transaction(reference<T>& reference, Tx&& closure)
    -> std::invoke_result_t<Tx, std::remove_const_t<T>&>
{
    using return_t = std::invoke_result_t<Tx, std::decay_t<T>&>;

    repository& repo = object_repo();

    // TODO: transaction coordination

    if constexpr(transaction_traits<Tx>::is_read_only) {
        auto scope = repo.create_scope(reference, false);

        // resolve object reference to cache
        const auto& object = repo.read_access(scope);

        // run the transaction
        if constexpr(std::is_void_v<return_t>) {
            closure(object);

            // commit and return result
            repo.commit_transaction(scope, true);
        } else {
            auto result = closure(object);

            // commit and return result
            repo.commit_transaction(scope, true);
            return result;
        }
    } else {
        auto scope = repo.create_scope(reference, true);
        // resolve object reference to cache
        auto& object = repo.write_access(scope);

        // run the transaction
        if constexpr(std::is_void_v<return_t>) {
            closure(object);

            // commit and return result
            repo.commit_transaction(scope, false);
        } else {
            auto result = closure(object);

            // commit and return result
            repo.commit_transaction(scope, false);
            return result;
        }
    }
}

} // namespace fossil
