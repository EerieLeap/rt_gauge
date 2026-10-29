#include <algorithm>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/ui_domain/models/widget_property.h"

#include "views/widgets/text_font.h"

#include "button_control.h"

namespace eerie_leap::views::widgets::controls {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using namespace eerie_leap::views::themes;

LOG_MODULE_REGISTER(button_control_logger);

// A button carries no setting, so an unset target means "no navigation".
static constexpr int32_t NO_TARGET = -1;

ButtonControl::ButtonControl(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context) :
    ControlBase(id, std::move(parent), std::move(context), false) {}

ButtonControl::~ButtonControl() {
    // A teardown ends a press: its release goes out from OnProcessingSuspended(), which only reaches this class
    // while it still stands.
    DetachDispatch();
}

void ButtonControl::RegisterProperties(WidgetPropertyStore& store) const {
    ControlBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_INACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_SECONDARY_ACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_SECONDARY_INACTIVE);

    store.Register(WidgetPropertyType::LABEL, ConfigValue{ std::pmr::string{} }, PropertyChangeEffect::Rebuild);
    TextFont::RegisterProperties(store, 20);
    store.Register(WidgetPropertyType::TARGET_SCREEN_GROUP, ConfigValue{ NO_TARGET }, PropertyChangeEffect::None);
    store.Register(
        WidgetPropertyType::NAVIGATION_INTENT, ConfigValue{ static_cast<int>(NavigationIntent::None) },
        PropertyChangeEffect::None
    );
}

void ButtonControl::OnConfigured() {
    // LVGL reports a press at touch-down, before it can tell a tap from a swipe, so a press is only safe to send
    // where no swipe can start: a button that sends one owns its touch area, as a slider owns its drag.
    const bool is_sending_press = std::ranges::any_of(configuration_->bindings, [](const auto& binding) {
        return binding.target == WidgetPropertyType::VALUE && binding.direction != PropertyBindingDirection::In;
    });

    if(is_sending_press)
        CaptureGestures();
}

int ButtonControl::ApplyTheme(const ITheme& theme) {
    // A checked button is drawn as a pressed one is: the same active pair, and none of the darkening LVGL's theme
    // lays over a press.
    for(const auto state : { LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_CHECKED }) {
        const bool is_active = state != LV_STATE_DEFAULT;
        const auto selector = static_cast<lv_style_selector_t>(LV_PART_MAIN) | state;
        const auto primary = properties_->ResolveColor(
            is_active ? WidgetPropertyType::COLOR_PRIMARY_ACTIVE : WidgetPropertyType::COLOR_PRIMARY_INACTIVE,
            is_active ? theme.GetAccentColor() : theme.GetSurfaceColor()
        );
        const auto secondary = properties_->ResolveColor(
            is_active ? WidgetPropertyType::COLOR_SECONDARY_ACTIVE : WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
            theme.GetPrimaryColor()
        );

        if(is_active)
            lv_obj_set_style_recolor_opa(lv_button_, LV_OPA_TRANSP, selector);

        lv_obj_set_style_bg_color(lv_button_, primary.ToLvColor(), selector);
        lv_obj_set_style_bg_opa(lv_button_, primary.ToLvOpa(), selector);
        lv_obj_set_style_text_color(lv_label_, secondary.ToLvColor(), selector);
        lv_obj_set_style_text_opa(lv_label_, secondary.ToLvOpa(), selector);
    }

    lv_obj_set_style_text_font(lv_label_, font_.value_or(theme.GetPrimaryFont()).ToLvFont(), LV_PART_MAIN);
    lv_obj_set_state(lv_label_, LV_STATE_PRESSED, lv_obj_has_state(lv_button_, LV_STATE_PRESSED));

    return 0;
}

void ButtonControl::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::LABEL:
            label_ = ConfigValueAs<std::pmr::string>(value, "");
            break;

        case WidgetPropertyType::TARGET_SCREEN_GROUP: {
            auto target = ConfigValueAs<int>(value, NO_TARGET);

            if(target >= 0)
                target_id_ = static_cast<uint32_t>(target);

            break;
        }

        case WidgetPropertyType::NAVIGATION_INTENT:
            intent_ = static_cast<NavigationIntent>(
                ConfigValueAs<int>(value, static_cast<int>(NavigationIntent::None))
            );
            break;

        case WidgetPropertyType::VALUE:
            ControlBase::OnPropertyChanged(type, value);
            UpdateChecked();
            break;

        default:
            ControlBase::OnPropertyChanged(type, value);
            break;
    }
}

int ButtonControl::DoRender() {
    font_ = TextFont::Acquire(*properties_, context_.font_cache);

    lv_button_ = lv_button_create(content_frame_->GetObject());

    lv_obj_set_size(lv_button_, lv_pct(100), lv_pct(100));
    lv_obj_center(lv_button_);

    lv_label_ = lv_label_create(lv_button_);
    if(font_)
        font_->BindTo(lv_label_);
    lv_label_set_text(lv_label_, label_.c_str());
    lv_obj_center(lv_label_);

    AttachEvents(lv_button_, { LV_EVENT_CLICKED, LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST });

    UpdateChecked();

    content_frame_->SetChild(std::make_shared<Frame>(Frame::Create(lv_button_).Build()));

    return 0;
}

void ButtonControl::OnProcessingSuspended() {
    ControlBase::OnProcessingSuspended();
    if(lv_label_ != nullptr)
        lv_obj_remove_state(lv_label_, LV_STATE_PRESSED);

    // A press that is cut short still owes its release.
    PublishPress(false);
}

void ButtonControl::OnControlEvent(lv_event_code_t code) {
    if(code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        const bool is_pressed = lv_obj_has_state(lv_button_, LV_STATE_PRESSED);

        lv_obj_set_state(lv_label_, LV_STATE_PRESSED, is_pressed);
        PublishPress(is_pressed);
    }

    if(code == LV_EVENT_CLICKED)
        Navigate();
}

// The contact a physical button gives: each change goes out once.
void ButtonControl::PublishPress(bool is_pressed) {
    if(std::exchange(is_press_published_, is_pressed) == is_pressed)
        return;

    PublishPropertyLocal(WidgetPropertyType::VALUE, ConfigValue{ is_pressed ? 1.0 : 0.0 });
}

// The button shows what a binding wrote to VALUE; it never writes it itself, so a press that does not take leaves
// it as it was.
void ButtonControl::UpdateChecked() {
    if(lv_button_ == nullptr)
        return;

    const bool is_checked = properties_->GetAs<double>(WidgetPropertyType::VALUE, 0.0) != 0.0;

    lv_obj_set_state(lv_button_, LV_STATE_CHECKED, is_checked);
    lv_obj_set_state(lv_label_, LV_STATE_CHECKED, is_checked);
}

void ButtonControl::Navigate() {
    if(context_.navigation_service == nullptr)
        return;

    auto navigation = ResolveNavigation();
    if(!navigation.has_value())
        return;

    // Publishes an event; the view change itself happens on the event bus thread.
    if(context_.navigation_service->Handle(navigation->first, navigation->second) != 0)
        LOG_WRN("Widget %u failed navigation intent %u.", id_, static_cast<uint32_t>(navigation->first));
}

std::optional<std::pair<NavigationIntent, uint32_t>> ButtonControl::ResolveNavigation() const {
    switch(intent_) {
        case NavigationIntent::GoToGroup:
        case NavigationIntent::ShowOverlay:
            if(target_id_.has_value())
                return std::pair{ intent_, *target_id_ };

            return std::nullopt;

        case NavigationIntent::None:
            return std::nullopt;

        default:
            return std::pair{ intent_, uint32_t{ 0 } };
    }
}

} // namespace eerie_leap::views::widgets::controls
