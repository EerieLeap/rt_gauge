#include "views/assets/fonts/builtin_fonts.h"

#include <array>
#include <cstdint>

namespace eerie_leap::views::assets {

using eerie_leap::subsys::assets::BuiltInAsset;

#ifdef CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS

namespace {

// The files, as generated at build time from builtin/*.ttf (cmake/builtin_fonts.cmake). const, so the linker puts
// them in flash (.rodata) and nothing copies them to RAM. No alignment is asked for: stb_truetype reads a font byte
// by byte.
const uint8_t RUBIK_MEDIUM[] = {
    #include "builtin_font_rubik_medium.inc"
};

const uint8_t MONTSERRAT_MEDIUM[] = {
    #include "builtin_font_montserrat_medium.inc"
};

const uint8_t INCONSOLATA_BOLD[] = {
    #include "builtin_font_inconsolata_bold.inc"
};

// The names clients and saved configurations use for them: never renamed.
constexpr std::array<BuiltInAsset, 3> BUILT_IN_FONTS = { {
    { .name = "Rubik-Medium.ttf", .data = RUBIK_MEDIUM },
    { .name = "Montserrat-Medium.ttf", .data = MONTSERRAT_MEDIUM },
    { .name = "Inconsolata-Bold.ttf", .data = INCONSOLATA_BOLD },
} };

} // namespace

std::span<const BuiltInAsset> BuiltInFonts() {
    return BUILT_IN_FONTS;
}

#else

std::span<const BuiltInAsset> BuiltInFonts() {
    return {};
}

#endif

} // namespace eerie_leap::views::assets
