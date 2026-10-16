/*
 * v0.7.14 research probe: what is on screen outside the game (HOME menu,
 * applets), toward streaming the whole system. Off unless
 * sdmc:/config/switch-frame-tap/probe-ui.txt exists; a word "no<part>" in it
 * turns one part off (nomap, nodispdrv, nobinders).
 *
 * probe1 (logs/v0714-probe1): the HOME menu, the user picker and the
 * controller applet have no frame buffers of their own - none of their
 * memory is mapped by two devices (GPU and display), while the overlay and
 * the Album each have 2 x 1280x768x4 that are. Their frames are in a buffer
 * someone else owns (the system shared buffer, vi's "fbshare").
 *
 * probe2 (this):
 *  map      every process's large regions (>= 2 MB), at idle and when a
 *           HOME-menu process or applet starts - where that buffer lives
 *  dispdrv  the applets' own frame traffic (applet_mitm_uibinder.cpp): their
 *           slots, crops, and the NvGraphicBuffer of each slot
 *  binders  4 minutes after boot, from our own dispdrv session: which binder
 *           ids answer "query" (read-only), the HOME menu's among them, its
 *           buffer history (code 17), and requestBuffer on its slots - which
 *           only succeeds while the slot is dequeued, i.e. being drawn
 */
#include <stratosphere.hpp>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_service.hpp"
#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_uibinder.hpp"

namespace ams::mitm::applet {

    namespace {

        constinit bool g_probe_on = false;
        constinit bool g_probe_checked = false;
        constinit char g_probe_words[256] = {};
        constinit u64 g_next_ns = 0;
        constinit u64 g_dump_at_ns = 0;     /* a map dump due (a moment after a change) */
        constinit const char *g_dump_why = "";
        constinit u32 g_dumps = 0;
        constinit u64 g_last_app = ~UINT64_C(0);
        constinit u64 g_next_summary_ns = 0;

        struct Proc { u64 pid; u64 tid; };
        constinit Proc g_prev[96] = {};
        constinit u32 g_prev_n = 0;

        void ReadConfig() {
            if (g_probe_checked) { return; }
            g_probe_checked = true;
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/config/switch-frame-tap/probe-ui.txt", fs::OpenMode_Read))) { return; }
            size_t n = 0;
            static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, g_probe_words, sizeof(g_probe_words) - 1));
            fs::CloseFile(f);
            g_probe_words[n < sizeof(g_probe_words) ? n : sizeof(g_probe_words) - 1] = 0;
            g_probe_on = true;
        }

        /* whole words only (a substring once armed a dangerous feature) */
        bool HasWord(const char *w) {
            const size_t wl = std::strlen(w);
            for (const char *p = g_probe_words; (p = std::strstr(p, w)) != nullptr; p += wl) {
                const bool start = p == g_probe_words || !std::isalnum(static_cast<unsigned char>(p[-1]));
                const bool end = !std::isalnum(static_cast<unsigned char>(p[wl]));
                if (start && end) { return true; }
            }
            return false;
        }

        const char *AppletName(u64 tid) {
            switch (tid) {
                case 0x0100000000001000: return "HOME menu (qlaunch)";
                case 0x0100000000001001: return "auth";
                case 0x0100000000001002: return "cabinet (amiibo)";
                case 0x0100000000001003: return "controller";
                case 0x0100000000001004: return "dataErase";
                case 0x0100000000001005: return "error";
                case 0x0100000000001006: return "netConnect";
                case 0x0100000000001007: return "user picker";
                case 0x0100000000001008: return "keyboard";
                case 0x0100000000001009: return "miiEdit";
                case 0x010000000000100A: return "web";
                case 0x010000000000100B: return "shop";
                case 0x010000000000100C: return "overlay (overlayDisp)";
                case 0x010000000000100D: return "Album (photoViewer)";
                case 0x010000000000100F: return "offlineWeb";
                case 0x0100000000001010: return "loginShare";
                case 0x0100000000001012: return "starter";
                case 0x0100000000001013: return "myPage";
                default: return nullptr;
            }
        }

        const char *ProcName(u64 tid) {
            switch (tid) {
                case 0x0100000000000000: return "fs";
                case 0x0100000000000003: return "pm";
                case 0x0100000000000004: return "sm";
                case 0x0100000000000012: return "bsdsockets";
                case 0x0100000000000013: return "hid";
                case 0x0100000000000019: return "nvservices";
                case 0x010000000000001C: return "nvnflinger";
                case 0x010000000000001F: return "ns";
                case 0x0100000000000022: return "capsrv";
                case 0x0100000000000023: return "am";
                case 0x0100000000000024: return "ssl";
                case 0x010000000000002D: return "vi";
                case 0x0100000000000035: return "grc";
                case 0x0100000000000C20: return "SFT";
                case 0x0100000000000C21: return "SFT-donor";
                default: return AppletName(tid);
            }
        }

        u64 ProgramOf(u64 pid, os::NativeHandle *out_h) {
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) { return 0; }
            if (out_h != nullptr) { *out_h = h; } else { os::CloseNativeHandle(h); }
            return loc.program_id.value;
        }

        /* every process but the game: regions of 2 MB and up, and for the
         * HOME menu and applets any device-mapped one of 512 KB and up */
        void DumpMaps(const char *why) {
            static u64 pids[96];
            s32 n = 0;
            if (R_FAILED(::svcGetProcessList(&n, pids, 96))) { return; }
            const u64 app = g_app_pid.load(std::memory_order_relaxed);
            LogLine("uiprobe: memory map (%s): %d processes, regions >= 2 MB (type/perm/attr/devices/ipc)", why, n);
            u32 lines = 0;
            for (s32 i = 0; i < n && lines < 160; ++i) {
                if (pids[i] == app) { continue; }
                os::NativeHandle h = os::InvalidNativeHandle;
                const u64 tid = ProgramOf(pids[i], std::addressof(h));
                if (tid == 0) { continue; }
                const bool ui = IsUiProgram(tid);
                const char *nm = ProcName(tid);
                u64 addr = 0;
                for (u32 guard = 0; guard < 4096 && lines < 160; ++guard) {
                    ::MemoryInfo mi = {};
                    u32 pi = 0;
                    if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0) { break; }
                    const u32 type = mi.type & 0xFF;
                    const bool big = mi.size >= 2 * 1024 * 1024 && type != 0 && type != 0x10;
                    const bool dev = ui && mi.device_refcount > 0 && mi.size >= 512 * 1024;
                    if (big || dev) {
                        ++lines;
                        LogLine("map %-14s %#llx %7llu KB type %#04x perm %u attr %#x dev %u ipc %u",
                                nm != nullptr ? nm : "", static_cast<unsigned long long>(mi.addr), static_cast<unsigned long long>(mi.size / 1024),
                                mi.type, mi.perm, mi.attr, mi.device_refcount, mi.ipc_refcount);
                        if (nm == nullptr) { LogLine("    (that is %016llx, pid %llu)", static_cast<unsigned long long>(tid), static_cast<unsigned long long>(pids[i])); }
                    }
                    const u64 next = mi.addr + mi.size;
                    if (next <= addr) { break; }
                    addr = next;
                }
                os::CloseNativeHandle(h);
            }
            LogLine("uiprobe: memory map done, %u lines", lines);
        }

        /* ---- our own dispdrv session ---------------------------------- */

        constinit ::Service g_dd = {};
        constinit bool g_dd_ok = false;
        alignas(0x10) constinit u8 g_tx_in[0x200] = {};
        alignas(0x10) constinit u8 g_tx_out[0x400] = {};

        /* an IGraphicBufferProducer parcel: header, interface token, args */
        size_t BuildParcel(const s32 *args, u32 nargs) {
            static constexpr char Token[] = "android.gui.IGraphicBufferProducer";
            constexpr u32 TokenLen = sizeof(Token) - 1;   /* 34 */
            std::memset(g_tx_in, 0, sizeof(g_tx_in));
            u8 *p = g_tx_in + 0x10;
            u32 off = 0;
            const u32 policy = 0x100;
            std::memcpy(p + off, std::addressof(policy), 4); off += 4;
            std::memcpy(p + off, std::addressof(TokenLen), 4); off += 4;
            for (u32 i = 0; i < TokenLen; ++i) { const u16 c = static_cast<u8>(Token[i]); std::memcpy(p + off + 2 * i, std::addressof(c), 2); }
            off += ((TokenLen + 1) * 2 + 3) & ~3u;
            for (u32 i = 0; i < nargs; ++i) { std::memcpy(p + off, args + i, 4); off += 4; }
            const u32 hdr[4] = { off, 0x10, 0, 0x10 + off };
            std::memcpy(g_tx_in, hdr, sizeof(hdr));
            return 0x10 + off;
        }

        /* out: the reply's payload and its length */
        Result OwnTransact(s32 id, u32 code, const s32 *args, u32 nargs, const u8 **out_payload, u32 *out_len) {
            const size_t n = BuildParcel(args, nargs);
            std::memset(g_tx_out, 0, sizeof(g_tx_out));
            const struct { s32 id; u32 code; u32 flags; } in = { id, code, 0 };
            const ::Result rc = serviceDispatchIn(std::addressof(g_dd), 3, in,
                .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In, SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
                .buffers      = { { g_tx_in, n }, { g_tx_out, sizeof(g_tx_out) } },
            );
            u32 hdr[4];
            std::memcpy(hdr, g_tx_out, sizeof(hdr));
            if (hdr[1] > sizeof(g_tx_out) || hdr[0] > sizeof(g_tx_out) - hdr[1]) { hdr[0] = 0; hdr[1] = 0x10; }
            *out_payload = g_tx_out + hdr[1];
            *out_len = hdr[0];
            R_RETURN(::ams::Result(rc));
        }

        s32 Word(const u8 *p, u32 len, u32 i) {
            s32 v = 0;
            if ((i + 1) * 4 <= len) { std::memcpy(std::addressof(v), p + 4 * i, 4); }
            return v;
        }

        void Hex(const char *tag, const u8 *p, u32 len) {
            char hex[3 * 64 + 1] = {};
            const u32 n = len < 64 ? len : 64;
            for (u32 i = 0; i < n; ++i) { std::snprintf(hex + 3 * i, 4, "%02x ", p[i]); }
            LogLine("   %s (%u bytes): %s%s", tag, len, hex, len > n ? "..." : "");
        }

        /* the binders that answered query with a size, and what requestBuffer did */
        struct Cand {
            s32 id;
            s32 w, h, fmt;
            u32 ok[4], bad[4];
            bool logged[4];
        };
        constinit Cand g_cands[8] = {};
        constinit u32 g_ncands = 0;
        constinit u32 g_binder_stage = 0;   /* 0 waiting, 1 racing, 2 done */
        constinit u32 g_race_steps = 0;

        bool SeenInMitm(s32 id) {
            static UiBinderInfo b[16];
            const u32 n = UiBinderSnapshot(b, 16);
            for (u32 i = 0; i < n; ++i) { if (b[i].id == id) { return true; } }
            return false;
        }

        void Enumerate() {
            static UiBinderInfo b[16];
            const u32 nb = UiBinderSnapshot(b, 16);
            s32 top = 24;
            for (u32 i = 0; i < nb; ++i) { if (b[i].id + 8 > top) { top = b[i].id + 8; } }
            if (top > 64) { top = 64; }
            LogLine("uiprobe: binders 1..%d from our own dispdrv session (query: read-only)", top);
            for (s32 id = 1; id <= top; ++id) {
                const u8 *p = nullptr;
                u32 len = 0;
                s32 a = 0;   /* NATIVE_WINDOW_WIDTH */
                const Result rw = OwnTransact(id, 9, std::addressof(a), 1, std::addressof(p), std::addressof(len));
                const s32 w = Word(p, len, 0), sw = Word(p, len, 1);
                if (R_FAILED(rw) || len < 8) {
                    LogLine("   binder %d: query rc=0x%x reply %u bytes", id, rw.GetValue(), len);
                    continue;
                }
                a = 1;   /* HEIGHT */
                static_cast<void>(OwnTransact(id, 9, std::addressof(a), 1, std::addressof(p), std::addressof(len)));
                const s32 h = Word(p, len, 0);
                a = 2;   /* FORMAT */
                static_cast<void>(OwnTransact(id, 9, std::addressof(a), 1, std::addressof(p), std::addressof(len)));
                const s32 fmt = Word(p, len, 0);
                LogLine("   binder %d: %dx%d format %d (status %d)%s", id, w, h, fmt, sw, SeenInMitm(id) ? " - an applet's, seen by the mitm" : "");
                if (sw == 0 && w > 0 && h > 0 && g_ncands < 8) {
                    g_cands[g_ncands++] = { id, w, h, fmt, {}, {}, {} };
                    /* the buffer history: no slot in it per the emulators - see what the console says */
                    s32 cnt = 8;
                    const Result rh = OwnTransact(id, 17, std::addressof(cnt), 1, std::addressof(p), std::addressof(len));
                    LogLine("   binder %d: getBufferHistory(8) rc=0x%x", id, rh.GetValue());
                    if (R_SUCCEEDED(rh)) { Hex("history", p, len); }
                }
            }
        }

        /* one sample per candidate per step: which slots are dequeued now */
        void RaceStep() {
            for (u32 c = 0; c < g_ncands; ++c) {
                Cand &k = g_cands[c];
                for (s32 slot = 0; slot < 4; ++slot) {
                    const u8 *p = nullptr;
                    u32 len = 0;
                    const Result rc = OwnTransact(k.id, 1, std::addressof(slot), 1, std::addressof(p), std::addressof(len));
                    const auto *gb = R_SUCCEEDED(rc) ? FindGraphicBuffer(p, len) : nullptr;
                    if (gb != nullptr) {
                        ++k.ok[slot];
                        if (!k.logged[slot]) {
                            k.logged[slot] = true;
                            char tag[64];
                            std::snprintf(tag, sizeof(tag), "binder %d slot %d (our requestBuffer)", k.id, slot);
                            LogGraphicBuffer(tag, gb);
                        }
                    } else {
                        ++k.bad[slot];
                        if (k.bad[slot] == 1) { LogLine("   binder %d slot %d: requestBuffer rc=0x%x, reply %u bytes, status %d", k.id, slot, rc.GetValue(), len, Word(p, len, len >= 4 ? len / 4 - 1 : 0)); }
                    }
                }
            }
        }

        void Summaries(u64 now) {
            if (now < g_next_summary_ns) { return; }
            g_next_summary_ns = now + UINT64_C(5000000000);
            static u32 last_q[16] = {};
            static s32 last_id[16] = {};
            static UiBinderInfo b[16];
            const u32 n = UiBinderSnapshot(b, 16);
            for (u32 i = 0; i < n; ++i) {
                if (b[i].id == last_id[i] && b[i].queues == last_q[i]) { continue; }
                last_id[i] = b[i].id;
                last_q[i] = b[i].queues;
                const char *nm = AppletName(b[i].tid);
                LogLine("ui binder %d (%s): %u txns, %u queued, %u dequeued, %u requests; last slot %d, slots used 0x%x, crop (%d,%d)-(%d,%d), transform 0x%x",
                        b[i].id, nm != nullptr ? nm : "?", b[i].txns, b[i].queues, b[i].dequeues, b[i].requests, b[i].last_slot, b[i].slots_seen,
                        b[i].crop[0], b[i].crop[1], b[i].crop[2], b[i].crop[3], b[i].transform);
            }
        }

    }

    bool UiProbeOn() { ReadConfig(); return g_probe_on; }

    bool UiProbeWants(const char *word) {
        ReadConfig();
        if (!g_probe_on) { return false; }
        char no[32];
        std::snprintf(no, sizeof(no), "no%s", word);
        return !HasWord(no);
    }

    void UiProbeSetDispdrv(const ::Service &s) { g_dd = s; g_dd_ok = true; }

    void UiProbeStep() {
        if (!g_probe_checked) {
            ReadConfig();
            if (g_probe_on) {
                LogLine("uiprobe: on (config/switch-frame-tap/probe-ui.txt: \"%.40s\") - map %s, dispdrv %s, binders %s", g_probe_words,
                        UiProbeWants("map") ? "on" : "off", UiProbeWants("dispdrv") ? "on" : "off", UiProbeWants("binders") ? "on" : "off");
            }
        }
        if (!g_probe_on || g_pmdmnt_rc != 0) { return; }
        const u64 now = armTicksToNs(armGetSystemTick());

        Summaries(now);

        /* the binder look, last in the boot: 4 minutes in, then 60 s of samples */
        if (UiProbeWants("binders") && g_dd_ok) {
            if (g_binder_stage == 0 && now >= UINT64_C(240000000000)) {
                g_binder_stage = 1;
                Enumerate();
                LogLine("uiprobe: sampling requestBuffer on %u binder(s) for 60 s (succeeds only while the slot is being drawn)", g_ncands);
            } else if (g_binder_stage == 1) {
                RaceStep();
                if (++g_race_steps >= 240) {
                    g_binder_stage = 2;
                    for (u32 c = 0; c < g_ncands; ++c) {
                        const Cand &k = g_cands[c];
                        LogLine("uiprobe: binder %d (%dx%d): dequeued when sampled - slot0 %u/%u, slot1 %u/%u, slot2 %u/%u, slot3 %u/%u",
                                k.id, k.w, k.h, k.ok[0], k.ok[0] + k.bad[0], k.ok[1], k.ok[1] + k.bad[1], k.ok[2], k.ok[2] + k.bad[2], k.ok[3], k.ok[3] + k.bad[3]);
                    }
                    LogLine("uiprobe: binder look done");
                }
            }
        }

        if (now < g_next_ns) { return; }
        g_next_ns = now + UINT64_C(1000000000);

        static u64 pids[96];
        s32 n = 0;
        if (R_FAILED(::svcGetProcessList(&n, pids, 96))) { return; }
        Proc cur[96];
        u32 cn = 0;
        bool changed = false;
        for (s32 i = 0; i < n && cn < 96; ++i) {
            u64 tid = 0;
            for (u32 k = 0; k < g_prev_n; ++k) { if (g_prev[k].pid == pids[i]) { tid = g_prev[k].tid; break; } }
            const bool known = tid != 0;
            if (!known) { tid = ProgramOf(pids[i], nullptr); }
            cur[cn++] = { pids[i], tid };
            if (!known && g_prev_n != 0 && tid != 0) {
                const char *nm = AppletName(tid);
                LogLine("uiprobe: started %016llx%s%s%s (pid %llu)", static_cast<unsigned long long>(tid),
                        nm ? " (" : "", nm ? nm : "", nm ? ")" : "", static_cast<unsigned long long>(pids[i]));
                if (IsUiProgram(tid)) { changed = true; }
            }
        }
        for (u32 k = 0; k < g_prev_n && g_prev_n != 0; ++k) {
            bool alive = false;
            for (u32 j = 0; j < cn; ++j) { if (cur[j].pid == g_prev[k].pid) { alive = true; break; } }
            if (!alive) {
                const char *nm = AppletName(g_prev[k].tid);
                LogLine("uiprobe: exited %016llx%s%s%s", static_cast<unsigned long long>(g_prev[k].tid), nm ? " (" : "", nm ? nm : "", nm ? ")" : "");
            }
        }
        const bool first = g_prev_n == 0;
        std::memcpy(g_prev, cur, sizeof(Proc) * cn);
        g_prev_n = cn;

        /* the game in front or not (pid 0: the HOME menu or an applet) */
        const u64 app = g_app_pid.load(std::memory_order_relaxed);
        if (app != g_last_app) {
            LogLine("uiprobe: application running: %s", app != 0 ? "yes" : "no");
            g_last_app = app;
        }
        if ((first || changed) && UiProbeWants("map")) {
            g_dump_at_ns = now + UINT64_C(2000000000);   /* let it allocate first */
            g_dump_why = first ? "idle" : "a HOME-menu process or applet started";
        }
        if (g_dump_at_ns != 0 && now >= g_dump_at_ns && g_dumps < 12) {
            g_dump_at_ns = 0;
            ++g_dumps;
            DumpMaps(g_dump_why);
        }
    }

}
