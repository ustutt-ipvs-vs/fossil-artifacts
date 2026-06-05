#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <stack>
#include <vector>

#include <fossil/detail/unordered.hpp>

namespace fossil::detail {

class deadlock_tree
{
public:
    using resource_id = std::uint64_t;
    using accessor_id = std::uint64_t;

    enum class deadlock_state { NO_DANGER, DANGER, RECURSION };

    deadlock_tree() noexcept = default;
    deadlock_tree(const deadlock_tree&) = delete;
    deadlock_tree(deadlock_tree&&) = delete;
    auto operator=(const deadlock_tree&) -> deadlock_tree& = delete;
    auto operator=(deadlock_tree&&) -> deadlock_tree& = delete;

    auto await(accessor_id accessor, resource_id resource) noexcept -> bool
    {
        std::unique_lock lock{mtx_};

        if(await_causes_deadlock(accessor, resource)) {
            return false;
        }

        waits_for_.emplace(accessor, resource);
        awaited_by_[resource].emplace_back(accessor);

        return true;
    }

    auto own(accessor_id accessor, resource_id resource) noexcept -> void
    {
        std::unique_lock lock{mtx_};

        waits_for_.erase(accessor);

        if(auto aw_it = awaited_by_.find(resource); aw_it != awaited_by_.end()) {
            std::erase(aw_it->second, accessor);
        }

        owns_[accessor].emplace_back(resource);
        owned_by_.emplace(resource, accessor);
    }

    auto release(resource_id resource) noexcept -> void
    {
        std::unique_lock lock{mtx_}; // Ensure thread-safety

        auto owned_by_it = owned_by_.find(resource);
        const auto owner = owned_by_it->second;

        std::erase(owns_[owner], resource);
        if(owns_[owner].empty()) {
            owns_.erase(owner);
        }

        owned_by_.erase(owned_by_it);
    }

private:
    auto await_causes_deadlock(accessor_id accessor, resource_id resource) const noexcept -> bool
    {
        detail::unordered_set<accessor_id> visited;
        std::stack<std::pair<accessor_id, resource_id>> stack;
        stack.emplace(accessor, resource);


        while(!stack.empty()) {
            auto [current_accessor, current_resource] = stack.top();
            stack.pop();

            if(visited.find(current_accessor) != visited.end()) {
                // Found a cycle
                return true;
            }

            auto owned_resources_it = owns_.find(current_accessor);
            if(owned_resources_it == owns_.end()) {
                continue;
            }

            visited.insert(current_accessor);


            // Check if any resource owned by 'current_accessor' is awaited by another
            // accessor
            for(const auto& owned_resource : owned_resources_it->second) {
                if(owned_resource == current_resource) {
                    return true; // Owning the target resource creates a cycle
                }

                auto awaited_by_it = awaited_by_.find(owned_resource);
                if(awaited_by_it == awaited_by_.end()) {
                    continue;
                }

                for(const auto& awaiting_accessor : awaited_by_it->second) {
                    stack.emplace(awaiting_accessor, current_resource);
                }
            }

            // Check if the current_accessor is waiting for a resource that would cause a
            // cycle
            auto waits_for_it = waits_for_.find(current_accessor);
            if(waits_for_it != waits_for_.end() and waits_for_it->second == current_resource) {

                auto owned_by_it = owned_by_.find(current_resource);
                if(owned_by_it != owned_by_.end()) {
                    stack.emplace(owned_by_it->second, current_resource);
                }
            }

            // Backtrack: Mark the current_accessor as not visited to allow other paths to
            // explore it
            visited.erase(current_accessor);
        }

        return false; // No cycle found
    }


private:
    std::mutex mtx_;
    detail::unordered_map<accessor_id, resource_id> waits_for_;
    detail::unordered_map<resource_id, std::vector<accessor_id>> awaited_by_;

    detail::unordered_map<accessor_id, std::vector<resource_id>> owns_;
    detail::unordered_map<resource_id, accessor_id> owned_by_;
};


} // namespace fossil::detail
