#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include "zcbor_encode.h"
#include "cbor_ui_config_cbor_encode.h"
#include "zcbor_print.h"

#define log_result(state, result, func) do { \
	if (!result) { \
		zcbor_trace_file(state); \
		zcbor_log("%s error: %s\r\n", func, zcbor_error_str(zcbor_peek_error(state))); \
	} else { \
		zcbor_log("%s success\r\n", func); \
	} \
} while(0)

static bool encode_repeated_map_tstrtstr(zcbor_state_t *state, const struct map_tstrtstr *input);
static bool encode_CborPropertyValueType(zcbor_state_t *state, const struct CborPropertyValueType_r *input);
static bool encode_repeated_CborPropertiesConfig_CborPropertyValueType_m(zcbor_state_t *state, const struct CborPropertiesConfig_CborPropertyValueType_m *input);
static bool encode_CborPropertiesConfig(zcbor_state_t *state, const struct CborPropertiesConfig *input);
static bool encode_CborGridSettingsConfig(zcbor_state_t *state, const struct CborGridSettingsConfig *input);
static bool encode_CborWidgetPositionConfig(zcbor_state_t *state, const struct CborWidgetPositionConfig *input);
static bool encode_CborWidgetSizeConfig(zcbor_state_t *state, const struct CborWidgetSizeConfig *input);
static bool encode_CborPropertyBinding(zcbor_state_t *state, const struct CborPropertyBinding *input);
static bool encode_CborWidgetConfig(zcbor_state_t *state, const struct CborWidgetConfig *input);
static bool encode_CborScreenConfig(zcbor_state_t *state, const struct CborScreenConfig *input);
static bool encode_CborUiConfig(zcbor_state_t *state, const struct CborUiConfig *input);

static bool encode_repeated_map_tstrtstr(
		zcbor_state_t *state, const struct map_tstrtstr *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = ((((zcbor_tstr_encode(state, (&(*input).tstrtstr_key))))
	&& (zcbor_tstr_encode(state, (&(*input).tstrtstr)))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborPropertyValueType(
		zcbor_state_t *state, const struct CborPropertyValueType_r *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = ((*input).CborPropertyValueType_choice == CborPropertyValueType_r::CborPropertyValueType_int_c) ? ((zcbor_int32_encode(state, &std::get<int32_t>(input->value))))
		: (((*input).CborPropertyValueType_choice == CborPropertyValueType_r::CborPropertyValueType_float_c) ? ((zcbor_float64_encode(state, &std::get<double>(input->value))))
		: (((*input).CborPropertyValueType_choice == CborPropertyValueType_r::CborPropertyValueType_tstr_c) ? ((zcbor_tstr_encode(state, &std::get<zcbor_string>(input->value))))
		: (((*input).CborPropertyValueType_choice == CborPropertyValueType_r::CborPropertyValueType_bool_c) ? ((zcbor_bool_encode(state, &std::get<bool>(input->value))))
		: (((*input).CborPropertyValueType_choice == CborPropertyValueType_r::CborPropertyValueType_int_l_c) ? ((zcbor_list_start_encode(state, 0) && ((zcbor_multi_encode(std::get<std::pmr::vector<int32_t>>(input->value).size(), (zcbor_encoder_t *)zcbor_int32_encode, state, std::get<std::pmr::vector<int32_t>>(input->value).data(), sizeof(int32_t))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 0)))
		: (((*input).CborPropertyValueType_choice == CborPropertyValueType_r::CborPropertyValueType_tstr_l_c) ? ((zcbor_list_start_encode(state, 0) && ((zcbor_multi_encode(std::get<std::pmr::vector<zcbor_string>>(input->value).size(), (zcbor_encoder_t *)zcbor_tstr_encode, state, std::get<std::pmr::vector<zcbor_string>>(input->value).data(), sizeof(zcbor_string))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 0)))
		: (((*input).CborPropertyValueType_choice == CborPropertyValueType_r::CborPropertyValueType_map_c) ? ((zcbor_map_start_encode(state, 0) && ((zcbor_multi_encode(std::get<std::pmr::vector<map_tstrtstr>>(input->value).size(), (zcbor_encoder_t *)encode_repeated_map_tstrtstr, state, std::get<std::pmr::vector<map_tstrtstr>>(input->value).data(), sizeof(map_tstrtstr))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 0)))
		: false))))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_repeated_CborPropertiesConfig_CborPropertyValueType_m(
		zcbor_state_t *state, const struct CborPropertiesConfig_CborPropertyValueType_m *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = ((((zcbor_uint32_encode(state, (&(*input).CborPropertyValueType_m_key))))
	&& (encode_CborPropertyValueType(state, (&(*input).CborPropertyValueType_m)))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborPropertiesConfig(
		zcbor_state_t *state, const struct CborPropertiesConfig *input)
{
	zcbor_log("%s\r\n", __func__);

    size_t CborPropertyValueType_m_count = input->CborPropertyValueType_m.size();

	bool res = (((zcbor_map_start_encode(state, CborPropertyValueType_m_count) && ((zcbor_multi_encode(CborPropertyValueType_m_count, (zcbor_encoder_t *)encode_repeated_CborPropertiesConfig_CborPropertyValueType_m, state, input->CborPropertyValueType_m.data(), sizeof(struct CborPropertiesConfig_CborPropertyValueType_m))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, CborPropertyValueType_m_count))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborGridSettingsConfig(
		zcbor_state_t *state, const struct CborGridSettingsConfig *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_encode(state, 4) && ((((zcbor_bool_encode(state, (&(*input).snap_enabled))))
	&& ((zcbor_uint32_encode(state, (&(*input).width))))
	&& ((zcbor_uint32_encode(state, (&(*input).height))))
	&& ((zcbor_uint32_encode(state, (&(*input).spacing_px))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 4))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborWidgetPositionConfig(
		zcbor_state_t *state, const struct CborWidgetPositionConfig *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_encode(state, 2) && ((((zcbor_int32_encode(state, (&(*input).x))))
	&& ((zcbor_int32_encode(state, (&(*input).y))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 2))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborWidgetSizeConfig(
		zcbor_state_t *state, const struct CborWidgetSizeConfig *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_encode(state, 2) && ((((zcbor_uint32_encode(state, (&(*input).width))))
	&& ((zcbor_uint32_encode(state, (&(*input).height))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 2))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborPropertyBinding(
		zcbor_state_t *state, const struct CborPropertyBinding *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_encode(state, 9) && ((((zcbor_uint32_encode(state, (&(*input).target))))
	&& ((zcbor_uint32_encode(state, (&(*input).channel))))
	&& ((zcbor_uint32_encode(state, (&(*input).event_type))))
	&& ((zcbor_uint32_encode(state, (&(*input).payload_key))))
	&& ((zcbor_uint32_encode(state, (&(*input).direction))))
	&& ((zcbor_uint32_encode(state, (&(*input).outbound_event_type))))
	&& ((zcbor_bool_encode(state, (&(*input).has_selector))))
	&& ((zcbor_uint32_encode(state, (&(*input).selector_key))))
	&& ((encode_CborPropertyValueType(state, (&(*input).selector_value))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 9))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborWidgetConfig(
		zcbor_state_t *state, const struct CborWidgetConfig *input)
{
	zcbor_log("%s\r\n", __func__);

	size_t CborPropertyBinding_m_count = input->CborPropertyBinding_m.size();

	bool res = (((zcbor_list_start_encode(state, 7) && ((((zcbor_uint32_encode(state, (&(*input).type))))
	&& ((zcbor_uint32_encode(state, (&(*input).id))))
	&& ((encode_CborWidgetPositionConfig(state, (&(*input).position))))
	&& ((encode_CborWidgetSizeConfig(state, (&(*input).size))))
	&& ((zcbor_int32_encode(state, (&(*input).z_index))))
	&& (!(*input).properties_present || encode_CborPropertiesConfig(state, (&(*input).properties)))
	&& ((zcbor_list_start_encode(state, CborPropertyBinding_m_count) && ((zcbor_multi_encode(CborPropertyBinding_m_count, (zcbor_encoder_t *)encode_CborPropertyBinding, state, input->CborPropertyBinding_m.data(), sizeof(struct CborPropertyBinding))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, CborPropertyBinding_m_count)))
	) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 7))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborScreenConfig(
		zcbor_state_t *state, const struct CborScreenConfig *input)
{
	zcbor_log("%s\r\n", __func__);

    size_t CborWidgetConfig_m_count = input->CborWidgetConfig_m.size();

	bool res = (((zcbor_list_start_encode(state, 8) && ((
       ((zcbor_uint32_encode(state, (&(*input).id))))
    && ((zcbor_uint32_encode(state, (&(*input).group_id))))
    && ((zcbor_uint32_encode(state, (&(*input).type))))
	&& ((zcbor_int32_encode(state, (&(*input).z_index))))
	&& ((zcbor_bool_encode(state, (&(*input).is_visible))))
	&& ((zcbor_bool_encode(state, (&(*input).is_overlay))))
	&& ((encode_CborGridSettingsConfig(state, (&(*input).grid))))
	&& ((zcbor_list_start_encode(state, CborWidgetConfig_m_count) && ((zcbor_multi_encode(CborWidgetConfig_m_count, (zcbor_encoder_t *)encode_CborWidgetConfig, state, input->CborWidgetConfig_m.data(), sizeof(struct CborWidgetConfig))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, CborWidgetConfig_m_count)))
	) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 8))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborUiConfig(
		zcbor_state_t *state, const struct CborUiConfig *input)
{
	zcbor_log("%s\r\n", __func__);

    size_t CborScreenConfig_m_count = input->CborScreenConfig_m.size();

	bool res = (((zcbor_list_start_encode(state, 4) && ((((zcbor_uint32_encode(state, (&(*input).version))))
	&& ((zcbor_uint32_encode(state, (&(*input).active_screen_group_id))))
	&& (!(*input).properties_present || encode_CborPropertiesConfig(state, (&(*input).properties)))
	&& ((zcbor_list_start_encode(state, CborScreenConfig_m_count) && ((zcbor_multi_encode(CborScreenConfig_m_count, (zcbor_encoder_t *)encode_CborScreenConfig, state, input->CborScreenConfig_m.data(), sizeof(struct CborScreenConfig))
	) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, CborScreenConfig_m_count)))
	) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 4))));

	log_result(state, res, __func__);
	return res;
}



int cbor_encode_CborUiConfig(
		uint8_t *payload, size_t payload_len,
		const struct CborUiConfig *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[10];

	return zcbor_entry_function(payload, payload_len, (void *)input, payload_len_out, states,
		(zcbor_decoder_t *)encode_CborUiConfig, sizeof(states) / sizeof(zcbor_state_t), 1);
}
