/*
 * The PC preview's stand-in for libnx: only what manager/source/main.cpp
 * uses, answering as a console that streams (preview.cpp sets the state).
 */
#pragma once
#include <cstdint>
#include <cstring>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
typedef u32 Result;
#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res) ((res) != 0)

namespace preview {
    inline bool g_module_running = true;
    inline u32 g_ip = 0;            /* nifm's address, little-endian bytes */
    inline u32 g_fw = 0;
    inline u64 g_buttons = 0, g_down = 0;
    inline const void *g_status = nullptr;   /* what sftap command 0 returns */
    inline size_t g_status_size = 0;
}

struct Service { u32 dummy; size_t pointer_buffer_size; };
inline Result smGetService(Service *s, const char *) { s->dummy = 1; return 0; }
inline void serviceClose(Service *) {}
template <typename T> inline Result serviceDispatchOut(Service *, u32 cmd, T &out) {
    if (cmd == 0 && preview::g_status != nullptr && preview::g_status_size == sizeof(T)) { std::memcpy(&out, preview::g_status, sizeof(T)); return 0; }
    return 1;
}
template <typename T> inline Result serviceDispatchIn(Service *, u32, const T &) { return 0; }
inline Result serviceDispatch(Service *, u32) { return 0; }

inline Result pmdmntInitialize() { return 0; }
inline void pmdmntExit() {}
inline Result pmdmntGetProcessId(u64 *pid, u64) { *pid = 1; return preview::g_module_running ? 0 : 1; }

enum NifmServiceType { NifmServiceType_User };
inline Result nifmInitialize(NifmServiceType) { return 0; }
inline void nifmExit() {}
inline Result nifmGetCurrentIpAddress(u32 *ip) { *ip = preview::g_ip; return preview::g_ip ? 0 : 1; }

typedef int SplConfigItem;
inline Result splInitialize() { return 0; }
inline void splExit() {}
inline Result splGetConfig(SplConfigItem, u64 *out) { *out = (UINT64_C(1) << 56) | (UINT64_C(12) << 48) | (UINT64_C(0) << 40); return 0; }

enum PlServiceType { PlServiceType_User };
enum PlSharedFontType { PlSharedFontType_Standard, PlSharedFontType_NintendoExt };
struct PlFontData { void *address; u32 size; };
inline Result plInitialize(PlServiceType) { return 0; }
inline void plExit() {}

inline u32 hosversionGet() { return preview::g_fw; }
#define MAKEHOSVERSION(_major, _minor, _micro) (((u32)(_major) << 16) | ((u32)(_minor) << 8) | (u32)(_micro))
#define HOSVER_MAJOR(_version) (((_version) >> 16) & 0xFF)
#define HOSVER_MINOR(_version) (((_version) >> 8) & 0xFF)
#define HOSVER_MICRO(_version) ((_version) & 0xFF)

enum : u64 {
    HidNpadButton_A = 1ull << 0, HidNpadButton_B = 1ull << 1, HidNpadButton_X = 1ull << 2, HidNpadButton_Y = 1ull << 3,
    HidNpadButton_StickL = 1ull << 4, HidNpadButton_StickR = 1ull << 5, HidNpadButton_L = 1ull << 6, HidNpadButton_R = 1ull << 7,
    HidNpadButton_ZL = 1ull << 8, HidNpadButton_ZR = 1ull << 9, HidNpadButton_Plus = 1ull << 10, HidNpadButton_Minus = 1ull << 11,
    HidNpadButton_Left = 1ull << 12, HidNpadButton_Up = 1ull << 13, HidNpadButton_Right = 1ull << 14, HidNpadButton_Down = 1ull << 15,
    HidNpadButton_StickLLeft = 1ull << 16, HidNpadButton_StickLUp = 1ull << 17, HidNpadButton_StickLRight = 1ull << 18, HidNpadButton_StickLDown = 1ull << 19,
};
enum { HidNpadStyleSet_NpadStandard = 0 };
struct PadState { u64 buttons; };
inline void padConfigureInput(u32, u32) {}
inline void padInitializeDefault(PadState *) {}
inline void padUpdate(PadState *) {}
inline u64 padGetButtons(const PadState *) { return preview::g_buttons; }
inline u64 padGetButtonsDown(const PadState *) { return preview::g_down; }

struct HidTouchState { u32 x, y; };
struct HidTouchScreenState { s32 count; HidTouchState touches[16]; };
inline void hidInitializeTouchScreen() {}
inline size_t hidGetTouchScreenStates(HidTouchScreenState *s, size_t) { s->count = 0; return 0; }

inline bool appletMainLoop() { return true; }
