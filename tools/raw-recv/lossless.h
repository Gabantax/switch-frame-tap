/* v0.7.15: Lossless Scaling - upscaling and frame generation on the PC (the
 * console keeps sending its own picture; nothing changes on the Switch).
 *
 * Windows: Lossless Scaling (the Steam app, THS) scales a window it captures.
 *   The Upscaling setting "Lossless" (U) draws the stream 1:1 in a window of
 *   the stream's own size (1280x720 / 1920x1080) - no SDL scaling, none of
 *   the viewer's upscalers - so LS gets the console's real pixels and does
 *   the upscaling (LS1, FSR, NIS, SGSR, ... and LSFG if it is on, all as set
 *   in LS itself). With ls_auto=1 (the default) the viewer starts LS if it
 *   is not running and presses its Scale hotkey (ls_hotkey=, LS's default
 *   Ctrl+Alt+S) once the picture is up; F11 / double-click then switch LS's
 *   scaling on and off instead of the viewer's own fullscreen.
 *   viewer.ini: ls_auto=0|1, ls_hotkey=ctrl+alt+s, ls_path=C:\...\LosslessScaling.exe
 *
 * Linux: Lossless Scaling itself does not run here; its frame generation
 *   (LSFG) does, through lsfg-vk (a Vulkan layer that uses the shaders of the
 *   Steam install). lsfg-vk only sees Vulkan, and the viewer draws with
 *   OpenGL, so Frame generation on (G, Settings; viewer.ini lsfg=0|1) starts
 *   the viewer with Mesa's Zink (OpenGL on Vulkan). Everything else is
 *   lsfg-vk's own: it picks its conf.toml profile by the program's name
 *   (active_in) - the viewer adds one, "Switch Frame Tap", the first time -
 *   and multiplier, flow scale, pacing are set in lsfg-vk-ui. The viewer
 *   reads lsfg-vk's messages to say whether it generates. The upscaling stays
 *   the viewer's FSR 1 / NIS (U), below the generated frames. The driver is
 *   picked when the process starts, so changing the setting restarts it.
 *
 * macOS: neither exists; everything here is a no-op.
 */
#pragma once
#include <ctype.h>
#ifdef _WIN32
#include <tlhelp32.h>
#include <shellapi.h>
#elif !defined(__APPLE__)
#include <SDL2/SDL_opengl.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

static void prefs_path(char *out, size_t cap);
static void hud_toast(const char *t);

#ifndef __APPLE__
static int ls_file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* rel under some Steam library's steamapps/common: the roots themselves, then
 * every "path" in their libraryfolders.vdf (backslashes come doubled there) */
static int ls_steam_find(const char *const *roots, int nroots, const char *rel, char *out, size_t cap)
{
    for (int i = 0; i < nroots; i++) {
        if (!roots[i] || !roots[i][0]) continue;
        snprintf(out, cap, "%s/steamapps/common/%s", roots[i], rel);
        if (ls_file_exists(out)) return 1;
        char vdf[1024];
        snprintf(vdf, sizeof(vdf), "%s/steamapps/libraryfolders.vdf", roots[i]);
        FILE *f = fopen(vdf, "r");
        if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *k = strstr(line, "\"path\"");
            if (!k) continue;
            char *a = strchr(k + 6, '"');
            if (!a) continue;
            char lib[768]; size_t n = 0;
            for (char *s = a + 1; *s && *s != '"' && n + 1 < sizeof(lib); s++) {
                if (s[0] == '\\' && s[1] == '\\') s++;
                lib[n++] = *s;
            }
            lib[n] = 0;
            snprintf(out, cap, "%s/steamapps/common/%s", lib, rel);
            if (ls_file_exists(out)) { fclose(f); return 1; }
        }
        fclose(f);
    }
    out[0] = 0;
    return 0;
}
#endif

/* ======================================================================== */
#ifdef _WIN32

static int g_ls_auto = 1;                      /* viewer.ini ls_auto= */
static char g_ls_hotkey[48] = "ctrl+alt+s";    /* viewer.ini ls_hotkey= (LS's Scale hotkey) */
static char g_ls_path[512] = "";               /* viewer.ini ls_path= (else found through Steam) */
static struct {
    int on;                  /* we pressed the hotkey to start scaling (and not again since) */
    int launched, failed;    /* we started LS; it never came up */
    int manual;              /* F11 took over: no more automatic hotkey presses */
    Uint64 t_launch, t_seen, t_poll, t_picture;
    int running;
    int w, h;                /* the stream's size, for the 1:1 window */
    int pre_valid, pre_x, pre_y, pre_w, pre_h;   /* the window before "Lossless" */
} g_ls;

static double ls_secs(Uint64 since)
{
    return (double)(SDL_GetPerformanceCounter() - since) / (double)SDL_GetPerformanceFrequency();
}

static int ls_is_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    int found = 0;
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
        if (!_stricmp(pe.szExeFile, "LosslessScaling.exe")) found = 1;
    CloseHandle(snap);
    return found;
}

static int ls_find_exe(char *out, size_t cap)
{
    if (g_ls_path[0] && ls_file_exists(g_ls_path)) { snprintf(out, cap, "%s", g_ls_path); return 1; }
    char steam[512] = "", steam_lm[512] = "";
    DWORD sz = sizeof(steam);
    RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz);
    sz = sizeof(steam_lm);
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath", RRF_RT_REG_SZ, NULL, steam_lm, &sz);
    const char *roots[] = { steam, steam_lm, "C:/Program Files (x86)/Steam" };
    return ls_steam_find(roots, 3, "Lossless Scaling/LosslessScaling.exe", out, cap);
}

static int ls_launch(void)
{
    char exe[1024];
    if (ls_find_exe(exe, sizeof(exe))) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", exe);
        char *slash = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
        if (bs > slash) slash = bs;
        if (slash) *slash = 0;
        fprintf(stderr, "\nlossless: starting %s\n", exe);
        if ((INT_PTR)ShellExecuteA(NULL, "open", exe, NULL, dir, SW_SHOWMINNOACTIVE) > 32) return 1;
    }
    /* not found where Steam keeps it: let Steam start it (app 993090) */
    fprintf(stderr, "\nlossless: starting it through Steam\n");
    return (INT_PTR)ShellExecuteA(NULL, "open", "steam://rungameid/993090", NULL, NULL, SW_SHOWNORMAL) > 32;
}

/* "ctrl+alt+s", "ctrl+shift+f12", "alt+home" ... -> modifiers and one key */
static int ls_parse_hotkey(WORD *vk, WORD mods[3], int *nmods)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%s", g_ls_hotkey);
    *nmods = 0; *vk = 0;
    for (char *tok = strtok(buf, "+ "); tok; tok = strtok(NULL, "+ ")) {
        for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(tok, "ctrl") || !strcmp(tok, "control")) { if (*nmods < 3) mods[(*nmods)++] = VK_CONTROL; }
        else if (!strcmp(tok, "alt")) { if (*nmods < 3) mods[(*nmods)++] = VK_MENU; }
        else if (!strcmp(tok, "shift")) { if (*nmods < 3) mods[(*nmods)++] = VK_SHIFT; }
        else if (strlen(tok) == 1 && isalnum((unsigned char)tok[0])) *vk = (WORD)toupper((unsigned char)tok[0]);
        else if (tok[0] == 'f' && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 24) *vk = (WORD)(VK_F1 + atoi(tok + 1) - 1);
        else if (!strcmp(tok, "home")) *vk = VK_HOME;
        else if (!strcmp(tok, "end")) *vk = VK_END;
        else if (!strcmp(tok, "insert")) *vk = VK_INSERT;
        else if (!strcmp(tok, "pageup")) *vk = VK_PRIOR;
        else if (!strcmp(tok, "pagedown")) *vk = VK_NEXT;
        else if (!strcmp(tok, "pause")) *vk = VK_PAUSE;
        else if (!strcmp(tok, "scrolllock")) *vk = VK_SCROLL;
    }
    return *vk != 0;
}

/* LS's hotkey is global: pressed while our window has the focus, LS scales it */
static int ls_press_hotkey(SDL_Window *win)
{
    WORD vk, mods[3];
    int nm;
    if (!ls_parse_hotkey(&vk, mods, &nm)) {
        fprintf(stderr, "\nlossless: ls_hotkey=%s is not a key this viewer can press\n", g_ls_hotkey);
        return 0;
    }
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if (win && SDL_GetWindowWMInfo(win, &wmi)) SetForegroundWindow(wmi.info.win.window);
    INPUT in[8];
    int n = 0;
    memset(in, 0, sizeof(in));
    for (int i = 0; i < nm; i++) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = mods[i]; n++; }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    for (int i = nm - 1; i >= 0; i--) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = mods[i]; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    const UINT sent = SendInput((UINT)n, in, sizeof(INPUT));
    fprintf(stderr, "\nlossless: pressed %s (%u of %d events)\n", g_ls_hotkey, sent, n);
    return sent == (UINT)n;
}

/* the window at the stream's own size: LS scales the window's pixels, so
 * anything bigger would be SDL's linear scale upscaled a second time */
static void ls_fit_window(SDL_Window *win)
{
    if (!win || g_ls.w <= 0 || g_ls.h <= 0) return;
    const Uint32 fl = SDL_GetWindowFlags(win);
    if (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) SDL_SetWindowFullscreen(win, 0);
    if (fl & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(win);
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    if (w == g_ls.w && h == g_ls.h) return;
    if (!g_ls.pre_valid) {
        SDL_GetWindowPosition(win, &g_ls.pre_x, &g_ls.pre_y);
        g_ls.pre_w = w; g_ls.pre_h = h;
        g_ls.pre_valid = 1;
    }
    SDL_SetWindowSize(win, g_ls.w, g_ls.h);
    fprintf(stderr, "\nlossless: window %dx%d (1:1 with the stream)\n", g_ls.w, g_ls.h);
}

static void ls_unfit_window(SDL_Window *win)
{
    if (!win || !g_ls.pre_valid) return;
    g_ls.pre_valid = 0;
    SDL_SetWindowSize(win, g_ls.pre_w, g_ls.pre_h);
    SDL_SetWindowPosition(win, g_ls.pre_x, g_ls.pre_y);
}

/* F11 / double-click in "Lossless": LS's scaling on or off */
static void ls_toggle(SDL_Window *win)
{
    ls_fit_window(win);
    g_ls.manual = 1;
    if (!ls_is_running()) {
        if (ls_launch()) { g_ls.launched = 1; g_ls.failed = 0; g_ls.t_launch = SDL_GetPerformanceCounter(); g_ls.t_seen = 0; }
        hud_toast("Starting Lossless Scaling...");
        return;
    }
    if (ls_press_hotkey(win)) {
        g_ls.on = !g_ls.on;
        hud_toast(g_ls.on ? "Lossless Scaling on (F11 stops it)" : "Lossless Scaling off (F11 starts it)");
    }
}

/* the upscaler choice changed (raw-view.c act_upscaler) */
static void ls_mode_changed(SDL_Window *win, int now_ls)
{
    if (now_ls) {
        g_ls.failed = 0;
        g_ls.manual = 0;
        g_ls.t_picture = 0;
        ls_fit_window(win);
        return;
    }
    if (g_ls.on && ls_is_running()) ls_press_hotkey(win);   /* LS stops scaling us */
    g_ls.on = 0;
    ls_unfit_window(win);
}

/* every pass of the live loop: start LS and its scaling once there is a picture */
static void ls_tick(SDL_Window *win, int mode_ls, int picture)
{
    if (!mode_ls || !win) return;
    if (!picture) { g_ls.t_picture = 0; return; }
    if (!g_ls.t_picture) g_ls.t_picture = SDL_GetPerformanceCounter();
    if (!g_ls_auto || g_ls.on || g_ls.failed || g_ls.manual) return;
    if (g_ls.t_poll && ls_secs(g_ls.t_poll) < 0.5) return;
    g_ls.t_poll = SDL_GetPerformanceCounter();
    const int running = ls_is_running();
    if (running && !g_ls.running) g_ls.t_seen = g_ls.t_poll;
    g_ls.running = running;
    if (!running) {
        if (!g_ls.launched) {
            if (ls_launch()) {
                g_ls.launched = 1;
                g_ls.t_launch = g_ls.t_poll;
                hud_toast("Starting Lossless Scaling...");
            } else {
                g_ls.failed = 1;
                hud_toast("Lossless Scaling not found - install it from Steam, or set ls_path= in viewer.ini");
            }
        } else if (ls_secs(g_ls.t_launch) > 30) {
            g_ls.failed = 1;
            hud_toast("Lossless Scaling did not start - start it, then press F11");
        }
        return;
    }
    /* LS is up: give a fresh start a few seconds to register its hotkey, the
     * picture a second on screen, and wait for the window to have the focus */
    if (g_ls.launched && ls_secs(g_ls.t_seen) < 4.0) return;
    if (ls_secs(g_ls.t_picture) < 1.0) return;
    if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) return;
    ls_fit_window(win);
    if (ls_press_hotkey(win)) {
        g_ls.on = 1;
        hud_toast("Lossless Scaling on (F11 stops it)");
    } else {
        g_ls.failed = 1;
    }
}

static void ls_set_stream_size(SDL_Window *win, int w, int h, int mode_ls)
{
    if (w == g_ls.w && h == g_ls.h) return;
    g_ls.w = w; g_ls.h = h;
    if (mode_ls) ls_fit_window(win);
}

static void ls_describe(char *out, size_t cap)
{
    if (g_ls.on) snprintf(out, cap, "Lossless Scaling is scaling the %dx%d picture (F11 stops)", g_ls.w, g_ls.h);
    else if (g_ls.failed) snprintf(out, cap, "Lossless Scaling: start it, then F11 (hotkey %s)", g_ls_hotkey);
    else if (!g_ls_auto) snprintf(out, cap, "Lossless Scaling: press F11 or %s to scale", g_ls_hotkey);
    else snprintf(out, cap, "Lossless Scaling: starts with the picture (U)");
}

static void ls_prefs_line(const char *line)
{
    int v;
    if (sscanf(line, "ls_auto=%d", &v) == 1) g_ls_auto = v != 0;
    if (sscanf(line, "ls_hotkey=%47[^\r\n]", g_ls_hotkey) == 1) {}
    if (sscanf(line, "ls_path=%511[^\r\n]", g_ls_path) == 1) {}
}

static void ls_prefs_save(FILE *f)
{
    fprintf(f, "ls_auto=%d\nls_hotkey=%s\n", g_ls_auto, g_ls_hotkey);
    if (g_ls_path[0]) fprintf(f, "ls_path=%s\n", g_ls_path);
}

#else /* ================================================================= */

static void ls_prefs_line(const char *line) { (void)line; }
static void ls_prefs_save(FILE *f) { (void)f; }
static void ls_tick(SDL_Window *win, int mode_ls, int picture) { (void)win; (void)mode_ls; (void)picture; }
static void ls_set_stream_size(SDL_Window *win, int w, int h, int mode_ls) { (void)win; (void)w; (void)h; (void)mode_ls; }

#endif

/* ======================================================================== */
#if !defined(_WIN32) && !defined(__APPLE__)

#define LSFG_AVAILABLE 1
static const int lsfg_mult[] = { 0, 1 };                  /* the Settings segments: off / on */
#define LSFG_N ((int)(sizeof(lsfg_mult) / sizeof(lsfg_mult[0])))
static int g_lsfg = 0;                 /* viewer.ini lsfg=0|1 */
static int g_lsfg_started = 0;         /* what this process was started with (the driver is fixed now) */
static char **g_lsfg_argv = NULL;      /* for the restart */
static int g_lsfg_restart = 0;         /* the live loop restarts the viewer (1 = setting, 2 = lsfg-vk failed) */
static int g_lsfg_err_fd = -1;         /* the real stderr, while lsfg_tee reads lsfg-vk's messages */
/* the names lsfg-vk sees (/proc/self/exe, /proc/self/comm): a build from
 * source, the launcher's and the Linux download's / Flatpak's */
static const char *const lsfg_names = "[\"raw-view\", \"switch-frame-tap-viewer\"]";
#define LSFG_PROFILE_NAME "Switch Frame Tap"
static struct {
    int probed, zink, layer, dll, blocked, added;
    int state;               /* 0 not known yet, 1 generating, -1 failed or not set up */
    int said;                /* the toast for the state above shown */
    Uint64 t_poll, t_init, t_loaded;
    char gl[96], dll_path[1024], conf[1024], why[200];
    /* from lsfg-vk's messages (lsfg_tee, another thread: g_lsfg_mx) */
    int loaded, init, mult;
    char profile[96], err[200];
} g_lsfg_st;
static SDL_mutex *g_lsfg_mx;

/* lsfg-vk 2.x reads lsfg-vk.dll only; 1.x Lossless.dll */
static int lsfg_dll_ok(void) { return g_lsfg_st.dll == 2 || (g_lsfg_st.dll == 1 && g_lsfg_st.layer < 2); }

static int lsfg_index(int v) { return v ? 1 : 0; }

static void lsfg_prefs_line(const char *line)
{
    int v;
    if (sscanf(line, "lsfg=%d", &v) == 1) g_lsfg = v != 0;   /* 2/3/4 from v0.7.15 test builds: on */
}

static void lsfg_prefs_save(FILE *f) { fprintf(f, "lsfg=%d\n", g_lsfg); }

/* lsfg-vk's own settings file (its lsfg-vk-ui edits the same one) */
static void lsfg_conf_path(char *out, size_t cap)
{
    const char *c = getenv("LSFGVK_CONFIG"), *x = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (c && c[0]) snprintf(out, cap, "%s", c);
    else if (x && x[0]) snprintf(out, cap, "%s/lsfg-vk/conf.toml", x);
    else snprintf(out, cap, "%s/.config/lsfg-vk/conf.toml", home ? home : "");
}

/* lsfg-vk picks its profile by the program's name (active_in). Without one
 * for the viewer it does nothing - so the first time, the viewer adds one;
 * everything else (multiplier, flow scale, pacing...) is set in lsfg-vk-ui.
 * Also reads [global] dll = "..." for the check below. */
static void lsfg_conf_ensure(char *dll, size_t dcap)
{
    lsfg_conf_path(g_lsfg_st.conf, sizeof(g_lsfg_st.conf));
    dll[0] = 0;
    if (getenv("LSFGVK_PROFILE")) return;                     /* the user picks it by hand */
    FILE *f = fopen(g_lsfg_st.conf, "r");
    int has = 0, empty = 1;
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "raw-view") || strstr(line, "switch-frame-tap-viewer")) has = 1;
            if (line[strspn(line, " \t\r\n")]) empty = 0;
            const char *k = line + strspn(line, " \t");
            if (!strncmp(k, "dll", 3) && strchr(k, '=')) {
                const char *q = strchr(k, '"');
                if (q) { snprintf(dll, dcap, "%s", q + 1); char *z = strrchr(dll, '"'); if (z) *z = 0; }
            }
        }
        fclose(f);
    }
    if (has) return;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", g_lsfg_st.conf);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    f = fopen(g_lsfg_st.conf, "a");
    if (!f) { fprintf(stderr, "frame generation: cannot write %s\n", g_lsfg_st.conf); return; }
    if (empty) fprintf(f, "version = 2\n\n[global]\nallow_fp16 = true\n");
    fprintf(f, "\n[[profile]]\nname = \"" LSFG_PROFILE_NAME "\"\nactive_in = %s\nmultiplier = 2\n"
               "flow_scale = 1.0\nperformance_mode = false\npacing_mode = \"vsync\"\n"
               "override_present_mode = true\npreserve_swapchain_image_count = false\n", lsfg_names);
    fclose(f);
    g_lsfg_st.added = 1;
    fprintf(stderr, "frame generation: added the profile \"" LSFG_PROFILE_NAME "\" to %s\n", g_lsfg_st.conf);
}

/* one line lsfg-vk wrote to stderr */
static void lsfg_tee_line(const char *l)
{
    if (!strstr(l, "(lsfg-vk)")) return;
    SDL_LockMutex(g_lsfg_mx);
    const char *p;
    if (strstr(l, "Loaded lsfg-vk layer")) g_lsfg_st.loaded = 1;
    if (strstr(l, "Initializing lsfg-vk instance")) g_lsfg_st.init = 1;
    if ((p = strstr(l, "Using profile with name '"))) {
        snprintf(g_lsfg_st.profile, sizeof(g_lsfg_st.profile), "%s", p + 25);
        char *q = strchr(g_lsfg_st.profile, '\'');
        if (q) *q = 0;
    }
    if ((p = strstr(l, "Multiplier: "))) g_lsfg_st.mult = atoi(p + 12);
    if ((p = strstr(l, "[ERROR]: - ")) && !g_lsfg_st.err[0]) {
        snprintf(g_lsfg_st.err, sizeof(g_lsfg_st.err), "%s", p + 11);
        g_lsfg_st.err[strcspn(g_lsfg_st.err, "\r\n")] = 0;
    }
    SDL_UnlockMutex(g_lsfg_mx);
}

/* stderr through a pipe: everything still goes to the terminal / log, and
 * lsfg-vk's own messages (it has no other way to say what it does) are read */
static int lsfg_tee_main(void *arg)
{
    const int rfd = (int)(intptr_t)arg;
    char buf[4096], line[1024];
    size_t n = 0;
    for (;;) {
        const ssize_t got = read(rfd, buf, sizeof(buf));
        if (got <= 0) break;
        for (ssize_t off = 0; off < got; ) {
            const ssize_t w = write(g_lsfg_err_fd, buf + off, (size_t)(got - off));
            if (w <= 0) break;
            off += w;
        }
        for (ssize_t i = 0; i < got; i++) {
            if (buf[i] == '\n' || n + 1 >= sizeof(line)) { line[n] = 0; lsfg_tee_line(line); n = 0; }
            else line[n++] = buf[i];
        }
    }
    return 0;
}

static void lsfg_tee_start(void)
{
    int fds[2];
    if (pipe(fds) != 0) return;
    g_lsfg_err_fd = dup(2);
    if (g_lsfg_err_fd < 0 || dup2(fds[1], 2) < 0) { close(fds[0]); close(fds[1]); return; }
    close(fds[1]);
    g_lsfg_mx = SDL_CreateMutex();
    SDL_Thread *t = SDL_CreateThread(lsfg_tee_main, "lsfg-log", (void *)(intptr_t)fds[0]);
    if (t) SDL_DetachThread(t);
}

/* lsfg-vk's layer manifest in dir: its implementation_version (1, 2, ...), 0 = none */
static int lsfg_dir_has_layer(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int ver = 0;
    for (struct dirent *e; !ver && (e = readdir(d)); ) {
        char n[256];
        snprintf(n, sizeof(n), "%s", e->d_name);
        for (char *c = n; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strstr(n, "lsfg") || !strstr(n, ".json")) continue;
        ver = 1;
        char p[1024], buf[2048];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
        const char *iv = strstr(buf, "\"implementation_version\"");
        const char *q = iv ? strchr(iv + 24, '"') : NULL;
        if (q && atoi(q + 1) > 0) ver = atoi(q + 1);
    }
    closedir(d);
    return ver;
}

/* is lsfg-vk's implicit layer where the Vulkan loader looks, and which
 * version? (only for the note in Settings - the loader decides) */
static int lsfg_layer_installed(void)
{
    char p[1024];
    int v;
    const char *home = getenv("HOME"), *xdh = getenv("XDG_DATA_HOME"), *xdd = getenv("XDG_DATA_DIRS");
    if (xdh && xdh[0]) snprintf(p, sizeof(p), "%s/vulkan/implicit_layer.d", xdh);
    else snprintf(p, sizeof(p), "%s/.local/share/vulkan/implicit_layer.d", home ? home : "");
    if ((v = lsfg_dir_has_layer(p))) return v;
    snprintf(p, sizeof(p), "%s/.local/share/vulkan/implicit_layer.d", home ? home : "");
    if ((v = lsfg_dir_has_layer(p))) return v;
    char dirs[2048];
    snprintf(dirs, sizeof(dirs), "%s", xdd && xdd[0] ? xdd : "/usr/local/share:/usr/share");
    for (char *t = strtok(dirs, ":"); t; t = strtok(NULL, ":")) {
        snprintf(p, sizeof(p), "%s/vulkan/implicit_layer.d", t);
        if ((v = lsfg_dir_has_layer(p))) return v;
    }
    if ((v = lsfg_dir_has_layer("/etc/vulkan/implicit_layer.d"))) return v;
    if ((v = lsfg_dir_has_layer("/usr/share/vulkan/implicit_layer.d"))) return v;
    /* Flatpak: org.freedesktop.Platform.VulkanLayer.lsfgvk */
    if ((v = lsfg_dir_has_layer("/usr/lib/extensions/vulkan/share/vulkan/implicit_layer.d"))) return v;
    DIR *d = opendir("/usr/lib/extensions/vulkan");
    if (d) {
        for (struct dirent *e; !v && (e = readdir(d)); ) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), "/usr/lib/extensions/vulkan/%s/share/vulkan/implicit_layer.d", e->d_name);
            v = lsfg_dir_has_layer(p);
        }
        closedir(d);
    }
    return v;
}

/* the shaders: lsfg-vk 2.x wants lsfg-vk.dll, which Lossless Scaling's
 * "lsfg-vk" beta branch on Steam installs; 1.x read Lossless.dll.
 * 2 = lsfg-vk.dll (or a path given), 1 = only Lossless.dll, 0 = neither */
static int lsfg_find_dll(const char *conf_dll, char *out, size_t cap)
{
    const char *env = getenv("LSFGVK_DLL_PATH");
    if (conf_dll && conf_dll[0] && ls_file_exists(conf_dll)) { snprintf(out, cap, "%s", conf_dll); return 2; }
    if (env && ls_file_exists(env)) { snprintf(out, cap, "%s", env); return 2; }
    const char *home = getenv("HOME");
    char r[5][512];
    snprintf(r[0], sizeof(r[0]), "%s/.local/share/Steam", home ? home : "");
    snprintf(r[1], sizeof(r[1]), "%s/.steam/steam", home ? home : "");
    snprintf(r[2], sizeof(r[2]), "%s/.steam/root", home ? home : "");
    snprintf(r[3], sizeof(r[3]), "%s/.var/app/com.valvesoftware.Steam/.local/share/Steam", home ? home : "");
    snprintf(r[4], sizeof(r[4]), "%s/snap/steam/common/.local/share/Steam", home ? home : "");
    const char *roots[5] = { r[0], r[1], r[2], r[3], r[4] };
    if (ls_steam_find(roots, 5, "Lossless Scaling/lsfg-vk.dll", out, cap)) return 2;
    return ls_steam_find(roots, 5, "Lossless Scaling/Lossless.dll", out, cap) ? 1 : 0;
}

static void lsfg_setenv(const char *k, const char *v) { setenv(k, v, 0); }   /* the user's own wins */

/* main(), before anything starts SDL's video (the OpenGL library is loaded
 * when the first window is made, so the driver is still open to choose) */
static void lsfg_env_setup(char **argv)
{
    g_lsfg_argv = argv;
    char path[1024];
    prefs_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        char line[600];
        while (fgets(line, sizeof(line), f)) lsfg_prefs_line(line);
        fclose(f);
    }
    for (int i = 1; argv[i]; i++)                       /* --lsfg 0|1 (tests, launchers) */
        if (!strcmp(argv[i], "--lsfg") && argv[i + 1]) g_lsfg = atoi(argv[i + 1]) != 0;
    g_lsfg_started = g_lsfg;
    if (!g_lsfg) return;
    /* the run before saw lsfg-vk fail (lsfg_poll): this one goes without */
    const char *failed = getenv("SFT_LSFG_FAILED");
    if (failed && failed[0]) {
        g_lsfg_st.blocked = 1;
        g_lsfg_st.state = -1;
        snprintf(g_lsfg_st.why, sizeof(g_lsfg_st.why), "%s", failed);
        unsetenv("SFT_LSFG_FAILED");
        lsfg_setenv("DISABLE_LSFGVK", "1");
        fprintf(stderr, "frame generation: off for this run - lsfg-vk failed: %s\n", g_lsfg_st.why);
        return;
    }
    /* Flatpak: the sandbox's XDG_CONFIG_HOME is not where lsfg-vk-ui (on the
     * host) keeps conf.toml - point lsfg-vk at that one (finish-args share it) */
    if (getenv("FLATPAK_ID") && !getenv("LSFGVK_CONFIG") && getenv("HOME")) {
        char hc[1024];
        snprintf(hc, sizeof(hc), "%s/.config/lsfg-vk/conf.toml", getenv("HOME"));
        setenv("LSFGVK_CONFIG", hc, 1);
    }
    char conf_dll[1024];
    lsfg_conf_ensure(conf_dll, sizeof(conf_dll));
    /* without the layer or its shaders lsfg-vk fails the window's swap chain
     * (a black window): then the viewer runs as usual and Settings says why */
    g_lsfg_st.layer = lsfg_layer_installed();
    if (!g_lsfg_st.layer && (getenv("VK_ADD_IMPLICIT_LAYER_PATH") || getenv("VK_IMPLICIT_LAYER_PATH"))) g_lsfg_st.layer = 1;
    g_lsfg_st.dll = lsfg_find_dll(conf_dll, g_lsfg_st.dll_path, sizeof(g_lsfg_st.dll_path));
    if (!g_lsfg_st.layer || !lsfg_dll_ok()) {
        g_lsfg_st.blocked = 1;
        g_lsfg_st.state = -1;
        snprintf(g_lsfg_st.why, sizeof(g_lsfg_st.why), "%s",
                 !g_lsfg_st.layer ? "lsfg-vk is not installed" :
                 !g_lsfg_st.dll ? "Lossless Scaling is not installed in Steam" :
                 "needs the \"lsfg-vk\" beta of Lossless Scaling (Steam > Properties > Betas)");
        lsfg_setenv("DISABLE_LSFGVK", "1");
        fprintf(stderr, "frame generation: asked for, but %s - off for now\n", g_lsfg_st.why);
        return;
    }
    /* OpenGL through Mesa's Zink, so the Vulkan layer sees the frames;
     * lsfg-vk then reads its own conf.toml, as for any game */
    lsfg_setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa");
    lsfg_setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink");
    static const char *const egl_mesa[] = { "/usr/share/glvnd/egl_vendor.d/50_mesa.json",
                                            "/usr/lib/x86_64-linux-gnu/GL/default/share/glvnd/egl_vendor.d/50_mesa.json" };
    for (size_t i = 0; i < sizeof(egl_mesa) / sizeof(egl_mesa[0]); i++)
        if (ls_file_exists(egl_mesa[i])) { lsfg_setenv("__EGL_VENDOR_LIBRARY_FILENAMES", egl_mesa[i]); break; }
    unsetenv("DISABLE_LSFGVK");
    unsetenv("LSFGVK_ENV");                   /* left over from a launcher: it would ignore conf.toml */
    fprintf(stderr, "frame generation: on - OpenGL on Zink, lsfg-vk with %s, shaders %s\n",
            g_lsfg_st.conf, g_lsfg_st.dll_path);
    lsfg_tee_start();
}

/* around the video decoder's start: Mesa applies MESA_LOADER_DRIVER_OVERRIDE
 * to VAAPI too, and Zink has no video decoder (hardware decoding would fail).
 * The OpenGL driver is picked once, when the window's context is made, so
 * taking it away for this moment changes nothing there. (getenv/setenv are
 * not thread-safe; nothing else in the viewer reads the environment then.) */
static char g_lsfg_va_saved[32];
static void lsfg_va_begin(void)
{
    const char *v = getenv("MESA_LOADER_DRIVER_OVERRIDE");
    g_lsfg_va_saved[0] = 0;
    if (!v || strcmp(v, "zink") != 0) return;
    snprintf(g_lsfg_va_saved, sizeof(g_lsfg_va_saved), "%s", v);
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
}
static void lsfg_va_end(void)
{
    if (g_lsfg_va_saved[0]) setenv("MESA_LOADER_DRIVER_OVERRIDE", g_lsfg_va_saved, 1);
}

/* once, from the first frame drawn with OpenGL (its context is current then) */
static void lsfg_probe_gl(void)
{
    if (g_lsfg_st.probed) return;
    g_lsfg_st.probed = 1;
    typedef const GLubyte *(APIENTRY *gs_t)(GLenum);
    gs_t gs = (gs_t)SDL_GL_GetProcAddress("glGetString");
    const char *r = gs ? (const char *)gs(GL_RENDERER) : NULL;
    snprintf(g_lsfg_st.gl, sizeof(g_lsfg_st.gl), "%s", r ? r : "?");
    g_lsfg_st.zink = r && strstr(r, "zink") != NULL;
    if (g_lsfg_started && !g_lsfg_st.blocked)
        fprintf(stderr, "frame generation: OpenGL renderer \"%s\"%s\n", g_lsfg_st.gl, g_lsfg_st.zink ? "" : " (NOT Zink)");
}

static void lsfg_describe(char *out, size_t cap)
{
    if (g_lsfg != g_lsfg_started) { snprintf(out, cap, "Restarting the viewer for frame generation %s...", g_lsfg ? "on" : "off"); return; }
    if (!g_lsfg) { snprintf(out, cap, "Lossless Scaling's LSFG via lsfg-vk; set up in lsfg-vk-ui (G)"); return; }
    if (g_lsfg_st.state < 0) snprintf(out, cap, "Not active: %.100s", g_lsfg_st.why);
    else if (g_lsfg_st.probed && !g_lsfg_st.zink) snprintf(out, cap, "Not active: OpenGL is \"%.40s\", not Mesa Zink", g_lsfg_st.gl);
    else if (g_lsfg_st.state > 0) snprintf(out, cap, "Active: lsfg-vk profile \"%.40s\", %dx - change it in lsfg-vk-ui", g_lsfg_st.profile, g_lsfg_st.mult);
    else snprintf(out, cap, "Starting lsfg-vk... (G)");
}

static void lsfg_hud_text(char *out, size_t cap)
{
    if (!g_lsfg_started) snprintf(out, cap, "off (G)");
    else if (g_lsfg_st.state < 0 || (g_lsfg_st.probed && !g_lsfg_st.zink)) snprintf(out, cap, "NOT active");
    else if (g_lsfg_st.state > 0) snprintf(out, cap, "LSFG %dx active", g_lsfg_st.mult);
    else snprintf(out, cap, "starting");
}

/* the FPS counter's tag: generated frames do not show in the viewer's own
 * frame rate (lsfg-vk adds them after the viewer presents) */
static const char *lsfg_fps_tag(void)
{
    static char t[32];
    if (!g_lsfg_started) return NULL;
    if (g_lsfg_st.state > 0) snprintf(t, sizeof(t), "+ LSFG %dx", g_lsfg_st.mult);
    else if (g_lsfg_st.state < 0) snprintf(t, sizeof(t), "LSFG off");
    else snprintf(t, sizeof(t), "LSFG ...");
    return t;
}

/* the live loop, every pass: what lsfg-vk said (lsfg_tee). Its swap chain
 * failing leaves the window black: the viewer starts again without it, and
 * says why. No profile for the viewer: it says that too. */
static void lsfg_poll(int picture)
{
    if (!g_lsfg_started || g_lsfg_restart) return;
    if (g_lsfg_st.state < 0) {
        if (!g_lsfg_st.said) {
            g_lsfg_st.said = 1;
            char t[112];
            snprintf(t, sizeof(t), "Frame generation off: %.88s", g_lsfg_st.why);
            hud_toast(t);
        }
        return;
    }
    if (g_lsfg_st.state > 0 || !g_lsfg_mx) return;
    const Uint64 now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
    if (g_lsfg_st.t_poll && (double)(now - g_lsfg_st.t_poll) / (double)hz < 0.25) return;
    g_lsfg_st.t_poll = now;
    if (g_lsfg_st.added && !g_lsfg_st.said) {
        g_lsfg_st.said = 1;
        hud_toast("Added the lsfg-vk profile \"" LSFG_PROFILE_NAME "\" - change it in lsfg-vk-ui");
    }
    SDL_LockMutex(g_lsfg_mx);
    const int loaded = g_lsfg_st.loaded, init = g_lsfg_st.init;
    char err[200], prof[96];
    snprintf(err, sizeof(err), "%s", g_lsfg_st.err);
    snprintf(prof, sizeof(prof), "%s", g_lsfg_st.profile);
    SDL_UnlockMutex(g_lsfg_mx);
    if (err[0]) {
        if (strstr(err, "shader DLL does not exist") || strstr(err, "base shader"))
            snprintf(g_lsfg_st.why, sizeof(g_lsfg_st.why), "needs the \"lsfg-vk\" beta of Lossless Scaling (Steam > Properties > Betas)");
        else
            snprintf(g_lsfg_st.why, sizeof(g_lsfg_st.why), "lsfg-vk: %.80s", err);
        g_lsfg_st.state = -1;
        fprintf(stderr, "\nframe generation failed (%s) - restarting without it\n", err);
        setenv("SFT_LSFG_FAILED", g_lsfg_st.why, 1);
        hud_toast("Frame generation failed - restarting the viewer without it...");
        g_lsfg_restart = 2;                   /* 2: keep --lsfg; the next run reads SFT_LSFG_FAILED */
        return;
    }
    if (!picture) return;
    if (loaded && !prof[0]) {                 /* the layer runs, but no profile is for the viewer */
        if (!g_lsfg_st.t_loaded) g_lsfg_st.t_loaded = now;
        else if ((double)(now - g_lsfg_st.t_loaded) / (double)hz > 3.0) {
            g_lsfg_st.state = -1;
            snprintf(g_lsfg_st.why, sizeof(g_lsfg_st.why), "no lsfg-vk profile has raw-view in \"active_in\" (lsfg-vk-ui)");
            fprintf(stderr, "\nframe generation: lsfg-vk found no profile for this program (%s)\n", g_lsfg_st.conf);
        }
        return;
    }
    if (!loaded && g_lsfg_st.probed && g_lsfg_st.zink) {   /* Zink, but the layer never said a word */
        if (!g_lsfg_st.t_loaded) g_lsfg_st.t_loaded = now;
        else if ((double)(now - g_lsfg_st.t_loaded) / (double)hz > 3.0) {
            g_lsfg_st.state = -1;
            snprintf(g_lsfg_st.why, sizeof(g_lsfg_st.why), "the Vulkan loader did not load lsfg-vk");
        }
        return;
    }
    /* its profile, set up and no error a moment later: generating */
    if (init && prof[0]) {
        if (!g_lsfg_st.t_init) g_lsfg_st.t_init = now;
        else if ((double)(now - g_lsfg_st.t_init) / (double)hz > 1.5) {
            g_lsfg_st.state = 1;
            snprintf(g_lsfg_st.profile, sizeof(g_lsfg_st.profile), "%s", prof);
            char t[112];
            snprintf(t, sizeof(t), "Frame generation active: lsfg-vk profile \"%.40s\", %dx", prof, g_lsfg_st.mult);
            hud_toast(t);
            fprintf(stderr, "\nframe generation: active - lsfg-vk profile \"%s\", %dx\n", prof, g_lsfg_st.mult);
        }
    }
}

/* the driver cannot change in a running process: the viewer starts again */
static void lsfg_restart_now(void)
{
    fprintf(stderr, "\nframe generation: %s - restarting the viewer\n", g_lsfg_restart == 2 ? "failed" : g_lsfg ? "on" : "off");
    if (!g_lsfg_argv) return;
    if (g_lsfg_err_fd >= 0) dup2(g_lsfg_err_fd, 2);    /* the real stderr back, for the next run */
    /* as update.h: the USB device's descriptor must not stay claimed */
    for (int fd = 3; fd < 4096; fd++) close(fd);
    char **av = g_lsfg_argv;
    int n = 1;
    for (int i = 1; av[i]; i++) {             /* a --lsfg on the command line would undo the choice */
        if (g_lsfg_restart == 1 && !strcmp(av[i], "--lsfg") && av[i + 1]) { i++; continue; }
        av[n++] = av[i];
    }
    av[n] = NULL;
    execv("/proc/self/exe", av);
    fprintf(stderr, "frame generation: restart failed - start the viewer again\n");
    exit(0);
}

#else
#define LSFG_AVAILABLE 0
static void lsfg_env_setup(char **argv) { (void)argv; }
static void lsfg_va_begin(void) {}
static void lsfg_va_end(void) {}
static void lsfg_poll(int picture) { (void)picture; }
static const char *lsfg_fps_tag(void) { return NULL; }
#endif
