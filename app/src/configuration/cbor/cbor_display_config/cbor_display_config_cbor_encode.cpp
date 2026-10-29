/*
 * Generated using zcbor version 0.9.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 24
 */

#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include "zcbor_encode.h"
#include "cbor_display_config_cbor_encode.h"
#include "zcbor_print.h"

#define log_result(state, result, func) do { \
        if (!result) { \
                zcbor_trace_file(state); \
                zcbor_log("%s error: %s\r\n", func, zcbor_error_str(zcbor_peek_error(state))); \
        } else { \
                zcbor_log("%s success\r\n", func); \
        } \
} while(0)

static bool encode_CborDisplayConfig(zcbor_state_t *state, const struct CborDisplayConfig *input);


static bool encode_CborDisplayConfig(
                zcbor_state_t *state, const struct CborDisplayConfig *input)
{
        zcbor_log("%s\r\n", __func__);

        bool res = (((zcbor_list_start_encode(state, 6) && ((((zcbor_uint32_encode(state, (&(*input).version))))
        && ((zcbor_uint32_encode(state, (&(*input).brightness))))
        && ((zcbor_bool_encode(state, (&(*input).blanking_enabled))))
        && ((zcbor_uint32_encode(state, (&(*input).screen_timeout_s))))
        && ((zcbor_uint32_encode(state, (&(*input).theme_id))))
        && ((zcbor_tstr_encode(state, (&(*input).brightness_sensor_id))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 6))));

        log_result(state, res, __func__);
        return res;
}



int cbor_encode_CborDisplayConfig(
                uint8_t *payload, size_t payload_len,
                const struct CborDisplayConfig *input,
                size_t *payload_len_out)
{
        zcbor_state_t states[3];

        return zcbor_entry_function(payload, payload_len, (void *)input, payload_len_out, states,
                (zcbor_decoder_t *)encode_CborDisplayConfig, sizeof(states) / sizeof(zcbor_state_t), 1);
}
