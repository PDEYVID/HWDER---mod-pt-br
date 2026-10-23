// Host input: XInput (loaded dynamically) + keyboard fallback while the HWDER window has focus.
#include "host_input.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "gpu/settings.h"

namespace hid {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_get_state = nullptr;
XInputSetStateFn g_set_state = nullptr;
bool g_loaded = false;
int g_active_pad = -1;
u32 g_probe_counter = 0;
bool g_pad_present[4] = {true, true, true, true};
std::atomic<int> g_block{0};
std::mutex g_last_m;
PadInput g_last;

// Switch button per settings::Action (stick directions are analog: 0).
constexpr u64 kActionBits[settings::kActCount] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    kBtnA, kBtnB, kBtnX, kBtnY, kBtnL, kBtnR, kBtnZL, kBtnZR,
    kBtnPlus, kBtnMinus, kBtnStickL, kBtnStickR, kBtnUp, kBtnDown, kBtnLeft, kBtnRight,
};

void load_xinput() {
    if (g_loaded) return;
    g_loaded = true;
    const char* dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* d : dlls) {
        HMODULE m = LoadLibraryA(d);
        if (!m) continue;
        g_get_state = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
        g_set_state = (XInputSetStateFn)GetProcAddress(m, "XInputSetState");
        if (g_get_state) {
            hw_log("hid: using %s", d);
            return;
        }
    }
    hw_log("hid: XInput not available - keyboard only");
}

bool key(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

s32 axis(SHORT v, SHORT dead) {
    if (v > -dead && v < dead) return 0;
    return std::clamp((s32)v, -kStickMax, kStickMax);
}

bool source_down(const XINPUT_GAMEPAD& g, int src, BYTE trigger) {
    using namespace settings;
    switch (src) {
    case kPadA: return g.wButtons & XINPUT_GAMEPAD_A;
    case kPadB: return g.wButtons & XINPUT_GAMEPAD_B;
    case kPadX: return g.wButtons & XINPUT_GAMEPAD_X;
    case kPadY: return g.wButtons & XINPUT_GAMEPAD_Y;
    case kPadLB: return g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER;
    case kPadRB: return g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER;
    case kPadLT: return g.bLeftTrigger > trigger;
    case kPadRT: return g.bRightTrigger > trigger;
    case kPadStart: return g.wButtons & XINPUT_GAMEPAD_START;
    case kPadBack: return g.wButtons & XINPUT_GAMEPAD_BACK;
    case kPadLS: return g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB;
    case kPadRS: return g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB;
    case kPadDUp: return g.wButtons & XINPUT_GAMEPAD_DPAD_UP;
    case kPadDDown: return g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN;
    case kPadDLeft: return g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT;
    case kPadDRight: return g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT;
    }
    return false;
}

BYTE trigger_threshold() { return (BYTE)std::clamp(settings::input().trigger * 255 / 100, 1, 254); }

}  // namespace

void poll_host_input(HostInput& out) {
    load_xinput();
    PadInput& s = out.pad;
    s = {};
    out.touch = {};
    const settings::Input& cfg = settings::input();
    const int block = g_block.load(std::memory_order_relaxed);

    if (g_get_state) {
        // Disconnected pads are expensive to query; re-probe them only every ~0.5 s.
        bool probe = (++g_probe_counter % 100) == 0;
        int active = -1;
        for (DWORD i = 0; i < 4; i++) {
            if (!g_pad_present[i] && !probe) continue;
            XINPUT_STATE xs;
            if (g_get_state(i, &xs) != ERROR_SUCCESS) {
                g_pad_present[i] = false;
                continue;
            }
            g_pad_present[i] = true;
            if (active >= 0 || (cfg.pad_index >= 0 && cfg.pad_index != (int)i)) continue;
            active = (int)i;
            if (block >= 2) continue;
            const XINPUT_GAMEPAD& g = xs.Gamepad;
            BYTE trig = trigger_threshold();
            for (int a = 0; a < settings::kActCount; a++)
                if (!settings::is_stick_action(a) && source_down(g, cfg.pad[a], trig)) s.buttons |= kActionBits[a];
            s.lx = axis(g.sThumbLX, (SHORT)(cfg.deadzone_l * kStickMax / 100));
            s.ly = axis(g.sThumbLY, (SHORT)(cfg.deadzone_l * kStickMax / 100));
            s.rx = axis(g.sThumbRX, (SHORT)(cfg.deadzone_r * kStickMax / 100));
            s.ry = axis(g.sThumbRY, (SHORT)(cfg.deadzone_r * kStickMax / 100));
        }
        g_active_pad = active;
    }

    DWORD fg_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    if (fg_pid == GetCurrentProcessId() && block == 0) {
        // Bindings from the Controls tab (defaults: WASD move, arrows camera, K/J/I/L = A/B/X/Y, ...).
        auto bound = [&](int a) { return key(cfg.keys[a][0]) || key(cfg.keys[a][1]); };
        if (bound(settings::kActLUp)) s.ly = kStickMax;
        if (bound(settings::kActLDown)) s.ly = -kStickMax;
        if (bound(settings::kActLLeft)) s.lx = -kStickMax;
        if (bound(settings::kActLRight)) s.lx = kStickMax;
        if (bound(settings::kActRUp)) s.ry = kStickMax;
        if (bound(settings::kActRDown)) s.ry = -kStickMax;
        if (bound(settings::kActRLeft)) s.rx = -kStickMax;
        if (bound(settings::kActRRight)) s.rx = kStickMax;
        for (int a = settings::kActA; a < settings::kActCount; a++)
            if (bound(a)) s.buttons |= kActionBits[a];
    }
    if (cfg.invert_rx) s.rx = -s.rx;
    if (cfg.invert_ry) s.ry = -s.ry;

    const s32 t = kStickMax / 2;
    if (s.lx < -t) s.buttons |= kBtnStickLLeft;
    if (s.lx > t) s.buttons |= kBtnStickLRight;
    if (s.ly > t) s.buttons |= kBtnStickLUp;
    if (s.ly < -t) s.buttons |= kBtnStickLDown;
    if (s.rx < -t) s.buttons |= kBtnStickRLeft;
    if (s.rx > t) s.buttons |= kBtnStickRRight;
    if (s.ry > t) s.buttons |= kBtnStickRUp;
    if (s.ry < -t) s.buttons |= kBtnStickRDown;

    std::lock_guard<std::mutex> l(g_last_m);
    g_last = s;
}

void set_rumble(float low, float high) {
    load_xinput();
    if (!g_set_state || g_active_pad < 0) return;
    float k = std::clamp(settings::input().rumble, 0, 100) / 100.0f;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = (WORD)(std::clamp(low * k, 0.0f, 1.0f) * 65535.0f);
    v.wRightMotorSpeed = (WORD)(std::clamp(high * k, 0.0f, 1.0f) * 65535.0f);
    g_set_state((DWORD)g_active_pad, &v);
}

PadInput last_input() {
    std::lock_guard<std::mutex> l(g_last_m);
    return g_last;
}

int active_pad() { return g_active_pad; }

bool pad_connected(int slot) { return slot >= 0 && slot < 4 && g_get_state && g_pad_present[slot]; }

u32 pad_sources_down() {
    load_xinput();
    int pad = g_active_pad;
    if (!g_get_state || pad < 0) return 0;
    XINPUT_STATE xs;
    if (g_get_state((DWORD)pad, &xs) != ERROR_SUCCESS) return 0;
    u32 m = 0;
    BYTE trig = trigger_threshold();
    for (int src = 0; src < settings::kPadSourceCount; src++)
        if (source_down(xs.Gamepad, src, trig)) m |= 1u << src;
    return m;
}

void set_input_block(int level) { g_block.store(level, std::memory_order_relaxed); }

}  // namespace hid
