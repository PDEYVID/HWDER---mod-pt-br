// User-facing video settings (edited in the F1 overlay, persisted in hwder_settings.ini next to the exe).
#pragma once

namespace settings {

struct Video {
    int refresh = 60;           // presenter rate in Hz: 60, 120, 144; 0 = match the display
    bool vsync = false;         // FIFO present mode when true, MAILBOX otherwise
    int aniso = 0;              // anisotropic filtering override: 0 = game default, 2/4/8/16
    int window_w = 0;           // requested client size, 0 = leave the window alone
    int window_h = 0;
    bool borderless = false;    // borderless fullscreen on the current monitor
    float res_scale = 1.0f;     // internal resolution scale (renderer support pending)
    bool unlocked_res = false;  // render at the window's native size / aspect (pending)
    bool show_fps = false;      // small frame-time readout in the corner
    int interp_hz = 0;          // frame interpolation output rate: 0 off, -1 match display, else Hz
    bool display_60hz = false;  // switch the monitor to 60 Hz while the game runs (even cadence for 60/30 fps content)
    int language = -1;          // F1 menu: -1 = follow Windows, 0 = English, 1 = Portuguese (Brazil)
};
int ui_language();  // `language` resolved: 0 English, 1 Portuguese

// Switch-side inputs that can be bound, in the order the Controls tab lists them.
enum Action : int {
    kActLUp, kActLDown, kActLLeft, kActLRight,
    kActRUp, kActRDown, kActRLeft, kActRRight,
    kActA, kActB, kActX, kActY,
    kActL, kActR, kActZL, kActZR,
    kActPlus, kActMinus, kActLClick, kActRClick,
    kActDUp, kActDDown, kActDLeft, kActDRight,
    kActCount
};
constexpr bool is_stick_action(int a) { return a <= kActRRight; }
const char* action_name(int a);

// XInput sources a Switch button can be bound to.
enum PadSource : int {
    kPadNone = -1,
    kPadA, kPadB, kPadX, kPadY, kPadLB, kPadRB, kPadLT, kPadRT,
    kPadStart, kPadBack, kPadLS, kPadRS, kPadDUp, kPadDDown, kPadDLeft, kPadDRight,
    kPadSourceCount
};
const char* pad_source_name(int s);

struct Input {
    int keys[kActCount][2] = {};  // virtual-key codes (primary, alternate); 0 = unbound
    int pad[kActCount] = {};      // PadSource per button action; stick directions always come from the sticks
    int pad_index = -1;           // XInput slot 0-3, -1 = first connected
    int deadzone_l = 24;          // stick deadzones, percent of full deflection
    int deadzone_r = 26;
    int trigger = 25;             // ZL/ZR press threshold, percent of trigger travel
    bool invert_rx = false;       // camera axes
    bool invert_ry = false;
    int rumble = 100;             // vibration strength, percent (0 = off)
};
void default_keys(Input& in);
void default_pad(Input& in, bool xbox_labels);  // false: Switch positions (Xbox A = Switch B), true: same labels

Video& video();
Input& input();
void load();
void save();
double refresh_hz();  // `refresh` resolved against the display when it is 0

}  // namespace settings
