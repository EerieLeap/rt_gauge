#pragma once

#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <zcbor_common.h>

#include <vector>
#include <variant>
#include <memory_resource>

struct map_tstrtstr {
	struct zcbor_string tstrtstr_key;
	struct zcbor_string tstrtstr;
};

using CborPropertyValueType = std::variant<
    std::monostate,
	int32_t,
    double,
    zcbor_string,
    bool,
    std::pmr::vector<int32_t>,
    std::pmr::vector<zcbor_string>,
    std::pmr::vector<map_tstrtstr>
>;

struct CborPropertyValueType_r {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	CborPropertyValueType value{};

	enum {
		CborPropertyValueType_int_c,
		CborPropertyValueType_float_c,
		CborPropertyValueType_tstr_c,
		CborPropertyValueType_bool_c,
		CborPropertyValueType_int_l_c,
		CborPropertyValueType_tstr_l_c,
		CborPropertyValueType_map_c,
	} CborPropertyValueType_choice;

    allocator_type allocator;

	CborPropertyValueType_r(std::allocator_arg_t, allocator_type alloc)
		: allocator(alloc) {}

    CborPropertyValueType_r(const CborPropertyValueType_r&) = delete;
	CborPropertyValueType_r& operator=(const CborPropertyValueType_r&) noexcept = default;
	CborPropertyValueType_r& operator=(CborPropertyValueType_r&&) noexcept = default;
	CborPropertyValueType_r(CborPropertyValueType_r&&) noexcept = default;
	~CborPropertyValueType_r() = default;

	CborPropertyValueType_r(CborPropertyValueType_r&& other, allocator_type alloc)
        : value(std::move(other.value)),
        CborPropertyValueType_choice(other.CborPropertyValueType_choice),
        allocator(alloc) {}
};

struct CborPropertiesConfig_CborPropertyValueType_m {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	uint32_t CborPropertyValueType_m_key{};
	CborPropertyValueType_r CborPropertyValueType_m;

	CborPropertiesConfig_CborPropertyValueType_m(std::allocator_arg_t, allocator_type alloc)
		: CborPropertyValueType_m(std::allocator_arg, alloc) {}

    CborPropertiesConfig_CborPropertyValueType_m(const CborPropertiesConfig_CborPropertyValueType_m&) = delete;
	CborPropertiesConfig_CborPropertyValueType_m& operator=(const CborPropertiesConfig_CborPropertyValueType_m&) noexcept = default;
	CborPropertiesConfig_CborPropertyValueType_m& operator=(CborPropertiesConfig_CborPropertyValueType_m&&) noexcept = default;
	CborPropertiesConfig_CborPropertyValueType_m(CborPropertiesConfig_CborPropertyValueType_m&&) noexcept = default;
	~CborPropertiesConfig_CborPropertyValueType_m() = default;

	CborPropertiesConfig_CborPropertyValueType_m(CborPropertiesConfig_CborPropertyValueType_m&& other, allocator_type alloc)
        : CborPropertyValueType_m_key(std::move(other.CborPropertyValueType_m_key)),
		  CborPropertyValueType_m(std::move(other.CborPropertyValueType_m), alloc) {}
};

struct CborPropertiesConfig {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	std::pmr::vector<CborPropertiesConfig_CborPropertyValueType_m> CborPropertyValueType_m;
	allocator_type allocator;

	CborPropertiesConfig(std::allocator_arg_t, allocator_type alloc)
        : CborPropertyValueType_m(alloc),
        allocator(alloc) {}

    CborPropertiesConfig(const CborPropertiesConfig&) = delete;
	CborPropertiesConfig& operator=(const CborPropertiesConfig&) noexcept = default;
	CborPropertiesConfig& operator=(CborPropertiesConfig&&) noexcept = default;
	CborPropertiesConfig(CborPropertiesConfig&&) noexcept = default;
	~CborPropertiesConfig() = default;

	CborPropertiesConfig(CborPropertiesConfig&& other, allocator_type alloc)
        : CborPropertyValueType_m(std::move(other.CborPropertyValueType_m), alloc),
		  allocator(alloc) {}
};

struct CborGridSettingsConfig {
	bool snap_enabled{};
	uint32_t width{};
	uint32_t height{};
	uint32_t spacing_px{};
};

struct CborWidgetPositionConfig {
	int32_t x{};
	int32_t y{};
};

struct CborWidgetSizeConfig {
	uint32_t width{};
	uint32_t height{};
};

struct CborPropertyBinding {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	uint32_t target{};
	uint32_t channel{};
	uint32_t event_type{};
	uint32_t payload_key{};
	uint32_t direction{};
	uint32_t outbound_event_type{};
	bool has_selector{};
	uint32_t selector_key{};
	CborPropertyValueType_r selector_value;

	CborPropertyBinding(std::allocator_arg_t, allocator_type alloc)
		: selector_value(std::allocator_arg, alloc) {}

	CborPropertyBinding(const CborPropertyBinding&) = delete;
	CborPropertyBinding& operator=(const CborPropertyBinding&) noexcept = default;
	CborPropertyBinding& operator=(CborPropertyBinding&&) noexcept = default;
	CborPropertyBinding(CborPropertyBinding&&) noexcept = default;
	~CborPropertyBinding() = default;

	CborPropertyBinding(CborPropertyBinding&& other, allocator_type alloc)
		: target(other.target),
		channel(other.channel),
		event_type(other.event_type),
		payload_key(other.payload_key),
		direction(other.direction),
		outbound_event_type(other.outbound_event_type),
		has_selector(other.has_selector),
		selector_key(other.selector_key),
		selector_value(std::move(other.selector_value), alloc) {}
};

struct CborWidgetConfig {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	uint32_t type{};
	uint32_t id{};
	CborWidgetPositionConfig position{};
	CborWidgetSizeConfig size{};
	int32_t z_index{};
	CborPropertiesConfig properties;
	bool properties_present{};
	std::pmr::vector<CborPropertyBinding> CborPropertyBinding_m;

	CborWidgetConfig(std::allocator_arg_t, allocator_type alloc)
        : properties(std::allocator_arg, alloc), CborPropertyBinding_m(alloc) {}

    CborWidgetConfig(const CborWidgetConfig&) = delete;
	CborWidgetConfig& operator=(const CborWidgetConfig&) noexcept = default;
	CborWidgetConfig& operator=(CborWidgetConfig&&) noexcept = default;
	CborWidgetConfig(CborWidgetConfig&&) noexcept = default;
	~CborWidgetConfig() = default;

	CborWidgetConfig(CborWidgetConfig&& other, allocator_type alloc)
        : type(other.type),
		id(other.id),
		position(other.position),
		size(other.size),
		z_index(other.z_index),
		properties(std::move(other.properties), alloc),
		properties_present(other.properties_present),
		CborPropertyBinding_m(std::move(other.CborPropertyBinding_m), alloc) {}
};

struct CborScreenConfig {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	uint32_t id{};
	uint32_t group_id{};
    uint32_t type{};
	int32_t z_index{};
	bool is_visible{};
	bool is_overlay{};
	CborGridSettingsConfig grid{};
	std::pmr::vector<CborWidgetConfig> CborWidgetConfig_m;
	allocator_type allocator;

	CborScreenConfig(std::allocator_arg_t, allocator_type alloc)
        : CborWidgetConfig_m(alloc), allocator(alloc) {}

    CborScreenConfig(const CborScreenConfig&) = delete;
	CborScreenConfig& operator=(const CborScreenConfig&) noexcept = default;
	CborScreenConfig& operator=(CborScreenConfig&&) noexcept = default;
	CborScreenConfig(CborScreenConfig&&) noexcept = default;
	~CborScreenConfig() = default;

	CborScreenConfig(CborScreenConfig&& other, allocator_type alloc)
        : id(other.id),
		group_id(other.group_id),
		type(other.type),
		z_index(other.z_index),
		is_visible(other.is_visible),
		is_overlay(other.is_overlay),
		grid(other.grid),
		CborWidgetConfig_m(std::move(other.CborWidgetConfig_m), alloc),
		allocator(alloc) {}
};

struct CborUiConfig {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	uint32_t version{};
	uint32_t active_screen_group_id{};
	CborPropertiesConfig properties;
	bool properties_present{};
	std::pmr::vector<CborScreenConfig> CborScreenConfig_m;
	allocator_type allocator;

	CborUiConfig(std::allocator_arg_t, allocator_type alloc)
        : properties(std::allocator_arg, alloc),
		CborScreenConfig_m(alloc),
		allocator(alloc) {}

    CborUiConfig(const CborUiConfig&) = delete;
	CborUiConfig& operator=(const CborUiConfig&) noexcept = default;
	CborUiConfig& operator=(CborUiConfig&&) noexcept = default;
	CborUiConfig(CborUiConfig&&) noexcept = default;
	~CborUiConfig() = default;

	CborUiConfig(CborUiConfig&& other, allocator_type alloc)
        : version(other.version),
		active_screen_group_id(other.active_screen_group_id),
		properties(std::move(other.properties), alloc),
		properties_present(other.properties_present),
		CborScreenConfig_m(std::move(other.CborScreenConfig_m), alloc),
		allocator(alloc) {}
};
