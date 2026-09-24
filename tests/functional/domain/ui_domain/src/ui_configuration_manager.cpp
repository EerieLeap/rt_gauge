#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <vector>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/ui_domain/configuration/ui_configuration_manager.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"
#include "domain/ui_domain/models/widget_property.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::domain::ui_domain::configuration;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::widgets::indicators;

ZTEST_SUITE(ui_configuration_manager, NULL, NULL, NULL, NULL, NULL);

std::shared_ptr<UiConfiguration> ui_configuration_manager_test_SetupTestUiConfiguration() {
    auto ui_configuration = make_shared_pmr<UiConfiguration>(Mrm::GetDefaultPmr());
    ui_configuration->active_screen_group_id = 3;

    auto screen_configuration = make_shared_pmr<ScreenConfiguration>(Mrm::GetDefaultPmr());
    screen_configuration->id = 8;
    screen_configuration->screen_group_id = 3;
    screen_configuration->type = ScreenType::Gauge;
    screen_configuration->z_index = -2;
    screen_configuration->is_visible = true;

    screen_configuration->grid.snap_enabled = true;
    screen_configuration->grid.width = 3;
    screen_configuration->grid.height = 3;
    screen_configuration->grid.spacing_px = 0;

    // First widget
    auto widget1 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget1->type = WidgetType::IndicatorArcFill;
    widget1->id = 0;
    widget1->position_grid.x = 0;
    widget1->position_grid.y = 0;
    widget1->size_grid.width = 3;
    widget1->size_grid.height = 3;
    widget1->z_index = -1;
    widget1->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget1->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget1->properties[WidgetPropertyType::LABEL] = "2348664336";

    PropertyBinding value_binding;
    value_binding.target = WidgetPropertyType::VALUE;
    value_binding.channel = EventChannelId::Sensors;
    value_binding.event_type = 0;
    value_binding.payload_key = 1;
    value_binding.selector_key = 0;
    value_binding.selector_value = std::pmr::string("2348664336");
    widget1->bindings.push_back(std::move(value_binding));

    PropertyBinding visibility_binding;
    visibility_binding.target = WidgetPropertyType::IS_VISIBLE;
    visibility_binding.channel = EventChannelId::Sensors;
    visibility_binding.event_type = 0;
    visibility_binding.payload_key = 1;
    visibility_binding.selector_key = 0;
    visibility_binding.selector_value = std::pmr::string("2348664336");
    widget1->bindings.push_back(std::move(visibility_binding));

    screen_configuration->AddWidget(std::move(widget1));

    // Second widget
    auto widget2 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget2->type = WidgetType::IndicatorDigital;
    widget2->id = 1;
    widget2->position_grid.x = 1;
    widget2->position_grid.y = 1;
    widget2->size_grid.width = 1;
    widget2->size_grid.height = 1;
    widget2->properties[WidgetPropertyType::IS_VISIBLE] = false;
    widget2->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget2->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget2->properties[WidgetPropertyType::LABEL] = "2348664336";

    PropertyBinding setting_binding;
    setting_binding.target = WidgetPropertyType::VALUE;
    setting_binding.channel = EventChannelId::Settings;
    setting_binding.event_type = 0;
    setting_binding.payload_key = 1;
    setting_binding.direction = PropertyBindingDirection::InOut;
    setting_binding.outbound_event_type = 2;
    widget2->bindings.push_back(std::move(setting_binding));

    screen_configuration->AddWidget(std::move(widget2));

    // Third widget
    auto widget3 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget3->type = WidgetType::IndicatorHorizontalChart;
    widget3->id = 2;
    widget3->position_grid.x = 0;
    widget3->position_grid.y = 0;
    widget3->size_grid.width = 3;
    widget3->size_grid.height = 1;
    widget3->z_index = 2;
    widget3->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget3->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget3->properties[WidgetPropertyType::LABEL] = "2348664336";
    widget3->properties[WidgetPropertyType::CHART_POINT_COUNT] = 35;
    widget3->properties[WidgetPropertyType::CHART_TYPE] = static_cast<std::uint16_t>(HorizontalChartIndicatorType::Line);
    screen_configuration->AddWidget(std::move(widget3));

    ui_configuration->screen_configurations.push_back(std::move(screen_configuration));

    return ui_configuration;
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_Save_config_successfully_saved) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_ui_configuration_service = std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::move(cbor_ui_configuration_service));

    auto ui_configuration = ui_configuration_manager_test_SetupTestUiConfiguration();
    bool result = ui_configuration_manager->Update(ui_configuration);
    zassert_true(result);
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_Save_config_and_Load) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_ui_configuration_service = std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::move(cbor_ui_configuration_service));

    auto ui_configuration = ui_configuration_manager_test_SetupTestUiConfiguration();
    bool result = ui_configuration_manager->Update(ui_configuration);
    zassert_true(result);

    auto saved_ui_configuration = ui_configuration_manager->Get(true);

    zassert_equal(saved_ui_configuration->active_screen_group_id, ui_configuration->active_screen_group_id);

    for(std::size_t i = 0; i < ui_configuration->screen_configurations.size(); i++) {
        zassert_equal(saved_ui_configuration->screen_configurations[i]->id, ui_configuration->screen_configurations[i]->id);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->screen_group_id, ui_configuration->screen_configurations[i]->screen_group_id);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->type, ui_configuration->screen_configurations[i]->type);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->z_index, ui_configuration->screen_configurations[i]->z_index);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->is_visible, ui_configuration->screen_configurations[i]->is_visible);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->grid.snap_enabled, ui_configuration->screen_configurations[i]->grid.snap_enabled);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->grid.width, ui_configuration->screen_configurations[i]->grid.width);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->grid.height, ui_configuration->screen_configurations[i]->grid.height);
        zassert_equal(saved_ui_configuration->screen_configurations[i]->grid.spacing_px, ui_configuration->screen_configurations[i]->grid.spacing_px);

        for(std::size_t j = 0; j < ui_configuration->screen_configurations[i]->widget_configurations.size(); j++) {
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->type, ui_configuration->screen_configurations[i]->widget_configurations[j]->type);
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->id, ui_configuration->screen_configurations[i]->widget_configurations[j]->id);
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.x, ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.x);
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.y, ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.y);
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.width, ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.width);
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.height, ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.height);
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->z_index, ui_configuration->screen_configurations[i]->widget_configurations[j]->z_index);
            zassert_equal(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->properties.size(), ui_configuration->screen_configurations[i]->widget_configurations[j]->properties.size());
            for(auto& property : ui_configuration->screen_configurations[i]->widget_configurations[j]->properties) {
                zassert_true(saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->properties[property.first] == ui_configuration->screen_configurations[i]->widget_configurations[j]->properties[property.first]);
            }

            const auto& bindings = ui_configuration->screen_configurations[i]->widget_configurations[j]->bindings;
            const auto& saved_bindings = saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->bindings;

            zassert_equal(saved_bindings.size(), bindings.size());

            for(std::size_t k = 0; k < bindings.size(); k++) {
                zassert_equal(saved_bindings[k].target, bindings[k].target);
                zassert_equal(saved_bindings[k].channel, bindings[k].channel);
                zassert_equal(saved_bindings[k].event_type, bindings[k].event_type);
                zassert_equal(saved_bindings[k].payload_key, bindings[k].payload_key);
                zassert_equal(saved_bindings[k].direction, bindings[k].direction);
                zassert_equal(saved_bindings[k].outbound_event_type, bindings[k].outbound_event_type);
                zassert_equal(saved_bindings[k].selector_key, bindings[k].selector_key);
                zassert_equal(saved_bindings[k].HasSelector(), bindings[k].HasSelector());
                zassert_true(saved_bindings[k].selector_value == bindings[k].selector_value);
            }
        }
    }
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_validates_once_per_boundary) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    size_t checks = 0;
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service),
        [&](const auto&, auto) { ++checks; });
    auto ui_configuration = ui_configuration_manager_test_SetupTestUiConfiguration();
    const auto widget_count = ui_configuration->screen_configurations[0]->widget_configurations.size();

    checks = 0;
    zassert_true(ui_configuration_manager->Update(ui_configuration));
    zassert_equal(checks, widget_count, "Saving validates once and keeps the configuration");
    zassert_equal(ui_configuration_manager->Get().get(), ui_configuration.get());

    const auto exported = ui_configuration_manager->GetCborConfiguration();
    checks = 0;
    zassert_true(ui_configuration_manager->ApplyCborConfiguration(exported));
    zassert_equal(checks, widget_count, "Importing validates once, while decoding");
    zassert_not_equal(ui_configuration_manager->Get().get(), ui_configuration.get());

    checks = 0;
    zassert_not_null(ui_configuration_manager->Get(true).get());
    zassert_equal(checks, widget_count, "Loading from storage validates once");
}

namespace {

std::shared_ptr<FsService> FormattedFs() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();
    return fs_service;
}

const std::pmr::vector<int>* ChildIds(const WidgetConfiguration& widget) {
    const auto it = widget.properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
    return it != widget.properties.end() ? std::get_if<std::pmr::vector<int>>(&it->second) : nullptr;
}

} // namespace

ZTEST(ui_configuration_manager, test_UiConfigurationManager_persists_ordered_child_ids_through_storage) {
    auto fs_service = FormattedFs();
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service));
    auto ui_configuration = ui_configuration_manager_test_SetupTestUiConfiguration();
    auto& widgets = ui_configuration->screen_configurations[0]->widget_configurations;
    // Widget 2 (z 2) comes before widget 1 (z 0), and ID order, in the owner's list.
    widgets[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 2, 1 }, Mrm::GetDefaultPmr());
    widgets[1]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>(Mrm::GetDefaultPmr());
    widgets[2]->properties[WidgetPropertyType::ANCHOR_POINT_X] = 7;
    widgets[2]->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
    zassert_true(ui_configuration_manager->Update(ui_configuration));

    auto loaded = ui_configuration_manager->Get(true);
    zassert_not_equal(loaded.get(), ui_configuration.get(), "Reloaded from storage");
    const auto& loaded_widgets = loaded->screen_configurations[0]->widget_configurations;
    zassert_equal(loaded_widgets.size(), 3);
    const auto* owner_ids = ChildIds(*loaded_widgets[0]);
    zassert_not_null(owner_ids);
    zassert_true(*owner_ids == std::pmr::vector<int>({ 2, 1 }));
    const auto* empty_ids = ChildIds(*loaded_widgets[1]);
    zassert_not_null(empty_ids, "An empty child list is stored, not dropped");
    zassert_true(empty_ids->empty());
    zassert_equal(std::get<int>(loaded_widgets[2]->properties.at(WidgetPropertyType::ANCHOR_POINT_X)), 7);
    zassert_equal(std::get<int>(loaded_widgets[2]->properties.at(WidgetPropertyType::ANCHOR_POINT_Y)), 7);
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_rejects_invalid_compositions_before_persisting) {
    auto fs_service = FormattedFs();
    bool reject_children = false;
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service),
        [&](const auto&, auto children) {
            if(reject_children && !children.empty())
                throw std::invalid_argument("This owner accepts no children.");
        });

    auto composed = ui_configuration_manager_test_SetupTestUiConfiguration();
    composed->screen_configurations[0]->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] =
        std::pmr::vector<int>({ 1 }, Mrm::GetDefaultPmr());
    zassert_true(ui_configuration_manager->Update(composed));
    const auto exported = ui_configuration_manager->GetCborConfiguration();
    auto plain = ui_configuration_manager_test_SetupTestUiConfiguration();
    zassert_true(ui_configuration_manager->Update(plain));

    auto expect_plain_stored = [&](const char* mode) {
        zassert_equal(ui_configuration_manager->Get().get(), plain.get(), "%s", mode);
        auto stored = ui_configuration_manager->Get(true);
        zassert_is_null(ChildIds(*stored->screen_configurations[0]->widget_configurations[0]), "%s", mode);
        plain = stored;
    };

    reject_children = true;
    zassert_false(ui_configuration_manager->Update(composed));
    expect_plain_stored("Rejected save");
    zassert_false(ui_configuration_manager->ApplyCborConfiguration(exported));
    expect_plain_stored("Rejected import");

    reject_children = false;
    auto missing = ui_configuration_manager_test_SetupTestUiConfiguration();
    missing->screen_configurations[0]->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] =
        std::pmr::vector<int>({ 99 }, Mrm::GetDefaultPmr());
    zassert_false(ui_configuration_manager->Update(missing), "Graph checks run without widget rules too");
    expect_plain_stored("Missing child reference");
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_falls_back_when_the_stored_version_is_stale) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    // A payload from before the schema bump: structurally valid, but unreadable now.
    auto stale_config = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    stale_config->version = UiConfigurationCborParser::configuration_version - 1;
    stale_config->active_screen_group_id = 3;

    auto writer_service = std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);
    zassert_true(writer_service->Save(stale_config.get()));

    auto cbor_ui_configuration_service = std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::move(cbor_ui_configuration_service));

    // The default configuration rather than a crash or a half-decoded one.
    auto configuration = ui_configuration_manager->Get();

    zassert_not_null(configuration.get());
    zassert_true(configuration->screen_configurations.empty());
}
