#pragma once

#include <algorithm>
#include <cstdint>
#include <fossil/detail/nv_allocator.hpp>
#include <fossil/detail/nv_util.hpp>
#include <fossil/detail/serialize.hpp>
#include <fossil/detail/unordered.hpp>
#include <fossil/is_resizable_object.hpp>
#include <fossil/reference.hpp>
#include <fossil/transaction_header.hpp>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fossil::detail {

struct c_object_header
{
    const constinit static inline std::uint64_t OBJECT_MAGIC = 0xCCCCCCCCCCCCCCCC;
    constinit static std::uint64_t OBJECT_HEADER_SIZE;

    std::uint64_t magic = OBJECT_MAGIC;
    std::uint64_t version; // higher version means that this should have precedence

    std::uint64_t object_reference;
};
constinit inline std::uint64_t c_object_header::OBJECT_HEADER_SIZE = sizeof(c_object_header);

struct r_object_header
{
    const constinit static inline std::uint64_t OBJECT_MAGIC = 0xDDDDDDDDDDDDDDDD;
    constinit static std::uint64_t OBJECT_HEADER_SIZE;

    std::uint64_t magic = OBJECT_MAGIC;
    std::uint64_t version; // higher version means that this should have precedence

    std::uint64_t object_reference;

    std::uint64_t log_offset;
    std::uint64_t log_length;
};
constinit inline std::uint64_t r_object_header::OBJECT_HEADER_SIZE = sizeof(r_object_header);

struct mem_locations
{
    // points to memory allocations that start with either a c or r object header
    nv_span main;
    nv_span back;

    void swap() { std::swap(main, back); }
};

class pmem
{
public:
    explicit pmem(std::size_t index) : alloc_(nv_allocator::create_unsafe(index)) {}

    template<typename T>
    void emplace(std::span<const std::byte> serialized, reference<T> reference)
    {
        static_assert(not is_resizable_object<T>);

        auto allocation = alloc_.allocate(serialized.size() + c_object_header::OBJECT_HEADER_SIZE);
        auto back = alloc_.allocate(serialized.size() + c_object_header::OBJECT_HEADER_SIZE);

        auto* const header = new(allocation.data())
            c_object_header{.version = 0, .object_reference = reference.id};
        new(back.data()) c_object_header{.version = 0, .object_reference = reference.id};
        nv_mem_fence(header, sizeof(*header));

        // copy the object to allocated memory
        nt_memcpy(allocation.data() + c_object_header::OBJECT_HEADER_SIZE,
                  serialized.data(),
                  serialized.size());

        header->version = 1;
        clwb(&header->version);
        sfence();

        // store in vector
        mem_ptr_.emplace(reference.id, {.main = allocation, .back = back});
    }

    template<is_resizable_object T>
    void emplace(std::span<const std::byte> serialized,
                 std::span<const std::byte> log,
                 reference<T> reference)
    {
        auto log_offset = nv_allocator::invalid_offset();

        if(not log.empty()) {
            auto log_allocation = alloc_.allocate(std::max(16uz, std::bit_ceil(log.size() * 2)));
            log_offset = log_allocation.offset();

            nt_memcpy(log_allocation.data(), log.data(), log.size());
        }

        auto allocation = alloc_.allocate(serialized.size() + r_object_header::OBJECT_HEADER_SIZE);
        auto back = alloc_.allocate(serialized.size() + r_object_header::OBJECT_HEADER_SIZE);

        auto* const header = new(allocation.data())
            r_object_header{.version = 0,
                            .object_reference = reference.id,
                            .log_offset = log_offset,
                            .log_length = log.size()};
        new(back.data()) r_object_header{.version = 0,
                                         .object_reference = reference.id,
                                         .log_offset = 0,
                                         .log_length = 0};
        nv_mem_fence(header, sizeof(*header));

        // copy the object to allocated memory
        nt_memcpy(allocation.data() + r_object_header::OBJECT_HEADER_SIZE,
                  serialized.data(),
                  serialized.size());

        header->version = 1;
        clwb(&header->version);
        sfence();

        // store in vector
        mem_ptr_.emplace(reference.id, mem_locations{.main = allocation, .back = back});
    }

    template<typename T>
    void update(std::span<const std::byte> serialized, reference<T> reference)
    {
        static_assert(not is_resizable_object<T>);

        auto& allocs = mem_ptr_.at(reference.id);

        auto* old_header = reinterpret_cast<c_object_header*>(allocs.main.data());
        auto old_version = old_header->version;

        auto* new_header = reinterpret_cast<c_object_header*>(allocs.back.data());

        // copy the object to allocated memory
        nt_memcpy(allocs.back.data() + c_object_header::OBJECT_HEADER_SIZE,
                  serialized.data(),
                  serialized.size());

        sfence();

        nt_store_i64(&(new_header->version), old_header->version + 1);

        sfence();

        allocs.swap();
    }

    template<is_resizable_object T>
    void update(std::span<const std::byte> serialized,
                std::span<const std::byte> log,
                reference<T> reference)
    {
        const auto* const old_header = header_of(reference);
        const auto snapshot = resizable_header_snapshot{
            .version = old_header->version,
            .log_offset = old_header->log_offset,
            .log_length = old_header->log_length,
        };

        if(log.empty()) {
            update_no_log_changes(serialized, snapshot, reference);
            return;
        }

        if(snapshot.log_length == 0) {
            // there is no allocation yet
            update_no_old_log(serialized, log, reference);
            return;
        }
        assert(snapshot.log_offset != nv_allocator::invalid_offset());

        // there is an old log and we have some updates in the new log
        auto log_allocation = alloc_.from_offset(snapshot.log_offset);
        auto available = log_allocation.size() - snapshot.log_length;

        if(available >= log.size()) {
            update_append_log(serialized, log, log_allocation, snapshot, reference);
        } else {
            update_realloc_log(serialized, log, log_allocation, snapshot, reference);
        }
    }

    template<is_resizable_object T>
    auto log_of(reference<T> reference) -> std::span<const std::byte>
    {
        // lock handled implicitly via map access and allocation offsets,
        // assuming callers hold the global object read/write lock.
        const r_object_header* const header = header_of(reference);
        if(header->log_length == 0) {
            return {};
        }
        auto log_allocation = alloc_.from_offset(header->log_offset);
        return log_allocation.resize(header->log_length);
    }

    template<typename T>
    auto read(reference<T> reference) -> std::span<const std::byte>
    {
        auto it = mem_ptr_.find(reference.id);
        assert(it != mem_ptr_.end() && it->second.main.offset() != nv_allocator::invalid_offset() &&
               "Cannot read unknown object");
        if(it == mem_ptr_.end() || it->second.main.offset() == nv_allocator::invalid_offset()) {
            std::terminate();
        }

        return it->second.main;
    }

    template<typename T>
    void free_old_versions(reference<T> reference)
    {
        auto it = old_allocs_.find(reference.id);
        if(it == old_allocs_.end()) {
            return;
        }
        for(auto allocation : it->second) {
            alloc_.free(allocation);
        }
        it->second.clear();
    }

private:
    struct resizable_header_snapshot
    {
        std::uint64_t version;
        std::uint64_t log_offset;
        std::uint64_t log_length;
    };

    auto contains(std::uint64_t id) const -> bool
    {
        auto it = mem_ptr_.find(id);
        return it != mem_ptr_.end() && it->second.main.offset() != nv_allocator::invalid_offset();
    }

    auto filter_allocs(std::vector<nv_span> allocations) const noexcept
    {
        std::erase_if(allocations, [&](auto alloc) {
            if(const auto* r = reinterpret_cast<const r_object_header*>(alloc.data());
               r->magic == r_object_header::OBJECT_MAGIC) {
                return false;
            }

            if(const auto* c = reinterpret_cast<const c_object_header*>(alloc.data());
               c->magic == c_object_header::OBJECT_MAGIC) {
                return false;
            }
            return true;
        });

        return allocations;
    }

    struct recover_nv_span
    {
        std::variant<r_object_header*, c_object_header*> object_header;
        transaction_header* tx_header;
        nv_span raw;
    };

    struct recovered_version
    {
        std::uint64_t object_id;
        std::uint64_t version;
        std::uint64_t tx_id;
        std::uint64_t condition_tx_id;
        nv_span raw;
    };

    static auto recover_version_from(nv_span alloc) -> std::optional<recovered_version>
    {
        auto* ch = reinterpret_cast<c_object_header*>(alloc.data());
        if(ch->magic == c_object_header::OBJECT_MAGIC) {
            return recover_version_from(alloc, *ch, c_object_header::OBJECT_HEADER_SIZE);
        }

        auto* rh = reinterpret_cast<r_object_header*>(alloc.data());
        if(rh->magic == r_object_header::OBJECT_MAGIC) {
            return recover_version_from(alloc, *rh, r_object_header::OBJECT_HEADER_SIZE);
        }

        return std::nullopt;
    }

    static auto recover_version_from(nv_span alloc,
                                     const auto& object_header,
                                     std::uint64_t header_size)
        -> std::optional<recovered_version>
    {
        if(object_header.version == 0 ||
           alloc.size() < header_size + transaction_header::TX_HEADER_SIZE) {
            return std::nullopt;
        }

        auto* tx_header = reinterpret_cast<const transaction_header*>(alloc.data() + header_size);
        if(tx_header->magic != transaction_header::TX_MAGIC) {
            return std::nullopt;
        }

        return recovered_version{
            .object_id = object_header.object_reference,
            .version = object_header.version,
            .tx_id = tx_header->tx_id,
            .condition_tx_id = tx_header->condition_tx_id,
            .raw = alloc,
        };
    }

    static auto fulfilled_transactions(const std::vector<recovered_version>& versions)
        -> std::unordered_set<std::uint64_t>
    {
        constexpr auto tx_shard_mask = std::uint64_t{63};

        std::unordered_set<std::uint64_t> fulfilled;
        std::unordered_map<std::uint64_t, std::uint64_t> max_fulfilled_tx_by_shard;

        auto condition_is_fulfilled = [&fulfilled, &max_fulfilled_tx_by_shard](
                                          std::uint64_t condition) {
            return condition == transaction_header::NO_CONDITON_TX ||
                fulfilled.contains(condition) ||
                [&] {
                    const auto shard = condition & tx_shard_mask;
                    const auto it = max_fulfilled_tx_by_shard.find(shard);
                    return it != max_fulfilled_tx_by_shard.end() && it->second > condition;
                }();
        };

        bool changed = true;
        while(changed) {
            changed = false;
            for(const auto& version : versions) {
                if(fulfilled.contains(version.tx_id) ||
                   not condition_is_fulfilled(version.condition_tx_id)) {
                    continue;
                }

                fulfilled.insert(version.tx_id);
                auto& max_tx = max_fulfilled_tx_by_shard[version.tx_id & tx_shard_mask];
                max_tx = std::max(max_tx, version.tx_id);
                changed = true;
            }
        }

        return fulfilled;
    }

    static auto select_recovered_locations(
        const std::vector<nv_span>& object_allocs,
        const std::vector<recovered_version>& versions,
        const std::unordered_set<std::uint64_t>& fulfilled)
        -> std::unordered_map<std::uint64_t, mem_locations>
    {
        std::unordered_map<std::uint64_t, mem_locations> selected;

        for(const auto& version : versions) {
            if(not fulfilled.contains(version.tx_id)) {
                continue;
            }

            auto it = selected.find(version.object_id);
            if(it == selected.end()) {
                selected.emplace(version.object_id,
                                 mem_locations{.main = version.raw, .back = nv_span{nullptr}});
                continue;
            }

            auto current = recover_version_from(it->second.main);
            if(current.has_value() && current->version < version.version) {
                it->second.main = version.raw;
            }
        }

        for(auto& [object_id, loc] : selected) {
            for(const auto& alloc : object_allocs) {
                if(alloc.offset() == loc.main.offset() || obj_id_of_alloc(alloc) != object_id) {
                    continue;
                }

                loc.back = alloc;
                break;
            }
        }

        std::erase_if(selected, [](const auto& pair) { return not pair.second.back; });

        return selected;
    }


    // struct info
    // {
    //     std::uint64_t version;
    //     nv_span mem;
    //     bool is_valid;
    //     std::uint64_t tx;
    //     std::uint64_t condition;
    // };
    // auto to_infos(const std::vector<nv_span>& allocations) const noexcept
    // {
    //     detail::unordered_map<std::uint64_t, std::vector<info>> id_to_info_map;

    //     for(auto alloc : allocations) {
    //         if(const auto* r = reinterpret_cast<const r_object_header*>(alloc.data());
    //            r->magic == r_object_header::OBJECT_MAGIC) {

    //             const auto version = r->version;
    //             const auto ref = r->object_reference;

    //             const auto* trans_header = reinterpret_cast<const transaction_header*>(
    //                 alloc.data() + r_object_header::OBJECT_HEADER_SIZE);

    //             id_to_info_map[ref].emplace_back(info{
    //                 .version = version,
    //                 .mem = alloc,
    //                 .is_valid = r->is_valid == r_object_header::VALID,
    //                 .tx = trans_header->tx_id,
    //                 .condition = trans_header->condition_tx_id,
    //             });
    //             continue;
    //         }

    //         if(const auto* c = reinterpret_cast<const c_object_header*>(alloc.data());
    //            c->magic == c_object_header::OBJECT_MAGIC) {

    //             const auto version = c->version;
    //             const auto ref = c->object_reference;

    //             const auto* trans_header = reinterpret_cast<const transaction_header*>(
    //                 alloc.data() + c_object_header::OBJECT_HEADER_SIZE);

    //             id_to_info_map[ref].emplace_back(info{
    //                 .version = version,
    //                 .mem = alloc,
    //                 .is_valid = c->is_valid == c_object_header::VALID,
    //                 .tx = trans_header->tx_id,
    //                 .condition = trans_header->condition_tx_id,
    //             });
    //             continue;
    //         }

    //         std::unreachable();
    //     }

    //     return id_to_info_map;
    // }

    // auto map_to_mem_locations(const detail::unordered_map<std::uint64_t, std::vector<info>>&
    // id_map)
    //     -> detail::unordered_map<std::uint64_t, mem_locations>
    // {
    //     detail::unordered_map<std::uint64_t, mem_locations> out;
    //     for(const auto& [ref, infos] : id_map) {
    //         if(infos.size() != 2) {
    //             // there should always be 2 allocations per object (main, back)
    //             // otherwise, something went wrong during the creation of this object
    //             // hece, it does not exist
    //         }

    //         out.emplace(ref, mem_locations{.main = infos[0].mem, .back = infos[1].mem});
    //     }
    //     return out;
    // }

    // void process_mem_locations(detail::unordered_map<uint64_t, mem_locations>& locations)
    // {
    //     for(auto& allocations : locations) {}
    // }

    // auto process_r_locations(mem_locations& loc) -> bool
    // {
    //     auto* main = reinterpret_cast<r_object_header*>(loc.main.data());
    //     if(main->magic != r_object_header::OBJECT_MAGIC) {
    //         return false;
    //     }
    //     auto* back = reinterpret_cast<r_object_header*>(loc.back.data());
    //     if(back->magic != r_object_header::OBJECT_MAGIC) {
    //         return false;
    //     }


    //     if(main->is_valid == r_object_header::VALID && back->is_valid == r_object_header::VALID)
    //     {
    //         // both are valid, we have to make a decision
    //         auto* main_tx = reinterpret_cast<transaction_header*>(
    //             loc.main.data() + r_object_header::OBJECT_HEADER_SIZE);
    //         auto* back_tx = reinterpret_cast<transaction_header*>(
    //             loc.back.data() + r_object_header::OBJECT_HEADER_SIZE);

    //         if(main->version < back->version) {
    //             loc.swap();
    //         }
    //         return true;

    //     } else if(main->is_valid == r_object_header::VALID) {
    //         // main is valid
    //         return true;
    //     } else if(back->is_valid == r_object_header::VALID) {
    //         // back is valid
    //         loc.swap();
    //         return true;
    //     } else {
    //         // neither is valid, weird?
    //         return false;
    //     }
    // }

    // auto process_c_locations(mem_locations& loc, const detail::unordered_map<std::uint64_t,
    // std::vector<info>>& id_map) -> bool
    // {
    //     auto* main = reinterpret_cast<c_object_header*>(loc.main.data());
    //     if(main->magic != c_object_header::OBJECT_MAGIC) {
    //         return false;
    //     }
    //     auto* back = reinterpret_cast<c_object_header*>(loc.back.data());
    //     if(back->magic != c_object_header::OBJECT_MAGIC) {
    //         return false;
    //     }

    //     if(main->is_valid == c_object_header::VALID && back->is_valid == c_object_header::VALID)
    //     {
    //         // both are valid, we have to make a decision
    //         auto* main_tx = reinterpret_cast<transaction_header*>(
    //             loc.main.data() + c_object_header::OBJECT_HEADER_SIZE);
    //         auto* back_tx = reinterpret_cast<transaction_header*>(
    //             loc.back.data() + c_object_header::OBJECT_HEADER_SIZE);

    //         if((main->version > back->version) && is_condition_fullfilled(id_map,
    //         main_tx->condition_tx_id)) {

    //         }

    //         if( || !){
    //             // if the
    //             loc.swap();
    //         }

    //         return true;

    //     } else if(main->is_valid == c_object_header::VALID) {
    //         // main is valid
    //         return true;
    //     } else if(back->is_valid == c_object_header::VALID) {
    //         // back is valid
    //         loc.swap();
    //         return true;
    //     } else {
    //         // neither is valid, weird?
    //         return false;
    //     }
    // }

    static auto obj_id_of_alloc(nv_span alloc) -> std::uint64_t
    {

        auto* ch = reinterpret_cast<c_object_header*>(alloc.data());
        if(ch->magic != c_object_header::OBJECT_MAGIC) {

            auto* rh = reinterpret_cast<r_object_header*>(alloc.data());
            assert(rh->magic == r_object_header::OBJECT_MAGIC);
            return rh->object_reference;
        }
        return ch->object_reference;
    }

    auto find_object_allocs(std::vector<nv_span> allocs)
        -> std::unordered_map<std::uint64_t, mem_locations>
    {
        std::unordered_map<std::uint64_t, mem_locations> locs;

        for(const auto& alloc : allocs) {
            auto obj_id = obj_id_of_alloc(alloc);

            auto it = locs.find(obj_id);
            if(it != locs.end()) {
                it->second.back = alloc;
            } else {
                locs.emplace(obj_id, mem_locations{.main = alloc, .back = nv_span{nullptr}});
            }
        }

        std::erase_if(locs, [](const auto& pair) { return not pair.second.back; });

        return locs;
    }

    static auto fix_mem_location(mem_locations& loc) -> bool
    {
        // returns true if the object is invalid and create has never commited

        auto* cmain = reinterpret_cast<c_object_header*>(loc.main.data());
        auto* cback = reinterpret_cast<c_object_header*>(loc.back.data());
        auto* rmain = reinterpret_cast<r_object_header*>(loc.main.data());
        auto* rback = reinterpret_cast<r_object_header*>(loc.back.data());

        if(cmain->magic == c_object_header::OBJECT_MAGIC && cback->magic == c_object_header::OBJECT_MAGIC) {
            if(cback->version > cmain->version) {
                loc.swap();
            }

            return false;
        }
        if(rmain->magic == r_object_header::OBJECT_MAGIC && rback->magic == r_object_header::OBJECT_MAGIC) {
            if(rback->version > rmain->version) {
                loc.swap();
            }

            return false;
        }

        return true;
    }


public:
    auto recover() -> std::uint64_t
    {
        mem_ptr_.clear();

        std::vector<nv_span> found_allocations = alloc_.recover();
        // only allocs that contain either c or r headers.
        auto filtered = filter_allocs(std::move(found_allocations));

        std::uint64_t max_ref_seen = 0;
        std::vector<recovered_version> versions;
        versions.reserve(filtered.size());

        for(const auto& alloc : filtered) {
            max_ref_seen = std::max(max_ref_seen, obj_id_of_alloc(alloc));
            if(auto version = recover_version_from(alloc); version.has_value()) {
                versions.push_back(*version);
            }
        }

        auto fulfilled = fulfilled_transactions(versions);
        auto object_locations = select_recovered_locations(filtered, versions, fulfilled);

        for(const auto& pair : object_locations) {
            mem_ptr_.emplace(pair);
        }

        return max_ref_seen + 1;
    }

private:
    template<typename T>
    auto header_of(reference<T> reference) const noexcept -> c_object_header*
    {
        assert(contains(reference.id) && "Cannot query header of unknown object");
        return reinterpret_cast<c_object_header*>(mem_ptr_.find(reference.id)->second.main.data());
    }

    template<is_resizable_object T>
    auto header_of(reference<T> reference) const noexcept -> r_object_header*
    {
        assert(contains(reference.id) && "Cannot query header of unknown object");
        return reinterpret_cast<r_object_header*>(mem_ptr_.find(reference.id)->second.main.data());
    }
    auto is_condition_fullfilled(const auto& id_to_info_map,
                                 std::uint64_t condition_id) const noexcept -> bool
    {
        if(condition_id == transaction_header::NO_CONDITON_TX) {
            return true;
        }

        for(const auto& [ref, infos] : id_to_info_map) {
            for(const auto& info : infos) {
                if(info.tx == condition_id) {
                    return is_condition_fullfilled(id_to_info_map, info.condition);
                }
            }
        }

        return false;
    }

    template<is_resizable_object T>
    void update_no_old_log(std::span<const std::byte> serialized,
                           std::span<const std::byte> log,
                           reference<T> reference)
    {
        // we catch this case and would branch into update_no_log_changes
        assert(not log.empty());

        auto new_log_allocation = alloc_.allocate(std::max(16uz, std::bit_ceil(2uz * log.size())));

        // copy new log to new allocation
        nt_memcpy(new_log_allocation.data(), log.data(), log.size());

        update_new_csized_part(serialized, reference, new_log_allocation.offset(), log.size());
    }

    template<is_resizable_object T>
    void update_no_log_changes(std::span<const std::byte> serialized,
                               resizable_header_snapshot old_header,
                               reference<T> reference)
    {
        update_new_csized_part(serialized, reference, old_header.log_offset, old_header.log_length);
    }

    template<is_resizable_object T>
    void update_append_log(std::span<const std::byte> serialized,
                           std::span<const std::byte> log,
                           nv_span log_allocation,
                           resizable_header_snapshot old_header,
                           reference<T> reference)
    {
        // we catch this case and would branch into update_no_log_changes
        assert(not log.empty());

        // easy case
        nt_memcpy(log_allocation.subspan(old_header.log_length).data(), log.data(), log.size());

        update_new_csized_part(serialized,
                               reference,
                               old_header.log_offset,
                               old_header.log_length + log.size());
    }

    template<is_resizable_object T>
    void update_realloc_log(std::span<const std::byte> serialized,
                            std::span<const std::byte> log,
                            nv_span log_allocation,
                            resizable_header_snapshot old_header,
                            reference<T> reference)
    {
        // we catch this case and would branch into update_no_log_changes
        assert(not log.empty());

        const auto alloc_size = std::bit_ceil(
            std::max(2 * log_allocation.size(), log_allocation.size() + log.size()));

        // difficult case
        auto new_log_allocation = alloc_.allocate(alloc_size);

        // copy old log to new allocation
        nt_memcpy(new_log_allocation.data(), log_allocation.data(), old_header.log_length);

        // copy new log to new allocation
        nt_memcpy(new_log_allocation.subspan(old_header.log_length).data(), log.data(), log.size());

        update_new_csized_part(serialized,
                               reference,
                               new_log_allocation.offset(),
                               old_header.log_length + log.size());

        // mark previous log allocation as old
        old_allocs_[reference.id].emplace_back(log_allocation);
    }

    template<is_resizable_object T>
    void update_new_csized_part(std::span<const std::byte> serialized,
                                reference<T> reference,
                                std::uint64_t log_offset,
                                std::uint64_t log_size)
    {
        auto& allocs = mem_ptr_.at(reference.id);

        auto* old_header = reinterpret_cast<r_object_header*>(allocs.main.data());
        auto old_version = old_header->version;

        auto* new_header = reinterpret_cast<r_object_header*>(allocs.back.data());

        nt_store_i64(&(new_header->log_offset), log_offset);
        nt_store_i64(&(new_header->log_length), log_size);

        // copy the object to allocated memory
        nt_memcpy(allocs.back.data() + r_object_header::OBJECT_HEADER_SIZE,
                  serialized.data(),
                  serialized.size());

        sfence();

        nt_store_i64(&(new_header->version), old_header->version + 1);

        sfence();

        // swap allocs
        allocs.swap();
    }

private:
    nv_allocator alloc_;
    detail::unordered_map<std::uint64_t, mem_locations> mem_ptr_;
    detail::unordered_map<std::uint64_t, std::vector<nv_span>> old_allocs_;
};


} // namespace fossil::detail
