/* The PC preview's stand-in for manager/source/extras.hpp (curl, jansson and
 * minizip are not needed to draw the screens): Ultrahand installed, the rest
 * not. */
#pragma once
#include <functional>
#include <string>

namespace extras {

    using Progress = std::function<bool(const char *what, double frac)>;
    struct Tool { const char *name; };

    inline const Tool Ultrahand = { "Ultrahand Overlay" };
    inline const Tool SaltyNX   = { "SaltyNX" };
    inline const Tool ReverseNX = { "ReverseNX-RT" };
    inline const Tool SysClk    = { "sys-clk" };
    inline const Tool HorizonOC = { "Horizon OC" };

    inline bool HasUltrahand() { return true; }
    inline bool HasSaltyNX() { return true; }
    inline bool HasReverseNX() { return false; }
    inline bool HasHorizonOC() { return false; }
    inline bool HasSysClk() { return true; }
    inline bool HekateMissesHocKip() { return false; }
    inline bool Install(const Tool &, const Progress &, std::string *, std::string *err) { *err = "preview"; return false; }

    struct Release { std::string tag, name, url; long long size = 0; };
    inline int CheckUpdate(const char *, bool, Release *rel, const Progress &, std::string *) { rel->tag = "v0.7.15"; return 1; }
    inline bool InstallUpdate(const Release &, const Progress &, std::string *, std::string *err) { *err = "preview"; return false; }
    inline void NetDown() {}

}
