/*
 * v0.7.14 sound probe 1: sound outside games. Research only - off unless
 * sdmc:/config/switch-frame-tap/probe-sound.txt exists, and left out of
 * release builds (SFT_SOUND_PROBE).
 *
 * grc:d's continuous recorder (applet_mitm_audio.cpp) records the running
 * game only. audrec:u's final output recorder records the console's final
 * mix, but nx-btred reports it carries games only too: plausibly each
 * process's "record volume" (aud:a 8/9, Get/SetAudioOutputProcessRecordVolume),
 * which the system may keep at 0 for the HOME menu and the applets.
 *
 * Per cycle (60 s, 4 cycles, 70 s after start):
 *   log    aud:a record and master volumes of qlaunch and the running game
 *   A 15 s audrec:u final output, as the system has it: peak / non-zero per s
 *   B 15 s the same with qlaunch's record volume raised to 1.0 (read back to
 *          check the request's layout), then restored
 * The first 3 cycles' A and B go to probe/audrec-cN-A.raw / -B.raw (s16
 * stereo 48 kHz, as received).
 */
#include <stratosphere.hpp>
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_service.hpp"
#include "applet_mitm_soundprobe.hpp"

#if SFT_SOUND_PROBE

namespace ams::mitm::applet {

    namespace {

        constexpr u64 QlaunchId = UINT64_C(0x0100000000001000);
        constexpr const char *ProbeFile = "sdmc:/config/switch-frame-tap/probe-sound.txt";
        constexpr const char *ProbeDir  = "sdmc:/config/switch-frame-tap/probe";

        constexpr size_t BufCount = 4;
        constexpr size_t BufSize  = 0x2000;   /* 2048 frames, ~43 ms */
        alignas(0x1000) constinit u8 g_bufs[BufCount][BufSize];

        /* nn::audio::FinalOutputRecorderBuffer (libnx's layout) */
        struct RecBuffer {
            u64 released_ns;
            u64 next_buffer_ptr;
            u64 sample_buffer_ptr;
            u64 sample_buffer_capacity;
            u64 data_size;
            u64 data_offset;
        };
        constinit RecBuffer g_recbuf[BufCount] = {};

        alignas(os::ThreadStackAlignment) constinit u8 g_probe_stack[16_KB];
        constinit os::ThreadType g_probe_thread;

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

        /* aud:a 8 GetAudioOutputProcessRecordVolume / 4 GetAudioOutputProcessMasterVolume (aruid -> float) */
        ::Result GetVolume(::Service *aud, u32 cmd, u64 aruid, float *out) {
            return serviceDispatchInOut(aud, cmd, aruid, *out);
        }

        /* aud:a 9 SetAudioOutputProcessRecordVolume(float, aruid, TimeSpan):
         * raw arguments in ascending alignment, the float first. Which of the
         * two 8-byte ones comes first is not known - layout 0: aruid, fade;
         * layout 1: fade, aruid. */
        ::Result SetRecordVolume(::Service *aud, int layout, u64 aruid, float vol) {
            const struct { float vol; u32 pad; u64 a; u64 b; } in = { vol, 0, layout == 0 ? aruid : 0, layout == 0 ? 0 : aruid };
            return serviceDispatchIn(aud, 9, in);
        }

        void LogVolumes(::Service *aud, const char *when, u64 ql, u64 app) {
            float qr = -1, qm = -1, ar = -1, am = -1;
            const ::Result r1 = GetVolume(aud, 8, ql, &qr);
            const ::Result r2 = GetVolume(aud, 4, ql, &qm);
            ::Result r3 = 0, r4 = 0;
            if (app != 0) { r3 = GetVolume(aud, 8, app, &ar); r4 = GetVolume(aud, 4, app, &am); }
            LogLine("soundprobe: %s - qlaunch (pid %llu) record %.2f (rc=0x%x) master %.2f (rc=0x%x); game (pid %llu) record %.2f (rc=0x%x) master %.2f (rc=0x%x)",
                    when, static_cast<unsigned long long>(ql), qr, r1, qm, r2, static_cast<unsigned long long>(app), ar, r3, am, r4);
        }

        struct Recorder {
            ::Service srv = {};
            ::Event ev = {};
            bool open = false;
        };

        bool OpenRecorder(Recorder *r) {
            ::Service mgr = {};
            ::Result rc = 0;
            {
                SmUse sm;
                rc = smGetService(&mgr, "audrec:u");
            }
            if (R_FAILED(rc)) { LogLine("soundprobe: audrec:u not opened rc=0x%x", rc); return false; }
            const struct { u32 sample_rate; u32 channel_count; u64 aruid; } in = { 48000, 2, 0 };
            struct { u32 sample_rate; u32 channel_count; u32 sample_format; u32 state; } out = {};
            rc = serviceDispatchInOut(&mgr, 0, in, out,
                .in_num_handles = 1,
                .in_handles = { CUR_PROCESS_HANDLE },
                .out_num_objects = 1,
                .out_objects = &r->srv,
            );
            serviceClose(&mgr);
            LogLine("soundprobe: OpenFinalOutputRecorder rc=0x%x - %u Hz, %u channels, format %u, state %u", rc, out.sample_rate, out.channel_count, out.sample_format, out.state);
            if (R_FAILED(rc)) { return false; }
            Handle h = INVALID_HANDLE;
            rc = serviceDispatch(&r->srv, 4, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &h);
            if (R_FAILED(rc)) { LogLine("soundprobe: RegisterBufferEvent rc=0x%x", rc); serviceClose(&r->srv); return false; }
            eventLoadRemote(&r->ev, h, true);
            for (size_t i = 0; i < BufCount; ++i) {
                g_recbuf[i] = {};
                g_recbuf[i].sample_buffer_ptr = reinterpret_cast<u64>(g_bufs[i]);
                g_recbuf[i].sample_buffer_capacity = BufSize;
                g_recbuf[i].data_size = BufSize;
                const u64 client = reinterpret_cast<u64>(&g_recbuf[i]);
                rc = serviceDispatchIn(&r->srv, 8, client,
                    .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
                    .buffers = { { &g_recbuf[i], sizeof(RecBuffer) } },
                );
                if (R_FAILED(rc)) { LogLine("soundprobe: AppendFinalOutputRecorderBuffer[%zu] rc=0x%x", i, rc); }
            }
            rc = serviceDispatch(&r->srv, 1);
            u32 state = 9;
            static_cast<void>(serviceDispatchOut(&r->srv, 0, state));
            LogLine("soundprobe: Start rc=0x%x, state %u", rc, state);
            if (R_FAILED(rc)) { eventClose(&r->ev); serviceClose(&r->srv); return false; }
            r->open = true;
            return true;
        }

        void CloseRecorder(Recorder *r) {
            if (!r->open) { return; }
            static_cast<void>(serviceDispatch(&r->srv, 2));
            eventClose(&r->ev);
            serviceClose(&r->srv);
            r->open = false;
        }

        /* records for ms, logs once a second, appends to path (nullptr: no file) */
        void Measure(Recorder *r, const char *tag, u32 ms, const char *path) {
            fs::FileHandle f;
            bool file = false;
            s64 off = 0;
            if (path != nullptr) {
                static_cast<void>(fs::DeleteFile(path));
                if (R_SUCCEEDED(fs::CreateFile(path, 0)) && R_SUCCEEDED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) { file = true; }
            }
            const u64 start = armTicksToNs(armGetSystemTick());
            u64 win_start = start;
            u32 win_bufs = 0, win_peak = 0;
            u64 win_bytes = 0, win_nonzero = 0, total_bytes = 0;
            u64 last_dsize = 0, last_rel = 0;
            for (;;) {
                const u64 now = armTicksToNs(armGetSystemTick());
                if (now - start >= static_cast<u64>(ms) * 1000000) { break; }
                if (now - win_start >= UINT64_C(1000000000)) {
                    LogLine("soundprobe: %s %4llu ms: %u buffers, %llu bytes, peak %u, non-zero samples %llu (last data_size %llu, released %llu)",
                            tag, static_cast<unsigned long long>((now - start) / 1000000), win_bufs, static_cast<unsigned long long>(win_bytes), win_peak,
                            static_cast<unsigned long long>(win_nonzero), static_cast<unsigned long long>(last_dsize), static_cast<unsigned long long>(last_rel));
                    win_start = now; win_bufs = 0; win_peak = 0; win_bytes = 0; win_nonzero = 0;
                }
                if (R_FAILED(eventWait(&r->ev, 200000000))) { continue; }
                u64 ptrs[BufCount] = {};
                struct { u32 count; u32 pad; u64 released; } out = {};
                const ::Result rc = serviceDispatchOut(&r->srv, 9, out,
                    .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
                    .buffers = { { ptrs, sizeof(ptrs) } },
                );
                if (R_FAILED(rc)) { LogLine("soundprobe: GetReleased rc=0x%x", rc); os::SleepThread(TimeSpan::FromMilliSeconds(100)); continue; }
                last_rel = out.released;
                for (u32 k = 0; k < out.count && k < BufCount; ++k) {
                    size_t i = BufCount;
                    for (size_t j = 0; j < BufCount; ++j) { if (ptrs[k] == reinterpret_cast<u64>(&g_recbuf[j])) { i = j; } }
                    if (i == BufCount) { LogLine("soundprobe: released an unknown buffer %#llx", static_cast<unsigned long long>(ptrs[k])); continue; }
                    const u64 dsize = g_recbuf[i].data_size;
                    last_dsize = dsize;
                    const size_t n = dsize != 0 && dsize <= BufSize ? static_cast<size_t>(dsize) : BufSize;
                    const s16 *s = reinterpret_cast<const s16 *>(g_bufs[i]);
                    for (size_t q = 0; q < n / 2; ++q) {
                        const u32 a = static_cast<u32>(s[q] < 0 ? -static_cast<s32>(s[q]) : s[q]);
                        if (a > win_peak) { win_peak = a; }
                        if (a != 0) { ++win_nonzero; }
                    }
                    ++win_bufs;
                    win_bytes += n;
                    total_bytes += n;
                    if (file) {
                        if (R_SUCCEEDED(fs::WriteFile(f, off, g_bufs[i], n, fs::WriteOption::None))) { off += static_cast<s64>(n); }
                    }
                    std::memset(g_bufs[i], 0, BufSize);
                    g_recbuf[i].data_size = BufSize;
                    g_recbuf[i].released_ns = 0;
                    const u64 client = reinterpret_cast<u64>(&g_recbuf[i]);
                    static_cast<void>(serviceDispatchIn(&r->srv, 8, client,
                        .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
                        .buffers = { { &g_recbuf[i], sizeof(RecBuffer) } },
                    ));
                }
            }
            if (file) {
                static_cast<void>(fs::FlushFile(f));
                fs::CloseFile(f);
            }
            LogLine("soundprobe: %s done - %llu bytes in %u ms%s%s", tag, static_cast<unsigned long long>(total_bytes), ms, file ? ", saved to " : "", file ? path : "");
        }

        void ProbeThread(void *) {
            os::SleepThread(TimeSpan::FromSeconds(40));
            if (!ProbeOn()) { return; }
            static_cast<void>(fs::CreateDirectory(ProbeDir));
            LogLine("soundprobe: on - 4 cycles of A (as is) / B (qlaunch record volume 1.0), 15 s each, 60 s apart, from 30 s on");
            os::SleepThread(TimeSpan::FromSeconds(30));
            int layout = 0;
            for (int c = 0; c < 4; ++c) {
                const u64 ql = PidOf(QlaunchId);
                const u64 app = g_app_pid.load(std::memory_order_relaxed);
                ::Service aud = {};
                ::Result arc = 0;
                {
                    SmUse sm;
                    arc = smGetService(&aud, "aud:a");
                }
                if (R_FAILED(arc)) { LogLine("soundprobe: aud:a not opened rc=0x%x", arc); }
                char tag[48];
                std::snprintf(tag, sizeof(tag), "cycle %d (%s)", c + 1, app != 0 ? "a game is running" : "no game");
                if (R_SUCCEEDED(arc)) { LogVolumes(&aud, tag, ql, app); }

                Recorder r;
                if (!OpenRecorder(&r)) {
                    if (R_SUCCEEDED(arc)) { serviceClose(&aud); }
                    os::SleepThread(TimeSpan::FromSeconds(60));
                    continue;
                }
                char path[96];
                std::snprintf(path, sizeof(path), "%s/audrec-c%d-A.raw", ProbeDir, c + 1);
                std::snprintf(tag, sizeof(tag), "c%d A", c + 1);
                Measure(&r, tag, 15000, c < 3 ? path : nullptr);

                float before = -1;
                if (R_SUCCEEDED(arc) && ql != 0 && R_SUCCEEDED(GetVolume(&aud, 8, ql, &before))) {
                    ::Result src = SetRecordVolume(&aud, layout, ql, 1.0f);
                    float now = -1;
                    static_cast<void>(GetVolume(&aud, 8, ql, &now));
                    LogLine("soundprobe: set qlaunch record volume 1.0 (layout %d) rc=0x%x - reads %.2f (was %.2f)", layout, src, now, before);
                    if (now != 1.0f && layout == 0) {
                        layout = 1;
                        src = SetRecordVolume(&aud, layout, ql, 1.0f);
                        static_cast<void>(GetVolume(&aud, 8, ql, &now));
                        LogLine("soundprobe: set qlaunch record volume 1.0 (layout %d) rc=0x%x - reads %.2f", layout, src, now);
                    }
                    std::snprintf(path, sizeof(path), "%s/audrec-c%d-B.raw", ProbeDir, c + 1);
                    std::snprintf(tag, sizeof(tag), "c%d B", c + 1);
                    Measure(&r, tag, 15000, c < 3 ? path : nullptr);
                    const ::Result rrc = SetRecordVolume(&aud, layout, ql, before);
                    static_cast<void>(GetVolume(&aud, 8, ql, &now));
                    LogLine("soundprobe: qlaunch record volume restored rc=0x%x - reads %.2f", rrc, now);
                }
                CloseRecorder(&r);
                if (R_SUCCEEDED(arc)) { serviceClose(&aud); }
                os::SleepThread(TimeSpan::FromSeconds(30));
            }
            LogLine("soundprobe: done");
        }

    }

    void SoundProbeStart() {
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_probe_thread), ProbeThread, nullptr, g_probe_stack, sizeof(g_probe_stack), os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_probe_thread), "applet-mitm.SndProbe");
        os::StartThread(std::addressof(g_probe_thread));
        NoteThread(std::addressof(g_probe_thread));
    }

}

#else

namespace ams::mitm::applet {

    void SoundProbeStart() { }

}

#endif
