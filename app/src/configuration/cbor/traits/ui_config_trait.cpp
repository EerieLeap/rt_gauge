#include <configuration/cbor/cbor_ui_config/cbor_ui_config.h>
#include "configuration/cbor/cbor_trait.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_cbor_decode.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_cbor_encode.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_size.h"

namespace eerie_leap::configuration::cbor::traits {

namespace {

struct CborUiConfigTraitRegistrar {
    CborUiConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborUiConfig>(
            cbor_encode_CborUiConfig, cbor_decode_CborUiConfig, cbor_get_size_CborUiConfig
        );
    }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the registry insert can only throw std::bad_alloc, fatal before main
const CborUiConfigTraitRegistrar CBOR_UI_CONFIG_TRAIT_REGISTRAR;

} // namespace

} // namespace eerie_leap::configuration::cbor::traits
