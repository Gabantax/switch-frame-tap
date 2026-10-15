#include "applet_mitm_audio.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"

namespace ams::mitm::applet {

    constinit std::atomic<bool> g_audio_enabled{false};
    constinit std::atomic<bool> g_audio_active{false};
    constinit std::atomic<u32>  g_audio_state{0};

    namespace {

        /* grc:d replies 2212-0500 to a transfer when the continuous recorder
         * has not been started. grcdBegin may run only ONCE per boot - a second
         * call asserts inside grc - so, as SysDVR does, it is called only after
         * that reply, and at most once from this process. */
        constexpr u32 GrcdNotInitialized = 0x3E8D4;
        constinit bool g_begin_called = false;

        /* v0.7.13: Begin makes grc take ~7.2 MB of the System group for the
         * rest of the boot - the group every custom sysmodule shares, and am.
         * Called at boot, it left am short on consoles with a few more
         * sysmodules: am fataled with 2001-0132 (#15, GBAtemp; reproduced here
         * with 4 MB held, Run v0713-ballast4). Now only once a viewer streams,
         * and only with room to spare - otherwise that stream has no sound. */
        constexpr u64 GrcBeginBytes = UINT64_C(7680) * 1024;   /* measured 7228 KB */
        constexpr u64 SystemSpareBytes = UINT64_C(1024) * 1024;

        s64 SystemFreeBytes() {
            u64 cur = 0, lim = 0;
            if (g_pmdmnt_rc != 0 || R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_System, ::ams::svc::LimitableResource_PhysicalMemoryMax))) { return -1; }
            return static_cast<s64>(lim) - static_cast<s64>(cur);
        }

        /* one reader, one taker: a single-producer single-consumer byte ring */
        constexpr size_t RingSize = 64_KB;            /* ~340 ms of 48 kHz stereo s16 */
        alignas(64) constinit u8 g_ring[RingSize];
        constinit std::atomic<size_t> g_head{0};      /* written by the reader */
        constinit std::atomic<size_t> g_tail{0};      /* written by the taker */

        constexpr size_t ChunkSize = 0x1000;          /* what the official software asks for */
        alignas(0x1000) constinit u8 g_chunk[ChunkSize];

        alignas(os::ThreadStackAlignment) constinit u8 g_audio_stack[16_KB];
        constinit os::ThreadType g_audio_thread;

        void Push(const u8 *p, size_t n) {
            size_t head = g_head.load(std::memory_order_relaxed);
            const size_t tail = g_tail.load(std::memory_order_acquire);
            if (n > RingSize - 4) { return; }
            /* full: this chunk would overwrite unread sound - drop it, the
             * stream thread is behind (or not taking) */
            if (head - tail + n > RingSize) { return; }
            for (size_t i = 0; i < n; ) {
                const size_t at = (head + i) % RingSize;
                const size_t k = (RingSize - at) < (n - i) ? (RingSize - at) : (n - i);
                std::memcpy(g_ring + at, p + i, k);
                i += k;
            }
            g_head.store(head + n, std::memory_order_release);
        }

        ::Result Transfer(::Service *srv, u32 *size) {
            struct { u32 num_frames; u32 data_size; u64 start_timestamp; } out = {};
            const u32 stream = 1;   /* GrcStream_Audio */
            const ::Result rc = serviceDispatchInOut(srv, 2, stream, out,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { g_chunk, ChunkSize } },
            );
            *size = R_SUCCEEDED(rc) ? out.data_size : 0;
            return rc;
        }

        void AudioThread(void *) {
            ::Service srv = {};
            bool open = false;
            u32 errors = 0;
            bool refused = false;   /* this stream had no room for the recorder */
            for (;;) {
                const bool streaming = g_audio_active.load(std::memory_order_relaxed);
                if (!streaming) { refused = false; }
                /* the recorder only once a stream wants sound (see GrcBeginBytes) */
                if (!g_audio_enabled.load(std::memory_order_relaxed) || (!g_begin_called && (!streaming || refused))) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(300));
                    continue;
                }
                if (!open) {
                    SmUse sm_grc;
                    const ::Result rc = smGetService(&srv, "grc:d");
                    if (R_FAILED(rc)) {
                        g_audio_state.store(2, std::memory_order_relaxed);
                        LogLine("audio: grc:d unavailable rc=0x%x - no game audio", rc);
                        os::SleepThread(TimeSpan::FromSeconds(10));
                        continue;
                    }
                    open = true;
                    LogLine("audio: grc:d open");
                }
                u32 n = 0;
                ::Result rc = Transfer(&srv, &n);    /* blocks until the recorder has sound */
                if (rc == GrcdNotInitialized) {
                    if (g_begin_called) { os::SleepThread(TimeSpan::FromSeconds(1)); continue; }
                    const s64 free0 = SystemFreeBytes();
                    if (free0 >= 0 && static_cast<u64>(free0) < GrcBeginBytes + SystemSpareBytes) {
                        refused = true;
                        g_audio_state.store(4, std::memory_order_relaxed);
                        LogLine("audio: the console's recorder needs ~%llu KB of system memory and only %lld KB is free (other sysmodules use it) - this stream has no game audio",
                                static_cast<unsigned long long>((GrcBeginBytes + SystemSpareBytes) / 1024), static_cast<long long>(free0 / 1024));
                        continue;
                    }
                    g_begin_called = true;
                    rc = serviceDispatch(&srv, 1);    /* Begin: once per boot, never twice */
                    const s64 free1 = SystemFreeBytes();
                    LogLine("audio: grc:d Begin rc=0x%x (system memory free %lld KB -> %lld KB)", rc,
                            static_cast<long long>(free0 / 1024), static_cast<long long>(free1 / 1024));
                    if (R_FAILED(rc)) {
                        g_audio_state.store(3, std::memory_order_relaxed);
                        LogLine("audio: the recorder would not start - no game audio this boot");
                        for (;;) { os::SleepThread(TimeSpan::FromSeconds(3600)); }
                    }
                    continue;
                }
                if (R_FAILED(rc)) {
                    /* the game went away, or its recording is disabled */
                    if (++errors <= 5 || errors % 100 == 0) { LogLine("audio: transfer rc=0x%x (%u)", rc, errors); }
                    os::SleepThread(TimeSpan::FromMilliSeconds(200));
                    continue;
                }
                if (g_audio_state.load(std::memory_order_relaxed) != 1) {
                    g_audio_state.store(1, std::memory_order_relaxed);
                    LogLine("audio: game audio flowing (%u B chunks)", n);
                }
                /* read continuously (so a stream starts with current sound);
                 * keep it only while a stream runs */
                if (g_audio_active.load(std::memory_order_relaxed) && n != 0) { Push(g_chunk, n & ~3u); }
            }
        }

    }

    void StartAudio() {
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_audio_thread), AudioThread, nullptr,
                                        g_audio_stack, sizeof(g_audio_stack),
                                        os::GetThreadPriority(os::GetCurrentThread())));
        os::SetThreadNamePointer(std::addressof(g_audio_thread), "applet-mitm.Audio");
        os::StartThread(std::addressof(g_audio_thread));
    }

    size_t AudioTake(u8 *dst, size_t max) {
        const size_t head = g_head.load(std::memory_order_acquire);
        size_t tail = g_tail.load(std::memory_order_relaxed);
        size_t n = head - tail;
        if (n > max) { n = max; }
        n &= ~static_cast<size_t>(3);
        for (size_t i = 0; i < n; ) {
            const size_t at = (tail + i) % RingSize;
            const size_t k = (RingSize - at) < (n - i) ? (RingSize - at) : (n - i);
            std::memcpy(dst + i, g_ring + at, k);
            i += k;
        }
        g_tail.store(tail + n, std::memory_order_release);
        return n;
    }

    void AudioFlush() {
        g_tail.store(g_head.load(std::memory_order_acquire), std::memory_order_release);
    }

}
