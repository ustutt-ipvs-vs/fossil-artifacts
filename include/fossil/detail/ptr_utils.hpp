#pragma once


#include <cassert>
#include <cstddef>
#include <cstdint>

namespace fossil::detail {

/**
 * Aligns a pointer to a specified byte boundary.
 *
 * @param ptr The pointer to be aligned.
 * @param alignment The desired byte boundary alignment. Must be a power of 2.
 * @return The aligned pointer.
 *
 * This function takes a pointer and aligns it to the specified byte boundary.
 * It ensures that the returned pointer is divisible by the alignment value.
 * If the input pointer is already aligned, it is returned as is.
 *
 * Note: This function assumes that the alignment value is a power of two.
 *
 * Example usage:
 * std::byte* ptr = ...; // Pointer to be aligned
 * std::size_t alignment = 16; // Desired alignment boundary
 * std::byte* aligned_ptr = align_to_boundary(ptr, alignment);
 * // Use aligned_ptr, which is now aligned to a 16-byte boundary
 */
static inline auto align_to_boundary(std::uint64_t offset, std::size_t alignment) -> std::uint64_t
{
    // auto misalignment = reinterpret_cast<uintptr_t>(ptr) % alignment;
    // if(misalignment != 0) {
    //     ptr += (alignment - misalignment);
    // }
    // return ptr;

    // optimization which uses that the alignment is a power of 2
    // The expression (alignment - 1) generates a mask that has all the bits set below the alignment
    // bit. The bitwise NOT operator ~ inverts the bits of the mask, which results in all the bits
    // being unset below the alignment bit and all bits being set at the alignment bit and above.
    // The bitwise AND operation & then aligns the pointer to the nearest higher boundary that is a
    // multiple of alignment.
    assert((alignment & (alignment - 1)) == 0); // Ensure alignment is a power of 2

    return (offset + alignment - 1) & ~(alignment - 1);
}

} // namespace fossil::detail
