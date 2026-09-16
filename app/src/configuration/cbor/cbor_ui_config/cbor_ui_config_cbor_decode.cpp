#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include "zcbor_decode.h"
#include "cbor_ui_config_cbor_decode.h"
#include "zcbor_print.h"

#define log_result(state, result, func) do { \
	if (!result) { \
		zcbor_trace_file(state); \
		zcbor_log("%s error: %s\r\n", func, zcbor_error_str(zcbor_peek_error(state))); \
	} else { \
		zcbor_log("%s success\r\n", func); \
	} \
} while(0)

static bool decode_repeated_map_tstrtstr(zcbor_state_t *state, struct map_tstrtstr *result);
static bool decode_CborPropertyValueType(zcbor_state_t *state, struct CborPropertyValueType_r *result);
static bool decode_repeated_CborPropertiesConfig_CborPropertyValueType_m(zcbor_state_t *state, struct CborPropertiesConfig_CborPropertyValueType_m *result);
static bool decode_CborPropertiesConfig(zcbor_state_t *state, struct CborPropertiesConfig *result);
static bool decode_CborGridSettingsConfig(zcbor_state_t *state, struct CborGridSettingsConfig *result);
static bool decode_CborWidgetPositionConfig(zcbor_state_t *state, struct CborWidgetPositionConfig *result);
static bool decode_CborWidgetSizeConfig(zcbor_state_t *state, struct CborWidgetSizeConfig *result);
static bool decode_CborPropertyBinding(zcbor_state_t *state, struct CborPropertyBinding *result);
static bool decode_CborWidgetConfig(zcbor_state_t *state, struct CborWidgetConfig *result);
static bool decode_CborScreenConfig(zcbor_state_t *state, struct CborScreenConfig *result);
static bool decode_CborUiConfig(zcbor_state_t *state, struct CborUiConfig *result);


static bool decode_repeated_map_tstrtstr(
		zcbor_state_t *state, struct map_tstrtstr *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = ((((zcbor_tstr_decode(state, (&(*result).tstrtstr_key))))
	&& (zcbor_tstr_decode(state, (&(*result).tstrtstr)))));

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborPropertyValueType(
		zcbor_state_t *state, struct CborPropertyValueType_r *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = false;

	// Try to decode each variant type in order
	// Save the current state to backtrack if needed
	zcbor_state_t backup_state = *state;

	// Try int32_t
	{
		int32_t val;
		if (zcbor_int32_decode(state, &val)) {
			result->value = val;
			result->CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
			res = true;
		}
	}

	if (!res) {
		*state = backup_state;
		// Try double
		double val;
		if (zcbor_float64_decode(state, &val)) {
			result->value = val;
			result->CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_float_c;
			res = true;
		}
	}

	if (!res) {
		*state = backup_state;
		// Try string
		zcbor_string val;
		if (zcbor_tstr_decode(state, &val)) {
			result->value = val;
			result->CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_c;
			res = true;
		}
	}

	if (!res) {
		*state = backup_state;
		// Try bool
		bool val;
		if (zcbor_bool_decode(state, &val)) {
			result->value = val;
			result->CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_bool_c;
			res = true;
		}
	}

	if (!res) {
		*state = backup_state;
		// Try list of int32_t
		if (zcbor_list_start_decode(state)) {
			std::pmr::vector<int32_t> vec(result->allocator);
			bool list_success = true;

			while (!zcbor_array_at_end(state)) {
				int32_t val;
				if (!zcbor_int32_decode(state, &val)) {
					list_success = false;
					zcbor_list_map_end_force_decode(state);
					break;
				}
				vec.push_back(val);
			}

			if (list_success && zcbor_list_end_decode(state)) {
				result->value = std::move(vec);
				result->CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_l_c;
				res = true;
			}
		}
	}

	if (!res) {
		*state = backup_state;
		// Try list of strings
		if (zcbor_list_start_decode(state)) {
			std::pmr::vector<zcbor_string> vec(result->allocator);
			bool list_success = true;

			while (!zcbor_array_at_end(state)) {
				zcbor_string val;
				if (!zcbor_tstr_decode(state, &val)) {
					list_success = false;
					zcbor_list_map_end_force_decode(state);
					break;
				}
				vec.push_back(val);
			}

			if (list_success && zcbor_list_end_decode(state)) {
				result->value = std::move(vec);
				result->CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_l_c;
				res = true;
			}
		}
	}

	if (!res) {
		*state = backup_state;
		// Try map (vector of map_tstrtstr)
		if (zcbor_map_start_decode(state)) {
			std::pmr::vector<map_tstrtstr> vec(result->allocator);
			bool map_success = true;

			while (!zcbor_array_at_end(state)) {
				map_tstrtstr val;
				if (!decode_repeated_map_tstrtstr(state, &val)) {
					map_success = false;
					zcbor_list_map_end_force_decode(state);
					break;
				}
				vec.push_back(val);
			}

			if (map_success && zcbor_map_end_decode(state)) {
				result->value = std::move(vec);
				result->CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_map_c;
				res = true;
			}
		}
	}

	log_result(state, res, __func__);
	return res;
}

static bool decode_repeated_CborPropertiesConfig_CborPropertyValueType_m(
		zcbor_state_t *state, struct CborPropertiesConfig_CborPropertyValueType_m *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = ((zcbor_uint32_decode(state, (&(*result).CborPropertyValueType_m_key))))
		&& (decode_CborPropertyValueType(state, (&(*result).CborPropertyValueType_m)));

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborPropertiesConfig(
		zcbor_state_t *state, struct CborPropertiesConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	if (!zcbor_map_start_decode(state)) {
		return false;
	}

	result->CborPropertyValueType_m.clear();

	while (!zcbor_array_at_end(state)) {
		result->CborPropertyValueType_m.emplace_back();
		if (!decode_repeated_CborPropertiesConfig_CborPropertyValueType_m(state, &result->CborPropertyValueType_m.back())) {
			result->CborPropertyValueType_m.pop_back();
			zcbor_list_map_end_force_decode(state);
			zcbor_map_end_decode(state);
			return false;
		}
	}

	if (!zcbor_map_end_decode(state)) {
		return false;
	}

	log_result(state, true, __func__);
	return true;
}

static bool decode_CborGridSettingsConfig(
		zcbor_state_t *state, struct CborGridSettingsConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_decode(state) && ((((zcbor_bool_decode(state, (&(*result).snap_enabled))))
	&& ((zcbor_uint32_decode(state, (&(*result).width))))
	&& ((zcbor_uint32_decode(state, (&(*result).height))))
	&& ((zcbor_uint32_decode(state, (&(*result).spacing_px))))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_list_end_decode(state))));

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborWidgetPositionConfig(
		zcbor_state_t *state, struct CborWidgetPositionConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_decode(state) && ((((zcbor_int32_decode(state, (&(*result).x))))
	&& ((zcbor_int32_decode(state, (&(*result).y))))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_list_end_decode(state))));

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborWidgetSizeConfig(
		zcbor_state_t *state, struct CborWidgetSizeConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_decode(state) && ((((zcbor_uint32_decode(state, (&(*result).width))))
	&& ((zcbor_uint32_decode(state, (&(*result).height))))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_list_end_decode(state))));

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborPropertyBinding(
		zcbor_state_t *state, struct CborPropertyBinding *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_decode(state) && ((((zcbor_uint32_decode(state, (&(*result).target))))
	&& ((zcbor_uint32_decode(state, (&(*result).channel))))
	&& ((zcbor_uint32_decode(state, (&(*result).event_type))))
	&& ((zcbor_uint32_decode(state, (&(*result).payload_key))))
	&& ((zcbor_uint32_decode(state, (&(*result).direction))))
	&& ((zcbor_uint32_decode(state, (&(*result).outbound_event_type))))
	&& ((zcbor_bool_decode(state, (&(*result).has_selector))))
	&& ((zcbor_uint32_decode(state, (&(*result).selector_key))))
	&& ((decode_CborPropertyValueType(state, (&(*result).selector_value))))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_list_end_decode(state))));

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborWidgetConfig(
		zcbor_state_t *state, struct CborWidgetConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	if (!zcbor_list_start_decode(state)) {
		return false;
	}

	if (!zcbor_uint32_decode(state, &result->type)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_uint32_decode(state, &result->id)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!decode_CborWidgetPositionConfig(state, &result->position)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!decode_CborWidgetSizeConfig(state, &result->size)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_int32_decode(state, &result->z_index)) {
		zcbor_list_end_decode(state);
		return false;
	}

	result->properties_present = decode_CborPropertiesConfig(state, &result->properties);

	if (!zcbor_list_start_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	result->CborPropertyBinding_m.clear();

	while (!zcbor_array_at_end(state)) {
		result->CborPropertyBinding_m.emplace_back();
		if (!decode_CborPropertyBinding(state, &result->CborPropertyBinding_m.back())) {
			result->CborPropertyBinding_m.pop_back();
			zcbor_list_map_end_force_decode(state);
			zcbor_list_end_decode(state);
			zcbor_list_end_decode(state);
			return false;
		}
	}

	if (!zcbor_list_end_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_list_end_decode(state)) {
		return false;
	}

	log_result(state, true, __func__);
	return true;
}

static bool decode_CborScreenConfig(
		zcbor_state_t *state, struct CborScreenConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	if (!zcbor_list_start_decode(state)) {
		return false;
	}

	if (!zcbor_uint32_decode(state, &result->id)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_uint32_decode(state, &result->group_id)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_uint32_decode(state, &result->type)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_int32_decode(state, &result->z_index)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_bool_decode(state, &result->is_visible)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_bool_decode(state, &result->is_overlay)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!decode_CborGridSettingsConfig(state, &result->grid)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_list_start_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	result->CborWidgetConfig_m.clear();

	while (!zcbor_array_at_end(state)) {
		result->CborWidgetConfig_m.emplace_back();
		if (!decode_CborWidgetConfig(state, &result->CborWidgetConfig_m.back())) {
			result->CborWidgetConfig_m.pop_back();
			zcbor_list_map_end_force_decode(state);
			zcbor_list_end_decode(state);
			zcbor_list_end_decode(state);
			return false;
		}
	}

	if (!zcbor_list_end_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_list_end_decode(state)) {
		return false;
	}

	log_result(state, true, __func__);
	return true;
}

static bool decode_CborUiConfig(
		zcbor_state_t *state, struct CborUiConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	if (!zcbor_list_start_decode(state)) {
		return false;
	}

	if (!zcbor_uint32_decode(state, &result->version)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_uint32_decode(state, &result->active_screen_group_id)) {
		zcbor_list_end_decode(state);
		return false;
	}

	result->properties_present = decode_CborPropertiesConfig(state, &result->properties);

	if (!zcbor_list_start_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	result->CborScreenConfig_m.clear();

	while (!zcbor_array_at_end(state)) {
		result->CborScreenConfig_m.emplace_back();
		if (!decode_CborScreenConfig(state, &result->CborScreenConfig_m.back())) {
			result->CborScreenConfig_m.pop_back();
			zcbor_list_map_end_force_decode(state);
			zcbor_list_end_decode(state);
			zcbor_list_end_decode(state);
			return false;
		}
	}

	if (!zcbor_list_end_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_list_end_decode(state)) {
		return false;
	}

	log_result(state, true, __func__);
	return true;
}



int cbor_decode_CborUiConfig(
		const uint8_t *payload, size_t payload_len,
		struct CborUiConfig *result,
		size_t *payload_len_out)
{
	zcbor_state_t states[10];

	return zcbor_entry_function(payload, payload_len, (void *)result, payload_len_out, states,
		(zcbor_decoder_t *)decode_CborUiConfig, sizeof(states) / sizeof(zcbor_state_t), 1);
}
