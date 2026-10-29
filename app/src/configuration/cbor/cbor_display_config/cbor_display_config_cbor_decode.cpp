/*
 * Generated using zcbor version 0.9.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 24
 */

#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include "zcbor_decode.h"
#include "cbor_display_config_cbor_decode.h"
#include "zcbor_print.h"

#define log_result(state, result, func) do { \
        if (!result) { \
                zcbor_trace_file(state); \
                zcbor_log("%s error: %s\r\n", func, zcbor_error_str(zcbor_peek_error(state))); \
        } else { \
                zcbor_log("%s success\r\n", func); \
        } \
} while(0)

static bool decode_CborDisplayConfig(zcbor_state_t *state, struct CborDisplayConfig *result);


static bool decode_CborDisplayConfig(
                zcbor_state_t *state, struct CborDisplayConfig *result)
{
        zcbor_log("%s\r\n", __func__);

        bool res = (((zcbor_list_start_decode(state) && ((((zcbor_uint32_decode(state, (&(*result).version))))
        && ((zcbor_uint32_decode(state, (&(*result).brightness))))
        && ((zcbor_bool_decode(state, (&(*result).blanking_enabled))))
        && ((zcbor_uint32_decode(state, (&(*result).screen_timeout_s))))
        && ((zcbor_uint32_decode(state, (&(*result).theme_id))))
        && ((zcbor_tstr_decode(state, (&(*result).brightness_sensor_id))))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_list_end_decode(state))));

        log_result(state, res, __func__);
        return res;
}



int cbor_decode_CborDisplayConfig(
                const uint8_t *payload, size_t payload_len,
                struct CborDisplayConfig *result,
                size_t *payload_len_out)
{
        zcbor_state_t states[3];

        return zcbor_entry_function(payload, payload_len, (void *)result, payload_len_out, states,
                (zcbor_decoder_t *)decode_CborDisplayConfig, sizeof(states) / sizeof(zcbor_state_t), 1);
}
