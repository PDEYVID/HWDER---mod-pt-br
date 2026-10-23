#include "gpu/settings.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace settings {

static Video g_video;
static Input g_input = [] {
    Input in;
    default_keys(in);
    default_pad(in, false);
    return in;
}();
static const char* kFile = "hwder_settings.ini";

// ini key suffixes, indexed by Action
static const char* kActionKeys[kActCount] = {
    "lup", "ldown", "lleft", "lright", "rup", "rdown", "rleft", "rright",
    "a", "b", "x", "y", "l", "r", "zl", "zr",
    "plus", "minus", "lclick", "rclick", "dup", "ddown", "dleft", "dright",
};

const char* action_name(int a) {
    static const char* names[kActCount] = {
        "Left stick up", "Left stick down", "Left stick left", "Left stick right",
        "Right stick up", "Right stick down", "Right stick left", "Right stick right",
        "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
        "+ (Plus)", "- (Minus)", "Left stick click", "Right stick click",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    };
    return a >= 0 && a < kActCount ? names[a] : "?";
}

const char* pad_source_name(int s) {
    static const char* names[kPadSourceCount] = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back", "LS click", "RS click",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    };
    return s >= 0 && s < kPadSourceCount ? names[s] : "-";
}

void default_keys(Input& in) {
    // WASD move, arrows camera, K/J/I/L = A/B/X/Y (Enter = A, Backspace = B), Q/E = L/R, U/O = ZL/ZR,
    // Space/Tab = +/-, Z/C = stick clicks, 1-4 = D-pad.
    static const int keys[kActCount][2] = {
        {'W', 0}, {'S', 0}, {'A', 0}, {'D', 0},
        {VK_UP, 0}, {VK_DOWN, 0}, {VK_LEFT, 0}, {VK_RIGHT, 0},
        {'K', VK_RETURN}, {'J', VK_BACK}, {'I', 0}, {'L', 0},
        {'Q', 0}, {'E', 0}, {'U', 0}, {'O', 0},
        {VK_SPACE, 0}, {VK_TAB, 0}, {'Z', 0}, {'C', 0},
        {'1', 0}, {'2', 0}, {'3', 0}, {'4', 0},
    };
    memcpy(in.keys, keys, sizeof(keys));
}

void default_pad(Input& in, bool xbox_labels) {
    for (int a = 0; a < kActCount; a++) in.pad[a] = kPadNone;
    // Positional: Xbox bottom/right/left/top = Switch B/A/Y/X. Labels: Xbox A = Switch A and so on.
    in.pad[kActA] = xbox_labels ? kPadA : kPadB;
    in.pad[kActB] = xbox_labels ? kPadB : kPadA;
    in.pad[kActX] = xbox_labels ? kPadX : kPadY;
    in.pad[kActY] = xbox_labels ? kPadY : kPadX;
    in.pad[kActL] = kPadLB;
    in.pad[kActR] = kPadRB;
    in.pad[kActZL] = kPadLT;
    in.pad[kActZR] = kPadRT;
    in.pad[kActPlus] = kPadStart;
    in.pad[kActMinus] = kPadBack;
    in.pad[kActLClick] = kPadLS;
    in.pad[kActRClick] = kPadRS;
    in.pad[kActDUp] = kPadDUp;
    in.pad[kActDDown] = kPadDDown;
    in.pad[kActDLeft] = kPadDLeft;
    in.pad[kActDRight] = kPadDRight;
}

Video& video() { return g_video; }
Input& input() { return g_input; }

void load() {
    // Default for the 60 Hz monitor switch: on when the desktop refresh is not a multiple of 60. The game
    // runs at 60 fps and its movies at 30; on a 144 Hz panel FIFO presentation shows frames for 2/3 or
    // 5/5/5/4 refreshes, which reads as stutter. An explicit value in the ini always wins.
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            g_video.display_60hz = (dm.dmDisplayFrequency % 60) != 0;
    }
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line;
        const char* v = eq + 1;
        if (k == "refresh") g_video.refresh = atoi(v);
        else if (k == "vsync") g_video.vsync = atoi(v) != 0;
        else if (k == "aniso") g_video.aniso = atoi(v);
        else if (k == "window_w") g_video.window_w = atoi(v);
        else if (k == "window_h") g_video.window_h = atoi(v);
        else if (k == "borderless") g_video.borderless = atoi(v) != 0;
        else if (k == "res_scale") g_video.res_scale = (float)atof(v);
        else if (k == "unlocked_res") g_video.unlocked_res = atoi(v) != 0;
        else if (k == "show_fps") g_video.show_fps = atoi(v) != 0;
        else if (k == "display_60hz") g_video.display_60hz = atoi(v) != 0;
        else if (k == "interp_hz") g_video.interp_hz = atoi(v);
        else if (k == "pad_index") g_input.pad_index = atoi(v);
        else if (k == "deadzone_l") g_input.deadzone_l = atoi(v);
        else if (k == "deadzone_r") g_input.deadzone_r = atoi(v);
        else if (k == "trigger") g_input.trigger = atoi(v);
        else if (k == "invert_rx") g_input.invert_rx = atoi(v) != 0;
        else if (k == "invert_ry") g_input.invert_ry = atoi(v) != 0;
        else if (k == "rumble") g_input.rumble = atoi(v);
        else
            for (int a = 0; a < kActCount; a++) {
                if (k == std::string("kb_") + kActionKeys[a]) {
                    g_input.keys[a][0] = g_input.keys[a][1] = 0;
                    sscanf(v, "%d,%d", &g_input.keys[a][0], &g_input.keys[a][1]);
                } else if (k == std::string("pad_") + kActionKeys[a]) {
                    g_input.pad[a] = atoi(v);
                }
            }
    }
    fclose(f);
    if (g_video.interp_hz != 0) g_video.display_60hz = false;  // interpolation needs the panel's own refresh
}

void save() {
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    fprintf(f, "refresh=%d\nvsync=%d\naniso=%d\nwindow_w=%d\nwindow_h=%d\nborderless=%d\nres_scale=%.3f\nunlocked_res=%d\nshow_fps=%d\ndisplay_60hz=%d\ninterp_hz=%d\n",
            g_video.refresh, (int)g_video.vsync, g_video.aniso, g_video.window_w, g_video.window_h, (int)g_video.borderless,
            g_video.res_scale, (int)g_video.unlocked_res, (int)g_video.show_fps, (int)g_video.display_60hz, g_video.interp_hz);
    const Input& in = g_input;
    fprintf(f, "pad_index=%d\ndeadzone_l=%d\ndeadzone_r=%d\ntrigger=%d\ninvert_rx=%d\ninvert_ry=%d\nrumble=%d\n", in.pad_index,
            in.deadzone_l, in.deadzone_r, in.trigger, (int)in.invert_rx, (int)in.invert_ry, in.rumble);
    for (int a = 0; a < kActCount; a++) {
        fprintf(f, "kb_%s=%d,%d\n", kActionKeys[a], in.keys[a][0], in.keys[a][1]);
        if (!is_stick_action(a)) fprintf(f, "pad_%s=%d\n", kActionKeys[a], in.pad[a]);
    }
    fclose(f);
}

double refresh_hz() {
    if (g_video.refresh > 0) return (double)g_video.refresh;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return (double)dm.dmDisplayFrequency;
    return 60.0;
}

}  // namespace settings
