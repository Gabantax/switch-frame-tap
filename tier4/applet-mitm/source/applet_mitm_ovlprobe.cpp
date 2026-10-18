/*
 * v0.7.14 overlay probe 1: the overlays on top of a game - the system's
 * (overlayDisp, 010000000000100C: the volume bar, notifications) and Tesla /
 * Ultrahand's (nx-ovlloader, 420000000007E51A). Research only - off unless
 * sdmc:/config/switch-frame-tap/probe-ovl.txt exists, and left out of
 * release builds (SFT_OVL_PROBE).
 *
 * The stream copies the game's own frames, so what the console draws on top
 * never reaches it. Both overlay processes open their display connection at
 * boot, before this module, so their presents cannot be watched the way a
 * game's are; their pixels have to be found in their memory, as the HOME
 * menu's were in vi's. Known so far: overlayDisp has one 7680 KB region
 * mapped by two devices = 2 x 1280x768x4 (probe1 of the HOME-menu work);
 * libtesla draws a 448x720 RGBA4444 framebuffer, 2 buffers, through vi:m,
 * shown 672x1080 at the left edge.
 *
 * Every 30 s from 70 s after start, for each of the two processes that runs:
 * attach (the debug events pumped), list its device-shared regions, then for
 * 20 s hash each candidate region in 64 KB blocks every 200 ms and log which
 * blocks change per second; the first changes of a region (up to 3 per
 * region over the run) are saved whole to probe/ovl-<name>-<base>-<n>.bin.
 * No floats are formatted (a stratosphere module has no newlib heap).
 */
#include <stratosphere.hpp>
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_ovlprobe.hpp"

#if SFT_OVL_PROBE

namespace ams::mitm::applet {

    namespace {

        constexpr const char *ProbeFile = "sdmc:/config/switch-frame-tap/probe-ovl.txt";
        constexpr const char *ProbeDir  = "sdmc:/config/switch-frame-tap/probe";

        struct Target { u64 program; const char *name; };
        constexpr Target Targets[] = {
            { UINT64_C(0x010000000000100C), "overlayDisp" },
            { UINT64_C(0x420000000007E51A), "tesla" },
        };

        constexpr size_t Block = 0x10000;              /* 64 KB hash blocks */
        constexpr size_t MaxRegions = 8;
        constexpr size_t MaxBlocks = 512;              /* 32 MB of candidates per process */
        struct Region { u64 base; u64 size; u32 block0; u32 saved; u32 changes; };
        constinit Region g_regs[MaxRegions];
        constinit u32 g_nregs = 0;
        constinit u32 g_hash[MaxBlocks];
        constinit u32 g_saves[2][16];                  /* per target: saved snapshots by region index */

        constexpr size_t Chunk = 0x4000;
        alignas(0x1000) constinit u8 g_buf[Chunk];

        alignas(os::ThreadStackAlignment) constinit u8 g_probe_stack[16_KB];
        constinit os::ThreadType g_probe_thread;
        constinit u32 g_pumped = 0;

        bool ProbeOn() {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), ProbeFile, fs::OpenMode_Read))) { return false; }
            fs::CloseFile(f);
            return true;
        }
        u64 PidOf(u64 program_id) {
            os::ProcessId pid{0};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{program_id}))) { return 0; }
            return pid.value;
        }
        u32 Fnv(const u8 *p, size_t n, u32 h) {
            for (size_t i = 0; i < n; i += 4) { h = (h ^ (p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (static_cast<u32>(p[i + 3]) << 24))) * 16777619u; }
            return h;
        }
        void Pump(svc::Handle h) {
            s32 idx = 0;
            if (R_SUCCEEDED(svc::WaitSynchronization(std::addressof(idx), std::addressof(h), 1, 0))) {
                u32 n = 0;
                static_cast<void>(DbgResume(h, std::addressof(n)));
                ++g_pumped;
            }
        }
        /* the hash of one 64 KB block, read in 16 KB pieces */
        u32 HashBlock(svc::Handle h, u64 va, size_t len) {
            u32 v = 2166136261u;
            for (size_t off = 0; off < len; off += Chunk) {
                const size_t n = len - off < Chunk ? len - off : Chunk;
                if (!DbgRead(h, g_buf, va + off, n)) { return 0; }
                v = Fnv(g_buf, n, v);
            }
            return v;
        }
        /* the device-shared regions that could hold frame buffers */
        void Map(svc::Handle h, const char *name) {
            g_nregs = 0;
            u32 blocks = 0;
            u64 addr = 0;
            for (u32 guard = 0; guard < 4096; ++guard) {
                svc::MemoryInfo mi = {};
                svc::PageInfo pi = {};
                if (!DbgQuery(h, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const bool dev = (static_cast<u32>(mi.attribute) & svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count != 0;
                if (dev && mi.state != svc::MemoryState_Io && (mi.permission & svc::MemoryPermission_Read) != 0) {
                    LogLine("ovlprobe: %s device-shared %010llx %6llu KB state 0x%x perm %u attr 0x%x dev %u",
                            name, static_cast<unsigned long long>(mi.base_address), static_cast<unsigned long long>(mi.size / 1024),
                            static_cast<u32>(mi.state), static_cast<u32>(mi.permission), static_cast<u32>(mi.attribute), mi.device_count);
                    const u32 nb = static_cast<u32>((mi.size + Block - 1) / Block);
                    if (mi.size >= 256 * 1024 && g_nregs < MaxRegions && blocks + nb <= MaxBlocks) {
                        g_regs[g_nregs++] = { mi.base_address, mi.size, blocks, 0, 0 };
                        blocks += nb;
                    }
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            LogLine("ovlprobe: %s - %u candidate region(s), %u blocks", name, g_nregs, blocks);
        }
        void Save(svc::Handle h, u32 t, const char *name, u32 r) {
            Region &g = g_regs[r];
            if (g_saves[t][r & 15] >= 3) { return; }
            const u32 n = g_saves[t][r & 15]++;
            char path[128];
            std::snprintf(path, sizeof(path), "%s/ovl-%s-%010llx-%u.bin", ProbeDir, name, static_cast<unsigned long long>(g.base), n);
            static_cast<void>(fs::DeleteFile(path));
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(g.size)))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return; }
            for (u64 off = 0; off < g.size; off += Chunk) {
                const size_t len = static_cast<size_t>(g.size - off < Chunk ? g.size - off : Chunk);
                if (!DbgRead(h, g_buf, g.base + off, len)) { std::memset(g_buf, 0, len); }
                static_cast<void>(fs::WriteFile(f, static_cast<s64>(off), g_buf, len, fs::WriteOption::None));
                if ((off & 0xFFFFF) == 0) { Pump(h); }
            }
            static_cast<void>(fs::FlushFile(f));
            fs::CloseFile(f);
            LogLine("ovlprobe: %s saved %s", name, path);
        }
        void Window(u32 t) {
            const Target &tg = Targets[t];
            const u64 pid = PidOf(tg.program);
            if (pid == 0) { LogLine("ovlprobe: %s is not running", tg.name); return; }
            svc::Handle h = svc::InvalidHandle;
            const Result arc = svc::DebugActiveProcess(std::addressof(h), pid);
            if (R_FAILED(arc)) { LogLine("ovlprobe: %s (pid %llu) - DebugActiveProcess rc=0x%x", tg.name, static_cast<unsigned long long>(pid), arc.GetValue()); return; }
            u32 nev = 0;
            const Result crc = DbgResume(h, std::addressof(nev));
            LogLine("ovlprobe: %s (pid %llu) attached, %u events, continue rc=0x%x", tg.name, static_cast<unsigned long long>(pid), nev, crc.GetValue());
            g_pumped = 0;
            Map(h, tg.name);
            for (u32 r = 0; r < g_nregs; ++r) {
                const Region &g = g_regs[r];
                for (u64 off = 0; off < g.size; off += Block) {
                    g_hash[g.block0 + off / Block] = HashBlock(h, g.base + off, static_cast<size_t>(g.size - off < Block ? g.size - off : Block));
                }
                Pump(h);
            }
            const u64 start = armTicksToNs(armGetSystemTick());
            for (u32 sec = 0; sec < 20; ++sec) {
                u32 changed[MaxRegions] = {}, first[MaxRegions], last[MaxRegions] = {};
                for (u32 r = 0; r < MaxRegions; ++r) { first[r] = 0xFFFFFFFF; }
                for (u32 tick = 0; tick < 5; ++tick) {
                    const u64 due = start + (static_cast<u64>(sec) * 5 + tick + 1) * 200000000;
                    for (u32 r = 0; r < g_nregs; ++r) {
                        const Region &g = g_regs[r];
                        bool any = false;
                        for (u64 off = 0; off < g.size; off += Block) {
                            const u32 bi = static_cast<u32>(off / Block);
                            const u32 v = HashBlock(h, g.base + off, static_cast<size_t>(g.size - off < Block ? g.size - off : Block));
                            if (v != g_hash[g.block0 + bi]) {
                                g_hash[g.block0 + bi] = v;
                                ++changed[r]; any = true;
                                if (bi < first[r]) { first[r] = bi; }
                                if (bi > last[r]) { last[r] = bi; }
                            }
                        }
                        if (any) { Save(h, t, tg.name, r); }
                        Pump(h);
                    }
                    const u64 now = armTicksToNs(armGetSystemTick());
                    if (now < due) { os::SleepThread(TimeSpan::FromNanoSeconds(static_cast<s64>(due - now))); }
                }
                for (u32 r = 0; r < g_nregs; ++r) {
                    if (changed[r] == 0) { continue; }
                    LogLine("ovlprobe: %s %2u s: region %010llx changed %u block(s) in 5 samples, 64 KB blocks %u-%u",
                            tg.name, sec, static_cast<unsigned long long>(g_regs[r].base), changed[r], first[r], last[r]);
                }
            }
            svc::CloseHandle(h);
            LogLine("ovlprobe: %s detached (%u debug-event wakes)", tg.name, g_pumped);
        }

        void ProbeThread(void *) {
            os::SleepThread(TimeSpan::FromSeconds(40));
            if (!ProbeOn()) { return; }
            static_cast<void>(fs::CreateDirectory(ProbeDir));
            LogLine("ovlprobe: on (probe 1) - the system overlay and Tesla/Ultrahand, 20 s windows from ~70 s, for ~6 minutes");
            os::SleepThread(TimeSpan::FromSeconds(30));
            for (u32 round = 0; round < 6; ++round) {
                for (u32 t = 0; t < sizeof(Targets) / sizeof(Targets[0]); ++t) { Window(t); }
                os::SleepThread(TimeSpan::FromSeconds(10));
            }
            LogLine("ovlprobe: done");
        }

    }

    void OvlProbeStart() {
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_probe_thread), ProbeThread, nullptr, g_probe_stack, sizeof(g_probe_stack), os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_probe_thread), "applet-mitm.OvlProbe");
        os::StartThread(std::addressof(g_probe_thread));
        NoteThread(std::addressof(g_probe_thread));
    }

}

#else

namespace ams::mitm::applet {

    void OvlProbeStart() { }

}

#endif
