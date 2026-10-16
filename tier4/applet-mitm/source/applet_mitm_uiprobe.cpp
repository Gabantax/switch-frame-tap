/*
 * v0.7.14 research probe: what is on screen outside the game (HOME menu,
 * applets), toward streaming the whole system. Read-only and off unless
 * sdmc:/config/switch-frame-tap/probe-ui.txt exists.
 *
 * Once a second it logs which processes started or exited (applets by name),
 * and, when that changes, the memory layout of the HOME menu and any running
 * applet: only the large regions a device (the GPU, the display) has mapped -
 * where frame buffers would be - by address, size, state and device count.
 * Nothing is attached to, paused or read.
 */
#include <stratosphere.hpp>
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"

namespace ams::mitm::applet {

    namespace {

        constinit bool g_probe_on = false;
        constinit bool g_probe_checked = false;
        constinit u64 g_next_ns = 0;
        constinit u64 g_dump_at_ns = 0;     /* a layout dump due (a moment after a change) */
        constinit u32 g_dumps = 0;
        constinit u64 g_last_app = ~UINT64_C(0);

        struct Proc { u64 pid; u64 tid; };
        constinit Proc g_prev[96] = {};
        constinit u32 g_prev_n = 0;

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

        bool IsUiProcess(u64 tid) { return tid >= 0x0100000000001000ull && tid < 0x0100000000002000ull; }

        u64 ProgramOf(u64 pid, os::NativeHandle *out_h) {
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) { return 0; }
            if (out_h != nullptr) { *out_h = h; } else { os::CloseNativeHandle(h); }
            return loc.program_id.value;
        }

        /* the regions a device maps, 512 KB and up */
        void DumpLayout(u64 pid, u64 tid) {
            os::NativeHandle h = os::InvalidNativeHandle;
            if (ProgramOf(pid, std::addressof(h)) != tid) { if (h != os::InvalidNativeHandle) { os::CloseNativeHandle(h); } return; }
            const char *nm = AppletName(tid);
            u64 addr = 0, dev_total = 0;
            u32 lines = 0, regions = 0;
            for (u32 guard = 0; guard < 4096; ++guard) {
                ::MemoryInfo mi = {};
                u32 pi = 0;
                if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0) { break; }
                ++regions;
                if (mi.device_refcount > 0 && mi.size >= 512 * 1024) {
                    dev_total += mi.size;
                    if (lines++ < 12) {
                        LogLine("uiprobe:   %s region %#llx size %llu KB type %#x perm %#x attr %#x devices %u",
                                nm != nullptr ? nm : "?", static_cast<unsigned long long>(mi.addr), static_cast<unsigned long long>(mi.size / 1024),
                                mi.type, mi.perm, mi.attr, mi.device_refcount);
                    }
                }
                const u64 next = mi.addr + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            os::CloseNativeHandle(h);
            LogLine("uiprobe: %s (pid %llu): %u regions, %u device-mapped >= 512 KB, %llu KB in them",
                    nm != nullptr ? nm : "?", static_cast<unsigned long long>(pid), regions, lines, static_cast<unsigned long long>(dev_total / 1024));
        }

    }

    void UiProbeStep() {
        if (!g_probe_checked) {
            g_probe_checked = true;
            fs::DirectoryEntryType t;
            g_probe_on = R_SUCCEEDED(fs::GetEntryType(std::addressof(t), "sdmc:/config/switch-frame-tap/probe-ui.txt"));
            if (g_probe_on) { LogLine("uiprobe: on (read-only; config/switch-frame-tap/probe-ui.txt)"); }
        }
        if (!g_probe_on || g_pmdmnt_rc != 0) { return; }
        const u64 now = armTicksToNs(armGetSystemTick());
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
                if (IsUiProcess(tid)) { changed = true; }
            }
        }
        for (u32 k = 0; k < g_prev_n && g_prev_n != 0; ++k) {
            bool alive = false;
            for (u32 j = 0; j < cn; ++j) { if (cur[j].pid == g_prev[k].pid) { alive = true; break; } }
            if (!alive) {
                const char *nm = AppletName(g_prev[k].tid);
                LogLine("uiprobe: exited %016llx%s%s%s", static_cast<unsigned long long>(g_prev[k].tid), nm ? " (" : "", nm ? nm : "", nm ? ")" : "");
                if (IsUiProcess(g_prev[k].tid)) { changed = true; }
            }
        }
        const bool first = g_prev_n == 0;
        std::memcpy(g_prev, cur, sizeof(Proc) * cn);
        g_prev_n = cn;

        /* the game in front or not (pid 0: the HOME menu or an applet) */
        const u64 app = g_app_pid.load(std::memory_order_relaxed);
        if (app != g_last_app) {
            LogLine("uiprobe: application in front: %s", app != 0 ? "a game/app" : "none (HOME menu or an applet)");
            g_last_app = app;
            changed = true;
        }
        if (first || changed) { g_dump_at_ns = now + UINT64_C(2000000000); }   /* let it allocate first */
        if (g_dump_at_ns != 0 && now >= g_dump_at_ns && g_dumps < 40) {
            g_dump_at_ns = 0;
            ++g_dumps;
            for (u32 j = 0; j < cn; ++j) { if (IsUiProcess(cur[j].tid)) { DumpLayout(cur[j].pid, cur[j].tid); } }
        }
    }

}
