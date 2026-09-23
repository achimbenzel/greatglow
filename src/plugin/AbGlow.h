#pragma once

// Identity of the plug-in. These values are mirrored by tools/generate_pipl.py,
// which builds the PiPL resource; keep both in sync.
#define AB_GLOW_NAME "Profound Glow"
#define AB_GLOW_MATCH_NAME "ABBZ ProfoundGlow"
#define AB_GLOW_CATEGORY "AB Tools"
#define AB_GLOW_DESCRIPTION "High dynamic range glow with multi-scale light diffusion."

#define AB_GLOW_VERSION_MAJOR 1
#define AB_GLOW_VERSION_MINOR 3
#define AB_GLOW_VERSION_BUG 1
#define AB_GLOW_VERSION_BUILD 1

// Set by CMake from the git revision. Shown in the effect's parameters so the
// build After Effects actually loaded can be read off the UI - plug-ins are
// easy to leave a stale copy of, and After Effects searches several folders.
#ifndef AB_GLOW_BUILD_ID
#define AB_GLOW_BUILD_ID "local"
#endif

#define AB_GLOW_STRINGIFY_(x) #x
#define AB_GLOW_STRINGIFY(x) AB_GLOW_STRINGIFY_(x)
#define AB_GLOW_VERSION_STRING                                                       \
    AB_GLOW_STRINGIFY(AB_GLOW_VERSION_MAJOR) "." AB_GLOW_STRINGIFY(AB_GLOW_VERSION_MINOR) "." \
        AB_GLOW_STRINGIFY(AB_GLOW_VERSION_BUG)
