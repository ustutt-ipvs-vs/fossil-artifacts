#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fossil/concepts.hpp>
#include <fossil/detail/ptr_utils.hpp>
#include <fossil/detail/nv_util.hpp>
#include <fossil/reference.hpp>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace fossil::detail {

struct memory_header
{
    const constinit static inline std::uint64_t MEMORY_MAGIC = 0xAAAAAAAAAAAAAAAA;
    constinit static std::uint64_t MEMORY_HEADER_SIZE;

    std::uint64_t magic = MEMORY_MAGIC;
    std::uint64_t version = 1;
    std::uint64_t segments_offset = 0;
};
constinit inline std::uint64_t memory_header::MEMORY_HEADER_SIZE = sizeof(memory_header);

struct alloc_header
{
    const constinit static inline std::uint64_t ALLOC_MAGIC = 0xBBBBBBBBBBBBBBBB;
    constinit static std::uint64_t ALLOC_HEADER_SIZE;

    std::uint64_t magic = ALLOC_MAGIC;
    std::uint64_t capacity = 0;
    std::uint64_t order = 0;
};
constinit inline std::uint64_t alloc_header::ALLOC_HEADER_SIZE = sizeof(alloc_header);

struct segment_header
{
    const constinit static inline std::uint64_t SEGMENT_MAGIC = 0xABCDABCDABCDABCD;
    constinit static std::uint64_t SEGMENT_HEADER_SIZE;

    std::uint64_t magic = SEGMENT_MAGIC;
    std::uint64_t total_size = 0;
    std::uint64_t arena_offset = 0;
    std::uint64_t arena_size = 0;
};
constinit inline std::uint64_t segment_header::SEGMENT_HEADER_SIZE = sizeof(segment_header);

class nv_allocator;

class nv_span
{
public:
    constexpr nv_span(std::uint64_t offset, std::uint64_t size, nv_allocator* alloc)
        : offset_(offset), size_(size), alloc_(alloc)
    {}
    explicit constexpr nv_span(std::nullptr_t) : offset_(0), size_(0), alloc_(nullptr) {}

    constexpr auto subspan(std::uint64_t shift) const noexcept -> nv_span
    {
        return {offset_ + shift, size_ - shift, alloc_};
    }
    constexpr auto resize(std::uint64_t size) -> nv_span { return {offset_, size, alloc_}; }

    constexpr auto supspan(std::uint64_t shift) const noexcept -> nv_span
    {
        return {offset_ - shift, size_ + shift, alloc_};
    }

    constexpr auto data() const noexcept -> std::byte*;

    constexpr auto header() const noexcept -> alloc_header*
    {
        return reinterpret_cast<alloc_header*>(data() - alloc_header::ALLOC_HEADER_SIZE);
    }

    constexpr auto begin() const noexcept -> std::byte* { return data(); }

    constexpr auto end() const noexcept -> std::byte* { return data() + size_; }

    constexpr auto offset() const noexcept -> std::uint64_t { return offset_; }

    constexpr auto size() const noexcept -> std::uint64_t { return size_; }

    constexpr operator bool() const
    {
        return alloc_ != nullptr;
    }

private:
    std::uint64_t offset_;
    std::uint64_t size_;

    nv_allocator* alloc_;
};

class nv_allocator
{
public:
    nv_allocator() = delete;
    auto operator=(const nv_allocator&) -> nv_allocator& = delete;
    auto operator=(nv_allocator&&) -> nv_allocator& = delete;
    nv_allocator(const nv_allocator&) = delete;
    nv_allocator(nv_allocator&&) noexcept = delete;

    ~nv_allocator()
    {
        if(memory_ != nullptr and mem_size_ != 0) {
            munmap(memory_, mem_size_);
        }
        if(fd_ >= 0) {
            close(fd_);
        }
    }

    auto allocate(std::size_t capacity) -> nv_span
    {
        const auto required = capacity + alloc_header::ALLOC_HEADER_SIZE;
        const auto target_order = size_to_order(required);

        while(true) {
            if(auto offset = try_allocate(target_order, capacity); offset != invalid_offset()) {
                return {offset, capacity, this};
            }

            grow(required);
        }
    }

    void free(nv_span region)
    {
        auto* const header = region.header();
        assert(header->magic == alloc_header::ALLOC_MAGIC);

        const auto block_offset = region.offset() - alloc_header::ALLOC_HEADER_SIZE;
        auto current_order = static_cast<std::uint8_t>(header->order);
        auto current_offset = block_offset;

        header->magic = 0;
        header->capacity = 0;
        header->order = 0;

        const auto& segment = segment_of(block_offset);
        while(current_order < segment.root_order) {
            const auto buddy_offset = buddy_of(segment, current_offset, current_order);
            if(not is_free_block(buddy_offset, current_order)) {
                break;
            }

            erase_free_block(buddy_offset, current_order);
            current_offset = std::min(current_offset, buddy_offset);
            ++current_order;
        }

        add_free_block(current_offset, current_order);
    }

    auto flush() const noexcept -> void { msync(memory_, mem_size_, MS_SYNC); }

    static auto create_unsafe(std::size_t shard = 0) -> nv_allocator
    {
        auto filename = std::filesystem::path(std::format("fossil_persistence_{}", shard));
        if(not storage_directory().empty()) {
            filename = storage_directory() / filename;
        }
        return create(filename.string());
    }

    static void set_storage_directory(std::filesystem::path directory)
    {
        storage_directory() = std::move(directory);
    }

    static void set_dax_mapping(bool enabled)
    {
        dax_mapping() = enabled;
    }

    auto recover() -> std::vector<nv_span>
    {
        std::vector<nv_span> allocations;

        if(is_first_startup_) {
            return allocations;
        }

        for(const auto& segment : segments_) {
            const auto persisted = scan_segment_allocations(segment);
            allocations.reserve(allocations.size() + persisted.size());

            for(const auto& allocation : persisted) {
                allocations.emplace_back(allocation.block_offset + alloc_header::ALLOC_HEADER_SIZE,
                                         allocation.capacity,
                                         this);
            }
        }

        return allocations;
    }

    static constexpr auto invalid_offset() -> std::uint64_t
    {
        return static_cast<std::uint64_t>(-1);
    }

    auto from_offset(std::uint64_t offset) -> nv_span
    {
        auto* const header = reinterpret_cast<alloc_header*>(memory_ + offset -
                                                             alloc_header::ALLOC_HEADER_SIZE);
        return {offset, header->capacity, this};
    }

private:
    struct segment_info
    {
        std::uint64_t header_offset;
        std::uint64_t arena_offset;
        std::uint64_t arena_size;
        std::uint8_t root_order;
    };

    struct persisted_alloc
    {
        std::uint64_t block_offset;
        std::uint64_t capacity;
        std::uint8_t order;
    };

    nv_allocator(int fd, std::byte* mem, std::size_t mem_size)
        : fd_(fd), mem_size_(mem_size), memory_(mem)
    {
        if(not has_valid_memory_header()) {
            reset_storage();
            is_first_startup_ = true;
        }

        rebuild_runtime_state();
    }

    auto offset_to_ptr(nv_span alloc) const noexcept -> std::byte*
    {
        return memory_ + alloc.offset();
    }

    static constexpr auto min_block_size() -> std::uint64_t { return 64; }

    static constexpr auto min_block_order() -> std::uint8_t { return 6; }

    static constexpr auto initial_segment_size() -> std::uint64_t { return 1ULL << 20; }

    static auto page_size() -> std::uint64_t
    {
        const auto value = sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<std::uint64_t>(value) : 4096;
    }

    static auto block_size(std::uint8_t order) -> std::uint64_t { return 1ULL << order; }

    static auto size_to_order(std::uint64_t required) -> std::uint8_t
    {
        const auto rounded = std::max(required, min_block_size());
        const auto size = std::bit_ceil(rounded);
        return static_cast<std::uint8_t>(std::bit_width(size) - 1);
    }

    auto has_valid_memory_header() const noexcept -> bool
    {
        if(mem_size_ < sizeof(memory_header)) {
            return false;
        }

        const auto* const header = reinterpret_cast<const memory_header*>(memory_);
        return header->magic == memory_header::MEMORY_MAGIC and
            header->segments_offset >= align_to_boundary(memory_header::MEMORY_HEADER_SIZE, 8) and
            header->segments_offset < mem_size_;
    }

    auto memory_header_ptr() const noexcept -> memory_header*
    {
        return reinterpret_cast<memory_header*>(memory_);
    }

    auto remap(std::uint64_t new_size) -> void
    {
        if(ftruncate(fd_, static_cast<off_t>(new_size)) != 0) {
            std::println("Failed to resize the persistence file.");
            std::terminate();
        }

        if(memory_ != nullptr and mem_size_ != 0) {
            // Use mremap to extend the mapping in-place, avoiding TLB invalidation
            auto* const remapped = static_cast<std::byte*>(
                mremap(memory_, mem_size_, new_size, MREMAP_MAYMOVE));
            if(remapped == MAP_FAILED or remapped == nullptr) {
                std::println("Failed to remap the persistence file.");
                std::terminate();
            }
            memory_ = remapped;
            mem_size_ = new_size;
            return;
        }

        auto* const mapped = static_cast<std::byte*>(
            mmap(nullptr, new_size, PROT_READ | PROT_WRITE, mmap_flags(), fd_, 0));
        if(mapped == MAP_FAILED or mapped == nullptr) {
            const auto error = errno;
            std::println("Failed to map the persistence file{}: {}",
                         dax_mapping() ? " with MAP_SYNC" : "",
                         std::strerror(error));
            std::terminate();
        }

        memory_ = mapped;
        mem_size_ = new_size;
    }

    auto reset_storage() -> void
    {
        const auto segments_offset = align_to_boundary(memory_header::MEMORY_HEADER_SIZE,
                                                       page_size());
        const auto first_segment_total = aligned_segment_overhead() + initial_segment_size();

        remap(segments_offset + first_segment_total);
        std::memset(memory_, 0, mem_size_);

        auto* const header = memory_header_ptr();
        *header = memory_header{.segments_offset = segments_offset};
        nv_mem_fence(header, sizeof(*header));

        initialize_segment(segments_offset, initial_segment_size());
    }

    static constexpr auto aligned_segment_overhead() -> std::uint64_t
    {
        return align_to_boundary(segment_header::SEGMENT_HEADER_SIZE, min_block_size());
    }

    auto initialize_segment(std::uint64_t header_offset, std::uint64_t arena_size) -> void
    {
        auto* const header = reinterpret_cast<segment_header*>(memory_ + header_offset);
        *header = segment_header{.total_size = aligned_segment_overhead() + arena_size,
                                 .arena_offset = aligned_segment_overhead(),
                                 .arena_size = arena_size};
        nv_mem_fence(header, sizeof(*header));
    }

    auto is_valid_segment_header(const segment_header& header,
                                 std::uint64_t header_offset) const noexcept -> bool
    {
        return header.magic == segment_header::SEGMENT_MAGIC &&
            header.total_size >= aligned_segment_overhead() + min_block_size() &&
            header_offset + header.total_size <= mem_size_ &&
            header.arena_offset == aligned_segment_overhead() &&
            header.arena_size + aligned_segment_overhead() == header.total_size &&
            std::has_single_bit(header.arena_size);
    }

    void discard_torn_tail_segment(std::uint64_t valid_size)
    {
        if(valid_size == mem_size_) {
            return;
        }

        remap(valid_size);
    }

    auto load_segments() -> std::vector<segment_info>
    {
        std::vector<segment_info> loaded;
        const auto* const global = memory_header_ptr();
        auto current = global->segments_offset;

        while(current < mem_size_) {
            const auto* const header = reinterpret_cast<const segment_header*>(memory_ + current);
            if(not is_valid_segment_header(*header, current)) {
                if(not loaded.empty()) {
                    discard_torn_tail_segment(current);
                    break;
                }

                std::println("Corrupt persistence segment metadata.");
                std::terminate();
            }

            loaded.push_back(segment_info{
                .header_offset = current,
                .arena_offset = current + header->arena_offset,
                .arena_size = header->arena_size,
                .root_order = size_to_order(header->arena_size),
            });
            current += header->total_size;
        }

        return loaded;
    }

    auto rebuild_runtime_state() -> void
    {
        segments_ = load_segments();
        free_lists_.clear();

        for(const auto& segment : segments_) {
            ensure_free_list_capacity(segment.root_order);

            auto persisted = scan_segment_allocations(segment);
            std::ranges::sort(persisted, {}, &persisted_alloc::block_offset);

            rebuild_segment_free_lists(segment,
                                       persisted,
                                       0,
                                       persisted.size(),
                                       segment.arena_offset,
                                       segment.root_order);
        }
    }

    auto scan_segment_allocations(const segment_info& segment) const -> std::vector<persisted_alloc>
    {
        std::vector<persisted_alloc> allocations;
        const auto segment_end = segment.arena_offset + segment.arena_size;
        auto current = segment.arena_offset;

        while(current + alloc_header::ALLOC_HEADER_SIZE <= segment_end) {
            const auto* const header = reinterpret_cast<const alloc_header*>(memory_ + current);
            if(is_valid_alloc(*header, segment, current)) {
                const auto order = static_cast<std::uint8_t>(header->order);
                allocations.push_back(
                    {.block_offset = current, .capacity = header->capacity, .order = order});
                current += block_size(order);
                continue;
            }

            current += min_block_size();
        }

        return allocations;
    }

    auto is_valid_alloc(const alloc_header& header,
                        const segment_info& segment,
                        std::uint64_t block_offset) const noexcept -> bool
    {
        if(header.magic != alloc_header::ALLOC_MAGIC) {
            return false;
        }

        if(header.order < min_block_order() or header.order > segment.root_order) {
            return false;
        }

        const auto size = block_size(static_cast<std::uint8_t>(header.order));
        if(block_offset + size > segment.arena_offset + segment.arena_size) {
            return false;
        }

        if((block_offset - segment.arena_offset) % size != 0) {
            return false;
        }

        return header.capacity + alloc_header::ALLOC_HEADER_SIZE <= size;
    }

    auto rebuild_segment_free_lists(const segment_info& segment,
                                    const std::vector<persisted_alloc>& allocations,
                                    std::size_t begin,
                                    std::size_t end,
                                    std::uint64_t block_offset,
                                    std::uint8_t order) -> void
    {
        if(begin == end) {
            add_free_block(block_offset, order);
            return;
        }

        if(end - begin == 1 and allocations[begin].block_offset == block_offset and
           allocations[begin].order == order) {
            return;
        }

        if(order == min_block_order()) {
            std::println("Corrupt buddy allocator metadata.");
            std::terminate();
        }

        const auto half_size = block_size(order - 1);
        const auto split_offset = block_offset + half_size;
        const auto middle = std::lower_bound(allocations.begin() +
                                                 static_cast<std::ptrdiff_t>(begin),
                                             allocations.begin() + static_cast<std::ptrdiff_t>(end),
                                             split_offset,
                                             [](const persisted_alloc& allocation,
                                                std::uint64_t offset) {
                                                 return allocation.block_offset < offset;
                                             });
        const auto split_index = static_cast<std::size_t>(middle - allocations.begin());

        rebuild_segment_free_lists(segment,
                                   allocations,
                                   begin,
                                   split_index,
                                   block_offset,
                                   order - 1);
        rebuild_segment_free_lists(segment, allocations, split_index, end, split_offset, order - 1);
    }

    auto ensure_free_list_capacity(std::uint8_t order) -> void
    {
        if(free_lists_.size() <= order) {
            free_lists_.resize(order + 1);
        }
    }

    auto add_free_block(std::uint64_t offset, std::uint8_t order) -> void
    {
        ensure_free_list_capacity(order);
        auto& free_list = free_lists_[order];
        const auto it = std::ranges::lower_bound(free_list, offset);
        free_list.insert(it, offset);
    }

    auto erase_free_block(std::uint64_t offset, std::uint8_t order) -> void
    {
        auto& free_list = free_lists_[order];
        const auto it = std::ranges::lower_bound(free_list, offset);
        assert(it != free_list.end() and *it == offset);
        free_list.erase(it);
    }

    auto try_allocate(std::uint8_t target_order, std::uint64_t capacity) -> std::uint64_t
    {
        ensure_free_list_capacity(target_order);

        for(std::size_t order = target_order; order < free_lists_.size(); ++order) {
            auto& free_list = free_lists_[order];
            if(free_list.empty()) {
                continue;
            }

            // Take the first (lowest offset) free block
            auto block_offset = free_list.front();
            free_list.erase(free_list.begin());

            auto current_order = static_cast<std::uint8_t>(order);
            while(current_order > target_order) {
                --current_order;
                const auto right_offset = block_offset + block_size(current_order);
                add_free_block(right_offset, current_order);
            }

            auto* const header = reinterpret_cast<alloc_header*>(memory_ + block_offset);
            *header = alloc_header{.capacity = capacity, .order = target_order};
            return block_offset + alloc_header::ALLOC_HEADER_SIZE;
        }

        return invalid_offset();
    }

    auto grow(std::uint64_t required) -> void
    {
        const auto required_order = size_to_order(required);
        const auto required_size = block_size(required_order);

        auto new_segment_size = std::max(initial_segment_size(), required_size);
        if(not segments_.empty()) {
            new_segment_size = std::max(required_size, segments_.back().arena_size * 2);
        }

        const auto previous_size = mem_size_;
        remap(mem_size_ + aligned_segment_overhead() + new_segment_size);
        std::memset(memory_ + previous_size, 0, mem_size_ - previous_size);

        initialize_segment(previous_size, new_segment_size);

        const segment_info segment{
            .header_offset = previous_size,
            .arena_offset = previous_size + aligned_segment_overhead(),
            .arena_size = new_segment_size,
            .root_order = size_to_order(new_segment_size),
        };

        segments_.push_back(segment);
        add_free_block(segment.arena_offset, segment.root_order);
    }

    auto segment_of(std::uint64_t block_offset) const -> const segment_info&
    {
        const auto it = std::upper_bound(segments_.begin(),
                                         segments_.end(),
                                         block_offset,
                                         [](std::uint64_t offset, const segment_info& segment) {
                                             return offset < segment.arena_offset;
                                         });

        if(it == segments_.begin()) {
            std::println("Allocation outside of every persistence segment.");
            std::terminate();
        }

        const auto& segment = *std::prev(it);
        if(block_offset >= segment.arena_offset + segment.arena_size) {
            std::println("Allocation outside of every persistence segment.");
            std::terminate();
        }

        return segment;
    }


    auto buddy_of(const segment_info& segment,
                  std::uint64_t block_offset,
                  std::uint8_t order) const noexcept -> std::uint64_t
    {
        const auto relative_offset = block_offset - segment.arena_offset;
        const auto size = block_size(order);
        return segment.arena_offset + (relative_offset ^ size);
    }

    auto is_free_block(std::uint64_t offset, std::uint8_t order) const -> bool
    {
        if(order >= free_lists_.size()) {
            return false;
        }

        const auto& free_list = free_lists_[order];
        const auto it = std::ranges::lower_bound(free_list, offset);
        return it != free_list.end() and *it == offset;
    }

    int fd_ = -1;
    std::size_t mem_size_ = 0;
    std::byte* memory_ = nullptr;
    bool is_first_startup_ = false;
    std::vector<segment_info> segments_;
    std::vector<std::vector<std::uint64_t>> free_lists_;

    friend nv_span;

    static auto storage_directory() -> std::filesystem::path&
    {
        static std::filesystem::path directory;
        return directory;
    }

    static auto dax_mapping() -> bool&
    {
        static bool enabled = false;
        return enabled;
    }

    static auto mmap_flags() -> int
    {
        if(dax_mapping()) {
            return MAP_SHARED_VALIDATE | MAP_SYNC;
        }
        return MAP_SHARED;
    }

    static auto create(std::string_view filename) -> nv_allocator
    {
        const int fd = open(filename.data(), O_RDWR | O_CREAT, 0644);
        if(fd < 0) {
            std::println("Failed to open the persistence file: {}", filename);
            std::terminate();
        }

        struct stat st{};
        if(fstat(fd, &st) != 0) {
            std::println("Failed to query the persistence file: {}", filename);
            close(fd);
            std::terminate();
        }

        auto file_size = static_cast<std::uint64_t>(st.st_size);
        if(file_size == 0) {
            const auto initial_size = align_to_boundary(memory_header::MEMORY_HEADER_SIZE,
                                                        page_size()) +
                aligned_segment_overhead() + initial_segment_size();
            if(ftruncate(fd, static_cast<off_t>(initial_size)) != 0) {
                std::println("Failed to create the persistence file: {}", filename);
                close(fd);
                std::terminate();
            }
            file_size = initial_size;
        }

        auto* const memory = static_cast<std::byte*>(
            mmap(nullptr, file_size, PROT_READ | PROT_WRITE, mmap_flags(), fd, 0));
        if(memory == MAP_FAILED or memory == nullptr) {
            const auto error = errno;
            std::println("Failed to map the file {} into memory{}: {}",
                         filename,
                         dax_mapping() ? " with MAP_SYNC" : "",
                         std::strerror(error));
            close(fd);
            std::terminate();
        }

        return {fd, memory, file_size};
    }
};

constexpr auto nv_span::data() const noexcept -> std::byte* { return alloc_->offset_to_ptr(*this); }

} // namespace fossil::detail
