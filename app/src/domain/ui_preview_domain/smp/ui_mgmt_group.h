#pragma once

#include <cstdint>
#include <memory>

#include "subsys/smp/smp_group.h"

#include "domain/ui_domain/preview/ui_preview_session.h"
#include "domain/ui_preview_domain/ui_preview_service.h"

namespace eerie_leap::domain::ui_preview_domain::smp {

using eerie_leap::domain::ui_domain::preview::UiPreviewError;
using eerie_leap::subsys::smp::SmpGroup;

/**
 * @brief SMP `ui` group (73): the preview of a UI configuration on the unit that shows it.
 *
 * The group decodes the request on the MCUmgr work queue, with no allocation and no exception on its small stack,
 * and hands the rest to UiPreviewService, which runs it as jobs on the queue it belongs to while the handler
 * waits. Every request that carries `tok` renews the session's lease. Errors are SMP v2 group errors,
 * `{err: {group: 73, rc}}`, with `msg` on INVALID, `off` on BAD_OFFSET and `rev` on STALE.
 *
 * Commands (CDDL: `cbor_schemas/ui_preview_smp.cddl`, plan: `gauge-ui-editor.md` Appendix B):
 * - BEGIN `{? tok, ? pin}`: `{tok, lease_ms, rev, stored_crc, max_patch, max_ops, max_stage}`
 * - STAGE `{tok, off, data, ? len, ? crc, ? rev}`: `{off, ? rev}`
 * - PATCH `{tok, rev, ops: [+ op]}`: `{rev, effects: [+ 0..3]}`, op `[0, screen, widget-record]`,
 *   `[1, screen, widget]`, `[2, screen-record]`, `[3, screen]`
 * - SHOW `{tok, target: {group} / {overlay}}`: `{group, ? overlay}`
 * - HIGHLIGHT `{tok, ? screen, ? widget}`: `{}`
 * - END `{tok}`: `{reverted}`
 * - STATUS (read): `{active, ? rev, ? lease_left_ms, stored_crc, ? group}`
 *
 */
class UiMgmtGroup : public SmpGroup<UiMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        BEGIN = 0,
        STAGE = 1,
        PATCH = 2,
        SHOW = 3,
        HIGHLIGHT = 4,
        END = 5,
        STATUS = 6, ///< The one read.
    };

private:
    static const mgmt_handler HANDLERS[];

    std::shared_ptr<UiPreviewService> preview_service_;

    int HandleBegin(smp_streamer* ctxt);
    int HandleStage(smp_streamer* ctxt);
    int HandlePatch(smp_streamer* ctxt);
    int HandleShow(smp_streamer* ctxt);
    int HandleHighlight(smp_streamer* ctxt);
    int HandleEnd(smp_streamer* ctxt);
    int HandleStatus(smp_streamer* ctxt);

    /** @param key and @param value Sent beside the error when @p key is not empty (`off` for BAD_OFFSET, `rev` for STALE). */
    static int Fail(smp_streamer* ctxt, UiPreviewError error, const char* key = nullptr, uint32_t value = 0);
    int FailInvalid(smp_streamer* ctxt) const;

    static int TranslateError(uint16_t error);

public:
    explicit UiMgmtGroup(std::shared_ptr<UiPreviewService> preview_service);
};

} // namespace eerie_leap::domain::ui_preview_domain::smp
