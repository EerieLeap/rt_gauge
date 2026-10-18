#include <configuration/cbor/cbor_ui_config/cbor_ui_config.h>
#include "configuration/cbor/cbor_trait.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_cbor_decode.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_cbor_encode.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_size.h"

namespace eerie_leap::configuration::cbor::traits {

static struct CborUiConfigTraitRegistrar {
    CborUiConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborUiConfig>(
            cbor_encode_CborUiConfig, cbor_decode_CborUiConfig, cbor_get_size_CborUiConfig
        );
    }
} CborUiConfigTraitRegistrar;

} // namespace eerie_leap::configuration::cbor::traits
