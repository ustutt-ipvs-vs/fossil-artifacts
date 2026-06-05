#pragma once

#include <immintrin.h>
#include <cstdint>

#ifdef __clang__
#define ATTR_ALWAYS_INLINE clang::always_inline
#elif __GNUG__
#define ATTR_ALWAYS_INLINE gnu::always_inline
#endif

#ifdef NO_CLWB
[[ATTR_ALWAYS_INLINE]] inline void clwb(const void* ptr) { _mm_clflush(ptr); }
#else
[[ATTR_ALWAYS_INLINE]] inline void clwb(const void* ptr)
{
    _mm_clwb(const_cast<void*>(ptr)); // NOTE: gcc doesnt declare ptr as const, clang does...
}

#endif

[[ATTR_ALWAYS_INLINE]] inline void sfence() { _mm_sfence(); }

[[ATTR_ALWAYS_INLINE]] inline void nv_mem_fence(const void* ptr, size_t count)
{

    constexpr std::size_t cache_line_size = 64; // usually 64 on x86

    auto p = reinterpret_cast<std::uintptr_t>(ptr);
    auto start = p & ~(cache_line_size - 1); // round down
    auto end = (p + count + cache_line_size - 1) & ~(cache_line_size - 1); // round up

    while(start < end) {
        clwb(reinterpret_cast<const void*>(start));
        start += 64;
    }

    sfence();
}

[[ATTR_ALWAYS_INLINE]] inline void nt_optcpy128(void* dest, const void* src, unsigned char count)
{
    assert(count <= 16);

    // NOTE: negative integer shift is well-defined since C++20
    // NOTE: we subtract count >> 4, setting the second value to -1 in edge case count = 16
    auto mask = count >= 8
        ? __m128i{
            -1,
            ~(int64_t{-1} << ((count & 7) << 3)) - (count >> 4),
        }
        : __m128i{
            ~(int64_t{-1} << (count << 3)),
            0,
        };

    auto si = __m128i();
    memcpy(&si, src, count);

    _mm_maskmoveu_si128(si, mask, static_cast<char*>(dest));
}

[[ATTR_ALWAYS_INLINE]] inline void nt_memcpy_aligned(void* dest, const void* src, std::size_t count)
{
    assert(count < 32 || reinterpret_cast<std::uintptr_t>(dest) % 32 == 0);
    assert(count < 16 || reinterpret_cast<std::uintptr_t>(dest) % 16 == 0);

    const auto* src_char = reinterpret_cast<const unsigned char*>(src);
    auto* dest_char = reinterpret_cast<unsigned char*>(dest);

    auto chunk_bytes = count & -32;

    std::size_t i = 0;
    for(; i < chunk_bytes; i += 32) {
        auto si = __m256i();
        memcpy(&si, src_char + i, 32);

        _mm256_stream_si256(reinterpret_cast<__m256i*>(dest_char + i), si);
    }

    if(count & 16) {
        auto si = __m128i();
        memcpy(&si, src_char + i, 16);

        _mm_stream_si128(reinterpret_cast<__m128i*>(dest_char + i), si);
        i += 16;
    }

    if(count & 15) {
        nt_optcpy128(dest_char + i, src_char + i, count & 15);
    }
}

[[ATTR_ALWAYS_INLINE]] void nt_store_i64(void* dst, std::int64_t value) {
    _mm_stream_si64(reinterpret_cast<long long*>(dst), value);
}
[[ATTR_ALWAYS_INLINE]] void nt_store_i64(void* dst, std::uint64_t value) {
    nt_store_i64(dst, static_cast<std::int64_t>(value));
}


[[ATTR_ALWAYS_INLINE]] inline void nt_memcpy(void* dest, const void* src, std::size_t count)
{
    const auto* src_char = reinterpret_cast<const unsigned char*>(src);
    auto* dest_char = reinterpret_cast<unsigned char*>(dest);
    auto dest_pos = reinterpret_cast<std::uintptr_t>(dest);

    if(count <= 16) {
        nt_optcpy128(dest_char, src_char, count);
    } else if(count < 32) {
        nt_optcpy128(dest_char, src_char, 16);
        nt_optcpy128(dest_char + 16, src_char + 16, count - 16);

    } else if((dest_pos & 31) == 0) {
        nt_memcpy_aligned(dest_char, src_char, count);
    } else {
        auto prefix = (32 - (dest_pos & 31)) & 31;
        const auto consumed_prefix = prefix;

        if(prefix > 16) {
            nt_optcpy128(dest_char, src_char, 16);
            prefix -= 16;
            dest_char += 16;
            src_char += 16;
        }

        nt_optcpy128(dest_char, src_char, prefix);

        // The aligned tail must subtract the full unaligned prefix. `prefix` may
        // have been reduced after the first 16-byte copy; using the reduced
        // value here overreads the source log and can corrupt FossilDB prefill.
        nt_memcpy_aligned(dest_char + prefix, src_char + prefix, count - consumed_prefix);
    }
}
