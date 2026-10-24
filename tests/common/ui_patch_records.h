#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <variant>
#include <vector>

#include <eerie_memory.hpp>

#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "utilities/memory/memory_resource_manager.h"

// The records a PATCH carries, written by a CBOR writer of the tests' own, as the editor's encoder writes them
// (definite-length arrays, minimal-width integers, doubles as float64), so the unit's decoder is tried on bytes
// it did not produce.
namespace ui_patch_records {

using Bytes = std::vector<uint8_t>;

class Writer {
    Bytes out_;

    void Head(uint8_t major, uint64_t value) {
        const uint8_t m = static_cast<uint8_t>(major << 5);
        if(value < 24) {
            out_.push_back(m | static_cast<uint8_t>(value));
        } else if(value <= 0xFF) {
            out_.push_back(m | 24);
            out_.push_back(static_cast<uint8_t>(value));
        } else if(value <= 0xFFFF) {
            out_.push_back(m | 25);
            out_.push_back(static_cast<uint8_t>(value >> 8));
            out_.push_back(static_cast<uint8_t>(value));
        } else if(value <= 0xFFFFFFFFULL) {
            out_.push_back(m | 26);
            for(int shift = 24; shift >= 0; shift -= 8)
                out_.push_back(static_cast<uint8_t>(value >> shift));
        } else {
            out_.push_back(m | 27);
            for(int shift = 56; shift >= 0; shift -= 8)
                out_.push_back(static_cast<uint8_t>(value >> shift));
        }
    }

public:
    Writer& Uint(uint64_t value) {
        Head(0, value);
        return *this;
    }

    Writer& Int(int64_t value) {
        if(value >= 0)
            Head(0, static_cast<uint64_t>(value));
        else
            Head(1, static_cast<uint64_t>(-1 - value));
        return *this;
    }

    Writer& Bool(bool value) {
        out_.push_back(value ? 0xF5 : 0xF4);
        return *this;
    }

    Writer& Double(double value) {
        out_.push_back(0xFB);
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for(int shift = 56; shift >= 0; shift -= 8)
            out_.push_back(static_cast<uint8_t>(bits >> shift));
        return *this;
    }

    Writer& Text(std::string_view text) {
        return Text(text.data(), text.size());
    }

    Writer& Text(const char* text, size_t length) {
        Head(3, length);
        out_.insert(out_.end(), text, text + length);
        return *this;
    }

    Writer& Array(size_t count) {
        Head(4, count);
        return *this;
    }

    Writer& Map(size_t count) {
        Head(5, count);
        return *this;
    }

    Writer& Raw(const Bytes& bytes) {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
        return *this;
    }

    Writer& ByteString(const Bytes& bytes) {
        Head(2, bytes.size());
        return Raw(bytes);
    }

    [[nodiscard]] const Bytes& Get() const {
        return out_;
    }
};

inline void WriteValue(Writer& writer, const eerie_leap::utilities::type::ConfigValue& value) {
    std::visit(
        [&writer](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr(std::is_same_v<T, int>) {
                writer.Int(v);
            } else if constexpr(std::is_same_v<T, double>) {
                writer.Double(v);
            } else if constexpr(std::is_same_v<T, bool>) {
                writer.Bool(v);
            } else if constexpr(std::is_same_v<T, std::pmr::string>) {
                writer.Text(v.data(), v.size());
            } else if constexpr(std::is_same_v<T, std::pmr::vector<int>>) {
                writer.Array(v.size());
                for(const auto item : v)
                    writer.Int(item);
            } else {
                writer.Uint(0);
            }
        },
        value
    );
}

// A CborWidgetConfig.
inline Bytes EncodeWidget(const eerie_leap::domain::ui_domain::models::WidgetConfiguration& widget) {
    Writer writer;
    const bool has_properties = !widget.properties.empty();

    writer.Array(has_properties ? 7 : 6)
        .Uint(static_cast<uint32_t>(widget.type))
        .Uint(widget.id)
        .Array(2)
        .Int(widget.position_grid.x)
        .Int(widget.position_grid.y)
        .Array(2)
        .Uint(widget.size_grid.width)
        .Uint(widget.size_grid.height)
        .Int(widget.z_index);

    if(has_properties) {
        writer.Map(widget.properties.size());
        for(const auto& [type, value] : widget.properties) {
            writer.Uint(static_cast<uint32_t>(type));
            WriteValue(writer, value);
        }
    }

    writer.Array(widget.bindings.size());
    for(const auto& binding : widget.bindings) {
        writer.Array(9)
            .Uint(static_cast<uint32_t>(binding.target))
            .Uint(static_cast<uint32_t>(binding.event.channel))
            .Uint(binding.event.event_type)
            .Uint(binding.event.payload_key)
            .Uint(static_cast<uint32_t>(binding.direction))
            .Uint(binding.outbound_event_type)
            .Bool(binding.event.HasSelector())
            .Uint(binding.event.selector_key);
        if(binding.event.HasSelector())
            WriteValue(writer, binding.event.selector_value);
        else
            writer.Uint(0);
    }

    return writer.Get();
}

// A CborScreenConfig.
inline Bytes EncodeScreen(const eerie_leap::domain::ui_domain::models::ScreenConfiguration& screen) {
    Writer writer;

    writer.Array(8)
        .Uint(screen.id)
        .Uint(screen.screen_group_id)
        .Uint(static_cast<uint32_t>(screen.type))
        .Int(screen.z_index)
        .Bool(screen.is_visible)
        .Bool(screen.is_overlay)
        .Array(4)
        .Bool(screen.grid.snap_enabled)
        .Uint(screen.grid.width)
        .Uint(screen.grid.height)
        .Uint(screen.grid.spacing_px)
        .Array(screen.widget_configurations.size());

    for(const auto& widget : screen.widget_configurations)
        writer.Raw(EncodeWidget(*widget));

    return writer.Get();
}

} // namespace ui_patch_records
