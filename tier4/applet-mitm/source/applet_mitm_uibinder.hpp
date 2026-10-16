/*
 * v0.7.14 research (probe2): the HOME menu and the library applets present
 * through dispdrv, not vi.
 *
 * Firmware 22.5.0's IPC tables (Switch-Ghidra-Guides ipc_json_dumps) give
 * qlaunch, overlayDisp, playerSelect and controller no vi service at all:
 * they talk to dispdrv (nns::hosbinder::IHOSBinderDriver, hosted by
 * nvnflinger 010000000000001C) directly, the same IGraphicBufferProducer
 * protocol a game reaches through vi:u's relay. qlaunch and overlayDisp open
 * it at boot, before this module; a library applet opens it when it starts,
 * after us - so a dispdrv mitm sees the applets' frames.
 *
 * This wrapper only looks: every request is forwarded unchanged, except
 * requestBuffer (code 1), which is forwarded by hand so its reply - the
 * slot's NvGraphicBuffer - can be read. Off unless probe-ui.txt is on the SD
 * card.
 *
 * IHOSBinderDriver on 22.5.0 (nvnflinger's table):
 *   0 TransactParcel      (s32 id, u32 code, u32 flags) + type-5 in, type-6 out
 *   1 AdjustRefcount      (s32 id, s32 add, s32 type)
 *   2 GetNativeHandle     (s32 id, u32 what) -> copy handle
 *   3 TransactParcelAuto  (s32 id, u32 code, u32 flags) + auto-select in/out
 */
#pragma once
#include <stratosphere.hpp>

#define AMS_UI_BINDER_MITM_INTERFACE_INFO(C, H)                                                                                                                                                                      \
    AMS_SF_METHOD_INFO(C, H, 0, Result, TransactParcel,     (s32 binder_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out), (binder_id, code, flags, parcel_in, parcel_out))            \
    AMS_SF_METHOD_INFO(C, H, 1, Result, AdjustRefcount,     (s32 binder_id, s32 add, s32 type), (binder_id, add, type))                                                                                                     \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetNativeHandle,    (s32 binder_id, u32 what), (binder_id, what))                                                                                                                   \
    AMS_SF_METHOD_INFO(C, H, 3, Result, TransactParcelAuto, (s32 binder_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out), (binder_id, code, flags, parcel_in, parcel_out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IUiBinderMitm, AMS_UI_BINDER_MITM_INTERFACE_INFO, 0x2AB1E145)

namespace ams::mitm::applet {

    /* the HOME menu, the overlay and every library applet */
    inline bool IsUiProgram(u64 tid) { return tid >= UINT64_C(0x0100000000001000) && tid < UINT64_C(0x0100000000002000); }

    class UiBinderMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            /* program id only - no IPC here (see g_app_pid) */
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) { return IsUiProgram(client_info.program_id.value); }
        public:
            Result TransactParcel(s32 binder_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out);
            Result AdjustRefcount(s32 binder_id, s32 add, s32 type);
            Result GetNativeHandle(s32 binder_id, u32 what);
            Result TransactParcelAuto(s32 binder_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out);
        private:
            Result Transact(u32 cmd, s32 binder_id, u32 code, u32 flags, const void *in, size_t in_size, void *out, size_t out_size);
    };
    static_assert(IsIUiBinderMitm<UiBinderMitm>);

    /* probe-ui.txt: present, and the word in it is not disabled ("no<word>") */
    bool UiProbeOn();
    bool UiProbeWants(const char *word);

    /* our own dispdrv session, for transactions of our own (applet_mitm_uiprobe.cpp) */
    void UiProbeSetDispdrv(const ::Service &s);

    /* what the wrapper saw, for the probe's summaries (applet_mitm_uibinder.cpp) */
    struct UiBinderInfo {
        s32 id;
        u64 tid;
        u32 txns, queues, dequeues, requests;
        s32 last_slot;
        u32 slots_seen;        /* bit per slot queued */
        s32 crop[4];
        u32 transform;
        u64 last_queue_tick;
    };
    u32 UiBinderSnapshot(UiBinderInfo *out, u32 max);

    /* applet_mitm_service.cpp's parcel readers */
    s32 ParseParcelSlot(const u8 *p, size_t sz);
    bool ParseParcelQueueInput(const u8 *p, size_t sz, u32 *n, u64 out[4], u32 *transform, s32 crop[4]);

}
