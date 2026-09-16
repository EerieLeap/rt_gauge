#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <system_error>

namespace eerie_leap::utilities::type {

struct Color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;

    bool operator==(const Color&) const = default;

    [[nodiscard]] static std::expected<std::optional<Color>, std::errc> TryParse(std::string_view value) {
        if(value.empty())
            return std::nullopt;

        if(value.size() != 9 || value.front() != '#')
            return std::unexpected(std::errc::invalid_argument);

        std::array<uint8_t, 4> channels;
        for(std::size_t index = 0; index < channels.size(); ++index) {
            const char* begin = value.data() + 1 + index * 2;
            const char* end = begin + 2;
            auto result = std::from_chars(begin, end, channels[index], 16);
            if(result.ec != std::errc{} || result.ptr != end)
                return std::unexpected(std::errc::invalid_argument);
        }

        return Color { channels[0], channels[1], channels[2], channels[3] };
    }
};

} // namespace eerie_leap::utilities::type
