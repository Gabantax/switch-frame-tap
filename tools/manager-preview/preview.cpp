/*
 * The Switch app's screens on a PC (v0.7.14): manager/source/main.cpp built
 * against stand-ins for libnx, SDL2_ttf (stb_truetype) and SDL2_image, drawn
 * by SDL's software renderer and saved as BMPs (run.sh makes them PNGs).
 * The console's own font is not on a PC: a TTF stands in for it (DejaVu Sans
 * by default - wider than the console's, so what fits here fits there), and
 * the button glyphs are drawn as letters.
 *
 * It runs in a made-up SD card: run.sh builds one, with the files a full
 * install has, and the status a console streaming 1080p60 would answer.
 */
#define SFT_PREVIEW 1
#include "../../manager/source/main.cpp"
#include <fstream>
#include <iterator>

namespace {

    StreamStatus g_st = {};

    void Settle(App &app, int frames = 12) { for (int i = 0; i < frames; ++i) { Draw(app); } }

    void Save(SDL_Surface *s, const std::string &dir, const char *name) {
        const std::string p = dir + "/" + name + ".bmp";
        if (SDL_SaveBMP(s, p.c_str()) != 0) { std::fprintf(stderr, "could not save %s\n", p.c_str()); }
        else { std::printf("%s\n", p.c_str()); }
    }

    void Shot(App &app, SDL_Surface *s, const std::string &dir, const char *name, int page, bool content, int sel = -1) {
        app.GoPage(page);
        app.in_content = content;
        if (sel >= 0) { app.Cur().sel = sel; }
        Settle(app);
        Save(s, dir, name);
    }

}

int main(int argc, char **argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const char *font = argc > 2 ? argv[2] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

    SDL_Init(0);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ui::W, ui::H, 32, SDL_PIXELFORMAT_ARGB8888);
    ui::g_ren = SDL_CreateSoftwareRenderer(surf);
    SDL_SetRenderDrawBlendMode(ui::g_ren, SDL_BLENDMODE_BLEND);

    std::ifstream in(font, std::ios::binary);
    static std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) { std::fprintf(stderr, "no font at %s\n", font); return 1; }
    ui::LoadFonts(data.data(), data.size(), nullptr, 0);

    /* a console streaming 1080p60 over the network, HOME menu and overlays on */
    g_st.version = 4;
    g_st.state = 4;
    g_st.enabled = 1;
    g_st.width = 1920;
    g_st.height = 1080;
    g_st.fps_x10 = 599;
    g_st.game_fps_x10 = 600;
    g_st.shots = 3;
    g_st.reserved = 0x101;                  /* audio on, playing */
    g_st.reserved3 = 2 | 4 | 16 | 32;       /* network, a viewer on it, HOME menu, overlays */
    preview::g_status = &g_st;
    preview::g_status_size = sizeof(g_st);
    preview::g_fw = MAKEHOSVERSION(22, 5, 0);
    preview::g_ip = 192u | (168u << 8) | (1u << 16) | (42u << 24);

    App app;
    app.fw = hosversionGet();
    splGetConfig(0, &app.ams);
    app.Refresh();
    app.Build();

    Shot(app, surf, out, "01-home", 0, false);
    Shot(app, surf, out, "02-home-focus", 0, true, 1);
    Shot(app, surf, out, "03-stream", 1, true, 0);
    Shot(app, surf, out, "04-picture", 2, false);
    Shot(app, surf, out, "05-screenshots", 3, true, 1);
    Shot(app, surf, out, "06-beta", 4, true, 2);
    Shot(app, surf, out, "07-extras", 5, true, 2);
    Shot(app, surf, out, "08-setup", 6, true, 10);
    Shot(app, surf, out, "09-about", 7, true, 0);

    /* waiting for the viewer, the toast */
    g_st.state = 2;
    g_st.reserved3 = 2 | 16;
    app.Refresh();
    app.GoPage(0);
    app.in_content = false;
    app.Toast("Saved - restart the console to apply it");
    Settle(app, 4);
    Save(surf, out, "10-home-waiting");

    g_gal.Open();
    DrawGallery(g_gal);
    Save(surf, out, "11-gallery-empty");

    DrawProgressCard("Get extras", "ReverseNX-RT", "Downloading...", 0.42, "From the official GitHub release");
    DrawHints({ { "", 'B', "Cancel" } }, 0);
    Save(surf, out, "12-install");
    return 0;
}
