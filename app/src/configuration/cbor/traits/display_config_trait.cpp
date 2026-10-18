#include <configuration/cbor/cbor_display_config/cbor_display_config.h>
#include "configuration/cbor/cbor_display_config/cbor_display_config_cbor_decode.h"
#include "configuration/cbor/cbor_display_config/cbor_display_config_cbor_encode.h"
#include "configuration/cbor/cbor_display_config/cbor_display_config_size.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor::traits {

struct CborDisplayConfigTraitRegistrar {
    CborDisplayConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborDisplayConfig>(
            cbor_encode_CborDisplayConfig, cbor_decode_CborDisplayConfig, cbor_get_size_CborDisplayConfig
        );
    }
} CborDisplayConfigTraitRegistrar;

} // namespace eerie_leap::configuration::cbor::traits
