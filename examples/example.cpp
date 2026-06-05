#include "fossil/reference.hpp"
#include "fossil/repository.hpp"
#include <fossil/persist.hpp>
#include <fossil/transaction.hpp>

#include <format>
#include <print>
#include <ranges>

template<std::ranges::input_range Range>
struct std::formatter<Range> : std::formatter<std::string>
{
    template<typename FormatContext>
    auto format(const Range& range, FormatContext& ctx) const
    {
        auto out = ctx.out();
        // Start the range with '['
        out = std::format_to(out, "[");

        // Iterate over the range and format each element
        bool first = true;
        for(const auto& elem : range) {
            if(!first) {
                out = std::format_to(out, ", ");
            }
            first = false;
            out = std::format_to(out, "{}", elem);
        }

        // End the range with ']'
        return std::format_to(out, "]");
    }
};


template<>
struct std::formatter<std::byte> : std::formatter<std::string>
{
    template<typename FormatContext>
    auto format(std::byte range, FormatContext& ctx) const
    {
        auto out = ctx.out();
        return std::formatter<std::string>::format(std::format("{:02x}",
                                                               std::to_integer<unsigned>(range)),
                                                   ctx);
    }
};


auto main() -> int
{
    using pv = fossil::persist<std::vector<int>>;

    auto& repo = fossil::object_repo();
    // auto ref = repo.create<pv>();


    auto ref = fossil::reference<pv>{1};

    fossil::transaction(ref, [](const pv& vec) {
        std::println("{}", vec[0]);

        // vec.push_back(5);
    });
}
