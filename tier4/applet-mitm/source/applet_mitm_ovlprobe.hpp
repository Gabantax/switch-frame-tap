/*
 * v0.7.14 overlay probe (applet_mitm_ovlprobe.cpp): research only. Test builds
 * set SFT_OVL_PROBE=1; then sdmc:/config/switch-frame-tap/probe-ovl.txt turns
 * it on.
 */
#pragma once
#include <stratosphere.hpp>

#ifndef SFT_OVL_PROBE
#define SFT_OVL_PROBE 1
#endif

namespace ams::mitm::applet {

    void OvlProbeStart();

}
