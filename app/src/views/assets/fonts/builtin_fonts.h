#pragma once

#include <span>

#include "subsys/assets/asset_provider.h"

namespace eerie_leap::views::assets {

/**
 * @brief The fonts compiled into flash, to register as the `fonts` asset kind's built-ins: Rubik-Medium.ttf,
 *        Montserrat-Medium.ttf and Inconsolata-Bold.ttf, Latin-1 subsets (U+0020-007E, U+00A0-00FF).
 *
 * Static storage, so the span and the bytes it points to are valid for the life of the program. Empty without
 * CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS.
 */
[[nodiscard]] std::span<const eerie_leap::subsys::assets::BuiltInAsset> BuiltInFonts();

} // namespace eerie_leap::views::assets
