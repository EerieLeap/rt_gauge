#include <array>
#include <memory>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <eerie_memory.hpp>

#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/cbor/cbor_helpers.hpp"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "domain/ui_domain/models/widget_property.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/fs/services/fs_service.h"

using eerie_leap::domain::ui_domain::models::WidgetPropertyType;

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::device_tree;

// These storage tests use raw CBOR IDs to exercise key widths and value encodings.
// UiPropertyType validation is covered by the domain tests; no UI properties are defined yet.
ZTEST_SUITE(configuration_service_ui_config, NULL, NULL, NULL, NULL, NULL);

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_int_Save_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };
    std::vector<int32_t> values = {1, 2, 3, 4};

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
        property_value.CborPropertyValueType_m.value = values[i];
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 4);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
        zassert_equal(std::get<int32_t>(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value), values[i]);
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_double_Save_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };
    std::vector<double> values = {1.1, 2.2, 3.3, 4.4};

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_float_c;
        property_value.CborPropertyValueType_m.value = values[i];
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 4);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_float_c);
        zassert_equal(std::get<double>(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value), values[i]);
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_string_Save_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };
    std::vector<std::string> values = {"v_0", "v_1", "v_2", "v_3"};

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_c;
        property_value.CborPropertyValueType_m.value = CborHelpers::ToZcborString(values[i]);
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 4);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_tstr_c);
        zassert_equal(CborHelpers::ToStdString(
            std::get<zcbor_string>(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value)
        ), values[i]);
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_bool_Save_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };
    std::vector<bool> values = {true, false, true, false};

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_bool_c;
        property_value.CborPropertyValueType_m.value = values[i];
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 4);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_bool_c);
        zassert_equal(std::get<bool>(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value), values[i]);
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_int_list_Save_config_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };
    std::pmr::vector<std::pmr::vector<int32_t>> values = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}};

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_l_c;
        property_value.CborPropertyValueType_m.value = values[i];
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 4);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_l_c);
        zassert_equal(std::get<std::pmr::vector<int32_t>>(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value), values[i]);
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_string_list_Save_config_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };
    std::vector<std::vector<std::string>> values = {{"v_0", "v_1", "v_2"}, {"v_3", "v_4", "v_5"}, {"v_6", "v_7", "v_8"}, {"v_9", "v_10", "v_11"}};

    std::pmr::vector<std::pmr::vector<zcbor_string>> values_zcbor;
    values_zcbor.reserve(4);
    for(int i = 0; i < 4; i++) {
        std::pmr::vector<zcbor_string> value_zcbor;
        value_zcbor.reserve(3);
        for(int j = 0; j < 3; j++) {
            value_zcbor.push_back(CborHelpers::ToZcborString(values[i][j]));
        }
        values_zcbor.push_back(value_zcbor);
    }

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_l_c;
        property_value.CborPropertyValueType_m.value = values_zcbor[i];
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 4);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_tstr_l_c);

        for(int j = 0; j < 3; j++) {
            zassert_equal(
                CborHelpers::ToStdString(std::get<std::pmr::vector<zcbor_string>>(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value)[j]),
                values[i][j]);
        }
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_string_map_Save_config_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };

    std::vector<std::map<std::string, std::string>> values = {
        {{"k_0", "v_0"}, {"k_1", "v_1"}, {"k_2", "v_2"}},
        {{"k_3", "v_3"}, {"k_4", "v_4"}, {"k_5", "v_5"}},
        {{"k_6", "v_6"}, {"k_7", "v_7"}, {"k_8", "v_8"}},
        {{"k_9", "v_9"}, {"k_10", "v_10"}, {"k_11", "v_11"}}
    };

    std::pmr::vector<std::pmr::vector<map_tstrtstr>> values_zcbor;
    values_zcbor.reserve(4);
    for(int i = 0; i < 4; i++) {
        std::pmr::vector<map_tstrtstr> value_zcbor;
        value_zcbor.reserve(3);

        for(auto &item : values[i])
            value_zcbor.push_back({
                .tstrtstr_key = CborHelpers::ToZcborString(item.first),
                .tstrtstr = CborHelpers::ToZcborString(item.second)
            });

        values_zcbor.push_back(value_zcbor);
    }

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_map_c;
        property_value.CborPropertyValueType_m.value = values_zcbor[i];
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_map_c);

        for(int j = 0; j < 3; j++) {
            auto key = CborHelpers::ToStdString(
                std::get<std::pmr::vector<map_tstrtstr>>(
                    loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value)[j].tstrtstr_key);

            auto value = CborHelpers::ToStdString(
                std::get<std::pmr::vector<map_tstrtstr>>(
                    loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value)[j].tstrtstr);

            zassert_true(values[i].contains(key));
            zassert_equal(value, values[i][key]);
        }
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_CborPropertyValueType_string_all_mixed_Save_config_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 7> keys = { 0, 1, 23, 24, 255, 256, UINT32_MAX };
    int32_t uint32_t_value = 46;
    double double_value = 1.1;
    std::string string_value = "string";
    bool bool_value = true;

    std::pmr::vector<int32_t> int_vector_value = {1, 2, 3, 4, 5, 6, 7};

    std::vector<std::string> string_vector_value = {
        "string_0", "string_1", "string_2", "string_3", "string_4", "string_5", "string_6"
    };

    std::map<std::string, std::string> string_map_value = {
        {{"k_0", "v_0"}, {"k_1", "v_1"}, {"k_2", "v_2"}}
    };

    // int32_t
    CborPropertiesConfig_CborPropertyValueType_m property_value_int32_t(std::allocator_arg, Mrm::GetDefaultPmr());
    property_value_int32_t.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[0]);
    property_value_int32_t.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
    property_value_int32_t.CborPropertyValueType_m.value = uint32_t_value;
    ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value_int32_t));

    // double
    CborPropertiesConfig_CborPropertyValueType_m property_value_double(std::allocator_arg, Mrm::GetDefaultPmr());
    property_value_double.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[1]);
    property_value_double.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_float_c;
    property_value_double.CborPropertyValueType_m.value = double_value;
    ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value_double));

    // string
    CborPropertiesConfig_CborPropertyValueType_m property_value_string(std::allocator_arg, Mrm::GetDefaultPmr());
    property_value_string.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[2]);
    property_value_string.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_c;
    property_value_string.CborPropertyValueType_m.value = CborHelpers::ToZcborString(string_value);
    ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value_string));

    // bool
    CborPropertiesConfig_CborPropertyValueType_m property_value_bool(std::allocator_arg, Mrm::GetDefaultPmr());
    property_value_bool.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[3]);
    property_value_bool.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_bool_c;
    property_value_bool.CborPropertyValueType_m.value = bool_value;
    ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value_bool));

    // int vector
    CborPropertiesConfig_CborPropertyValueType_m property_value_int_vector(std::allocator_arg, Mrm::GetDefaultPmr());
    property_value_int_vector.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[4]);
    property_value_int_vector.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_l_c;
    property_value_int_vector.CborPropertyValueType_m.value = int_vector_value;
    ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value_int_vector));

    // string vector
    std::pmr::vector<zcbor_string> string_vector_value_zcbor;
    string_vector_value_zcbor.reserve(string_vector_value.size());
    for(int i = 0; i < string_vector_value.size(); i++)
        string_vector_value_zcbor.push_back(CborHelpers::ToZcborString(string_vector_value[i]));

    CborPropertiesConfig_CborPropertyValueType_m property_value_string_vector(std::allocator_arg, Mrm::GetDefaultPmr());
    property_value_string_vector.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[5]);
    property_value_string_vector.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_l_c;
    property_value_string_vector.CborPropertyValueType_m.value = string_vector_value_zcbor;
    ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value_string_vector));

    // string map
    std::pmr::vector<map_tstrtstr> string_map_value_zcbor;
    string_map_value_zcbor.reserve(string_map_value.size());
    for(auto &item : string_map_value) {
        string_map_value_zcbor.push_back({
            .tstrtstr_key = CborHelpers::ToZcborString(item.first),
            .tstrtstr = CborHelpers::ToZcborString(item.second)
        });
    }

    CborPropertiesConfig_CborPropertyValueType_m property_value_string_map(std::allocator_arg, Mrm::GetDefaultPmr());
    property_value_string_map.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[6]);
    property_value_string_map.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_map_c;
    property_value_string_map.CborPropertyValueType_m.value = string_map_value_zcbor;
    ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value_string_map));


    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 7);

    // int32_t
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[0].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[0]));
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[0].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
    zassert_equal(std::get<int32_t>(loaded_config.value().config->properties.CborPropertyValueType_m[0].CborPropertyValueType_m.value), uint32_t_value);

    // double
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[1].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[1]));
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[1].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_float_c);
    zassert_equal(std::get<double>(loaded_config.value().config->properties.CborPropertyValueType_m[1].CborPropertyValueType_m.value), double_value);

    // string
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[2].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[2]));
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[2].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_tstr_c);
    zassert_equal(CborHelpers::ToStdString(std::get<zcbor_string>(loaded_config.value().config->properties.CborPropertyValueType_m[2].CborPropertyValueType_m.value)), string_value);

    // bool
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[3].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[3]));
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[3].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_bool_c);
    zassert_equal(std::get<bool>(loaded_config.value().config->properties.CborPropertyValueType_m[3].CborPropertyValueType_m.value), bool_value);

    // int vector
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[4].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[4]));
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[4].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_l_c);
    zassert_equal(std::get<std::pmr::vector<int32_t>>(loaded_config.value().config->properties.CborPropertyValueType_m[4].CborPropertyValueType_m.value), int_vector_value);

    // string vector
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[5].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[5]));
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[5].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_tstr_l_c);
    for(int i = 0; i < string_vector_value.size(); i++)
        zassert_equal(
            CborHelpers::ToStdString(
                std::get<std::pmr::vector<zcbor_string>>(loaded_config.value().config->properties.CborPropertyValueType_m[5].CborPropertyValueType_m.value)[i]),
            string_vector_value[i]);

    // string map
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[6].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[6]));
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[6].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_map_c);
    for(int j = 0; j < 3; j++) {
        auto key = CborHelpers::ToStdString(
            std::get<std::pmr::vector<map_tstrtstr>>(
                loaded_config.value().config->properties.CborPropertyValueType_m[6].CborPropertyValueType_m.value)[j].tstrtstr_key);

        auto value = CborHelpers::ToStdString(
            std::get<std::pmr::vector<map_tstrtstr>>(
                loaded_config.value().config->properties.CborPropertyValueType_m[6].CborPropertyValueType_m.value)[j].tstrtstr);

        zassert_true(string_map_value.contains(key));
        zassert_equal(value, string_map_value[key]);
    }
}

ZTEST(configuration_service_ui_config, test_CborUiConfig_Save_successfully_saved_and_loaded) {
    auto ui_config = make_shared_pmr<CborUiConfig>(Mrm::GetDefaultPmr());

    ui_config->active_screen_group_id = 12;

    ui_config->properties_present = true;

    const std::array<uint32_t, 4> keys = { 1, 23, 24, UINT32_MAX };
    std::vector<int32_t> values = {1, 2, 3, 4};

    for(int i = 0; i < 4; i++) {
        CborPropertiesConfig_CborPropertyValueType_m property_value(std::allocator_arg, Mrm::GetDefaultPmr());

        property_value.CborPropertyValueType_m_key = static_cast<uint32_t>(keys[i]);
        property_value.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
        property_value.CborPropertyValueType_m.value = values[i];
        ui_config->properties.CborPropertyValueType_m.push_back(std::move(property_value));
    }

    // Create screen config
    CborScreenConfig screen(std::allocator_arg, Mrm::GetDefaultPmr());
    screen.id = 1;
    screen.group_id = 3;
    screen.type = 2;
    screen.z_index = -2;
    screen.is_visible = true;
    screen.grid.snap_enabled = true;
    screen.grid.width = 100;
    screen.grid.height = 100;
    screen.grid.spacing_px = 10;

    const std::array widget_keys = {
        WidgetPropertyType::MIN_VALUE, WidgetPropertyType::MAX_VALUE,
        WidgetPropertyType::STEP, WidgetPropertyType::WIDTH_PX
    };

    // First widget
    CborWidgetConfig widget1(std::allocator_arg, Mrm::GetDefaultPmr());
    widget1.type = 1;
    widget1.id = 1;
    widget1.position.x = 0;
    widget1.position.y = 0;
    widget1.size.width = 100;
    widget1.size.height = 100;
    widget1.z_index = 1;
    widget1.properties_present = true;

    CborPropertiesConfig_CborPropertyValueType_m prop1(std::allocator_arg, Mrm::GetDefaultPmr());
    prop1.CborPropertyValueType_m_key = static_cast<uint32_t>(widget_keys[0]);
    prop1.CborPropertyValueType_m.value = values[0];
    prop1.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
    widget1.properties.CborPropertyValueType_m.push_back(std::move(prop1));

    screen.CborWidgetConfig_m.push_back(std::move(widget1));

    // Second widget
    CborWidgetConfig widget2(std::allocator_arg, Mrm::GetDefaultPmr());
    widget2.type = 1;
    widget2.id = 2;
    widget2.position.x = 0;
    widget2.position.y = 0;
    widget2.size.width = 100;
    widget2.size.height = 100;
    widget2.z_index = -4;
    widget2.properties_present = true;

    CborPropertiesConfig_CborPropertyValueType_m prop2_1(std::allocator_arg, Mrm::GetDefaultPmr());
    prop2_1.CborPropertyValueType_m_key = static_cast<uint32_t>(widget_keys[0]);
    prop2_1.CborPropertyValueType_m.value = values[0];
    prop2_1.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
    widget2.properties.CborPropertyValueType_m.push_back(std::move(prop2_1));

    CborPropertiesConfig_CborPropertyValueType_m prop2_2(std::allocator_arg, Mrm::GetDefaultPmr());
    prop2_2.CborPropertyValueType_m_key = static_cast<uint32_t>(widget_keys[1]);
    prop2_2.CborPropertyValueType_m.value = values[1];
    prop2_2.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
    widget2.properties.CborPropertyValueType_m.push_back(std::move(prop2_2));

    CborPropertiesConfig_CborPropertyValueType_m prop2_3(std::allocator_arg, Mrm::GetDefaultPmr());
    prop2_3.CborPropertyValueType_m_key = static_cast<uint32_t>(widget_keys[2]);
    prop2_3.CborPropertyValueType_m.value = values[2];
    prop2_3.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
    widget2.properties.CborPropertyValueType_m.push_back(std::move(prop2_3));

    CborPropertiesConfig_CborPropertyValueType_m prop2_4(std::allocator_arg, Mrm::GetDefaultPmr());
    prop2_4.CborPropertyValueType_m_key = static_cast<uint32_t>(widget_keys[3]);
    prop2_4.CborPropertyValueType_m.value = values[3];
    prop2_4.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
    widget2.properties.CborPropertyValueType_m.push_back(std::move(prop2_4));

    screen.CborWidgetConfig_m.push_back(std::move(widget2));

    // Add screen to ui_config
    ui_config->CborScreenConfig_m.push_back(std::move(screen));

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();
    auto ui_config_service = std::make_shared<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);

    auto save_res = ui_config_service->Save(ui_config.get());
    zassert_true(save_res);

    auto loaded_config = ui_config_service->Load();

    zassert_true(loaded_config.has_value());
    zassert_equal(loaded_config.value().config->active_screen_group_id, 12);
    zassert_equal(loaded_config.value().config->properties_present, true);
    zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m.size(), 4);

    for(int i = 0; i < 4; i++) {
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m_key, static_cast<uint32_t>(keys[i]));
        zassert_equal(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
        zassert_equal(std::get<int32_t>(loaded_config.value().config->properties.CborPropertyValueType_m[i].CborPropertyValueType_m.value), values[i]);
    }

    zassert_equal(loaded_config.value().config->CborScreenConfig_m.size(), 1);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].id, 1);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].group_id, 3);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].type, 2);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].z_index, -2);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].is_visible, true);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].grid.snap_enabled, true);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].grid.width, 100);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].grid.height, 100);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].grid.spacing_px, 10);

    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m.size(), 2);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].type, 1);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].id, 1);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].position.x, 0);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].position.y, 0);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].size.width, 100);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].size.height, 100);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].z_index, 1);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].CborPropertyBinding_m.size(), 0U);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties_present, true);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties.CborPropertyValueType_m.size(), 1);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties.CborPropertyValueType_m[0].CborPropertyValueType_m_key, static_cast<uint32_t>(widget_keys[0]));
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties.CborPropertyValueType_m[0].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
    zassert_equal(std::get<int32_t>(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties.CborPropertyValueType_m[0].CborPropertyValueType_m.value), values[0]);

    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].type, 1);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].id, 2);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].position.x, 0);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].position.y, 0);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].size.width, 100);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].size.height, 100);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].z_index, -4);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].CborPropertyBinding_m.size(), 0U);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties_present, true);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m.size(), 4);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[0].CborPropertyValueType_m_key, static_cast<uint32_t>(widget_keys[0]));
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[0].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
    zassert_equal(std::get<int32_t>(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[0].CborPropertyValueType_m.value), values[0]);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[1].CborPropertyValueType_m_key, static_cast<uint32_t>(widget_keys[1]));
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[1].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
    zassert_equal(std::get<int32_t>(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[1].CborPropertyValueType_m.value), values[1]);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[2].CborPropertyValueType_m_key, static_cast<uint32_t>(widget_keys[2]));
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[2].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
    zassert_equal(std::get<int32_t>(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[2].CborPropertyValueType_m.value), values[2]);
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[3].CborPropertyValueType_m_key, static_cast<uint32_t>(widget_keys[3]));
    zassert_equal(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[3].CborPropertyValueType_m.CborPropertyValueType_choice, CborPropertyValueType_r::CborPropertyValueType_int_c);
    zassert_equal(std::get<int32_t>(loaded_config.value().config->CborScreenConfig_m[0].CborWidgetConfig_m[1].properties.CborPropertyValueType_m[3].CborPropertyValueType_m.value), values[3]);
}
