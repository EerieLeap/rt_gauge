#pragma once

#include <memory>

#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"

namespace eerie_leap::domain::ui_domain::images {

/**
 * @brief The `images` kind as the unit serves it: raw `.bin` pixels, `.svg` documents and `.lvimg` files that
 *        describe themselves, all kept in @p store.
 *
 * The kind's lists and checks of `.lvimg` files are ParseImageFileHead()'s, judged against this build's rules, so
 * the upload check, the listing and the loader that reads the files agree on what a valid file is. The `.bin` and
 * `.svg` files are not checked: a client's raw pixels have no header to judge, and the icon that draws an
 * `.svg` refuses a document it can't parse.
 */
eerie_leap::subsys::assets::AssetProvider
MakeImageAssetProvider(std::shared_ptr<eerie_leap::subsys::assets::AssetStore> store);

} // namespace eerie_leap::domain::ui_domain::images
