#include <zephyr/kernel.h>

#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW

    #include <array>
    #include <cstring>
    #include <utility>

    #include <zephyr/logging/log.h>

    #include "ui_mgmt_group.h"

LOG_MODULE_REGISTER(ui_mgmt_group, LOG_LEVEL_INF);

namespace eerie_leap::domain::ui_preview_domain::smp {

using eerie_leap::subsys::smp::SmpAddGroupError;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

namespace {

// SHOW's `target`: a map of one key, `group` or `overlay`.
struct TargetField {
    bool is_overlay = false;
    uint32_t id = 0;
};

bool DecodeTarget(zcbor_state_t* zsd, TargetField* target) {
    // Read by hand, one key and its value: a bulk decode inside the request's own bulk decode backs up more
    // state than a request has.
    zcbor_string key{};

    if(!zcbor_map_start_decode(zsd) || !zcbor_tstr_decode(zsd, &key) || !zcbor_uint32_decode(zsd, &target->id))
        return false;

    if(!zcbor_map_end_decode(zsd))
        return false;

    if(key.len == sizeof("group") - 1 && std::memcmp(key.value, "group", key.len) == 0)
        target->is_overlay = false;
    else if(key.len == sizeof("overlay") - 1 && std::memcmp(key.value, "overlay", key.len) == 0)
        target->is_overlay = true;
    else
        return false;

    return true;
}

} // namespace

const mgmt_handler UiMgmtGroup::HANDLERS[] = {
    { .mh_read = nullptr, .mh_write = Dispatch<&UiMgmtGroup::HandleBegin> },
    { .mh_read = nullptr, .mh_write = Dispatch<&UiMgmtGroup::HandleStage> },
    // PATCH: FW-2.
    { .mh_read = nullptr, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&UiMgmtGroup::HandleShow> },
    { .mh_read = nullptr, .mh_write = Dispatch<&UiMgmtGroup::HandleHighlight> },
    { .mh_read = nullptr, .mh_write = Dispatch<&UiMgmtGroup::HandleEnd> },
    { .mh_read = Dispatch<&UiMgmtGroup::HandleStatus>, .mh_write = nullptr },
};

UiMgmtGroup::UiMgmtGroup(std::shared_ptr<UiPreviewService> preview_service) :
    SmpGroup(SmpGroupId::UI, HANDLERS, TranslateError),
    preview_service_(std::move(preview_service)) {}

int UiMgmtGroup::HandleBegin(smp_streamer* ctxt) {
    uint32_t token = 0;
    bool is_pinned = true;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("pin", zcbor_bool_decode, &is_pinned),
    };

    if(!SmpDecodeRequest(ctxt, keys))
        return MGMT_ERR_EINVAL;

    UiPreviewService::BeginInfo info;
    const auto error = preview_service_->Begin(
        keys[0].found ? std::optional<uint32_t>(token) : std::nullopt, is_pinned, info
    );
    if(error != UiPreviewError::OK)
        return Fail(ctxt, error);

    zcbor_state_t* zse = ctxt->writer->zs;
    const bool ok = zcbor_tstr_put_lit(zse, "tok")
        && zcbor_uint32_put(zse, info.token)
        && zcbor_tstr_put_lit(zse, "lease_ms")
        && zcbor_uint32_put(zse, info.lease_ms)
        && zcbor_tstr_put_lit(zse, "rev")
        && zcbor_uint32_put(zse, info.rev)
        && zcbor_tstr_put_lit(zse, "stored_crc")
        && zcbor_uint32_put(zse, info.stored_crc)
        && zcbor_tstr_put_lit(zse, "max_patch")
        && zcbor_uint32_put(zse, info.max_patch)
        && zcbor_tstr_put_lit(zse, "max_stage")
        && zcbor_uint32_put(zse, info.max_stage);

    return MGMT_RETURN_CHECK(ok);
}

int UiMgmtGroup::HandleStage(smp_streamer* ctxt) {
    uint32_t token = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
    uint32_t rev = 0;
    zcbor_string data{};
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("off", zcbor_uint32_decode, &offset),
        SmpMapKey("data", zcbor_bstr_decode, &data),
        SmpMapKey("len", zcbor_uint32_decode, &length),
        SmpMapKey("crc", zcbor_uint32_decode, &crc),
        SmpMapKey("rev", zcbor_uint32_decode, &rev),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found || !keys[2].found)
        return MGMT_ERR_EINVAL;

    // The first chunk says what the transfer is.
    if(offset == 0 && (!keys[3].found || !keys[4].found))
        return MGMT_ERR_EINVAL;

    const UiPreviewService::StageRequest request{
        .token = token,
        .offset = offset,
        .data = { data.value, data.len },
        .length = keys[3].found ? std::optional<uint32_t>(length) : std::nullopt,
        .crc = keys[4].found ? std::optional<uint32_t>(crc) : std::nullopt,
        .rev = keys[5].found ? std::optional<uint32_t>(rev) : std::nullopt,
    };

    UiPreviewService::StageInfo info;
    const auto error = preview_service_->Stage(request, info);

    switch(error) {
        case UiPreviewError::OK:
            break;
        case UiPreviewError::BAD_OFFSET:
            return Fail(ctxt, error, "off", info.received);
        case UiPreviewError::STALE:
            return Fail(ctxt, error, "rev", info.current_rev);
        case UiPreviewError::INVALID:
            return FailInvalid(ctxt);
        default:
            return Fail(ctxt, error);
    }

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, info.received);
    if(ok && info.rev.has_value())
        ok = zcbor_tstr_put_lit(zse, "rev") && zcbor_uint32_put(zse, *info.rev);

    // Where the gauge stands now that the configuration is shown.
    if(ok && info.applied.has_value() && info.applied->group.has_value())
        ok = zcbor_tstr_put_lit(zse, "group") && zcbor_uint32_put(zse, *info.applied->group);

    if(ok && info.applied.has_value() && info.applied->overlay.has_value())
        ok = zcbor_tstr_put_lit(zse, "overlay") && zcbor_uint32_put(zse, *info.applied->overlay);

    return MGMT_RETURN_CHECK(ok);
}

int UiMgmtGroup::HandleShow(smp_streamer* ctxt) {
    uint32_t token = 0;
    TargetField target;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("target", DecodeTarget, &target),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    UiPreviewShown shown;
    const auto error = preview_service_->Show(token, { .is_overlay = target.is_overlay, .id = target.id }, shown);
    if(error != UiPreviewError::OK)
        return Fail(ctxt, error);

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "group") && zcbor_uint32_put(zse, shown.group);
    if(ok && shown.overlay.has_value())
        ok = zcbor_tstr_put_lit(zse, "overlay") && zcbor_uint32_put(zse, *shown.overlay);

    return MGMT_RETURN_CHECK(ok);
}

int UiMgmtGroup::HandleHighlight(smp_streamer* ctxt) {
    uint32_t token = 0;
    uint32_t screen = 0;
    uint32_t widget = 0;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("screen", zcbor_uint32_decode, &screen),
        SmpMapKey("widget", zcbor_uint32_decode, &widget),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found)
        return MGMT_ERR_EINVAL;

    const auto error = preview_service_->Highlight(
        token, keys[1].found ? std::optional<uint32_t>(screen) : std::nullopt,
        keys[2].found ? std::optional<uint32_t>(widget) : std::nullopt
    );

    return error == UiPreviewError::OK ? MGMT_ERR_EOK : Fail(ctxt, error);
}

int UiMgmtGroup::HandleEnd(smp_streamer* ctxt) {
    uint32_t token = 0;
    std::array keys = { SmpMapKey("tok", zcbor_uint32_decode, &token) };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found)
        return MGMT_ERR_EINVAL;

    bool is_reverted = false;
    const auto error = preview_service_->End(token, is_reverted);
    if(error != UiPreviewError::OK)
        return Fail(ctxt, error);

    zcbor_state_t* zse = ctxt->writer->zs;
    const bool ok = zcbor_tstr_put_lit(zse, "reverted") && zcbor_bool_put(zse, is_reverted);

    return MGMT_RETURN_CHECK(ok);
}

int UiMgmtGroup::HandleStatus(smp_streamer* ctxt) {
    const auto status = preview_service_->GetStatus();

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "active") && zcbor_bool_put(zse, status.is_active);

    if(ok && status.is_active) {
        ok = zcbor_tstr_put_lit(zse, "rev")
            && zcbor_uint32_put(zse, status.rev)
            && zcbor_tstr_put_lit(zse, "lease_left_ms")
            && zcbor_uint32_put(zse, status.lease_left_ms);
    }

    ok = ok && zcbor_tstr_put_lit(zse, "stored_crc") && zcbor_uint32_put(zse, status.stored_crc);

    if(ok && status.group.has_value())
        ok = zcbor_tstr_put_lit(zse, "group") && zcbor_uint32_put(zse, *status.group);

    return MGMT_RETURN_CHECK(ok);
}

int UiMgmtGroup::Fail(smp_streamer* ctxt, UiPreviewError error, const char* key, uint32_t value) {
    if(key == nullptr)
        return SmpAddGroupError(ctxt, SmpGroupId::UI, std::to_underlying(error));

    zcbor_state_t* zse = ctxt->writer->zs;
    const bool ok = smp_add_cmd_err(zse, std::to_underlying(SmpGroupId::UI), std::to_underlying(error))
        && zcbor_tstr_encode_ptr(zse, key, std::strlen(key))
        && zcbor_uint32_put(zse, value);

    return MGMT_RETURN_CHECK(ok);
}

int UiMgmtGroup::FailInvalid(smp_streamer* ctxt) const {
    const auto reason = preview_service_->GetReason();
    if(reason.empty())
        return Fail(ctxt, UiPreviewError::INVALID);

    zcbor_state_t* zse = ctxt->writer->zs;
    const bool ok = smp_add_cmd_err(
                        zse, std::to_underlying(SmpGroupId::UI), std::to_underlying(UiPreviewError::INVALID)
                    )
        && zcbor_tstr_put_lit(zse, "msg")
        && zcbor_tstr_encode_ptr(zse, reason.data(), reason.size());

    return MGMT_RETURN_CHECK(ok);
}

int UiMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<UiPreviewError>(error)) {
        case UiPreviewError::OK:
            return MGMT_ERR_EOK;
        case UiPreviewError::NO_SESSION:
        case UiPreviewError::NOT_FOUND:
            return MGMT_ERR_ENOENT;
        case UiPreviewError::BUSY:
            return MGMT_ERR_EBUSY;
        case UiPreviewError::CRC_MISMATCH:
            return MGMT_ERR_ECORRUPT;
        case UiPreviewError::NO_MEMORY:
            return MGMT_ERR_ENOMEM;
        case UiPreviewError::BAD_TOKEN:
        case UiPreviewError::STALE:
        case UiPreviewError::BASE_MISMATCH:
        case UiPreviewError::TOO_LARGE:
        case UiPreviewError::BAD_OFFSET:
        case UiPreviewError::INVALID:
            return MGMT_ERR_EINVAL;
        case UiPreviewError::UNKNOWN:
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::domain::ui_preview_domain::smp

#endif // CONFIG_EERIE_LEAP_UI_PREVIEW
