#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <iostream>

/**
 * @brief Prints the contents of a memory block in hexadecimal and ASCII formats.
 *
 * The function displays the content of a memory block, where each line of output
 * displays 16 bytes. The output contains the offset in hexadecimal, hexadecimal
 * representation of the data, and ASCII representation with non-printable
 * characters replaced by a dot ('.').
 *
 * @param addr A pointer to the beginning of the block of memory to display.
 * @param len The number of bytes from the start address to display.
 */
template<typename T>
inline void hex_dump(T* t, std::size_t len) noexcept
{
    const void* addr = reinterpret_cast<const void*>(t);
    std::array<char, 17> buff; // Stores the ASCII data
    const auto* pc = static_cast<const unsigned char*>(addr); // Cast to make the code cleaner.

    // Process every byte in the data.
    for(int i = 0; i < len; ++i) {
        // Multiple of 16 means new line (with line offset).
        if((i % 16) == 0) {
            // Just don't print ASCII for the zeroth line.
            if(i != 0) {
                std::cout << std::format("  {}", buff.data()) << std::endl;
            }
            // Output the offset.
            std::cout << std::format("  {:04x} ", i);
        }

        // Now the hex code for the specific character.
        std::cout << std::format(" {:02x}", pc[i]);

        // And store a printable ASCII character for later.
        if((pc[i] < 0x20) || (pc[i] > 0x7e)) {
            buff[i % 16] = '.';
        } else {
            buff[i % 16] = static_cast<char>(pc[i]);
        }
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out the last line if not exactly 16 characters.
    while((len % 16) != 0) {
        std::cout << std::format("   ");
        len++;
    }

    // And print the final ASCII bit.
    std::cout << std::format("  {}", buff.data()) << std::endl;
}
