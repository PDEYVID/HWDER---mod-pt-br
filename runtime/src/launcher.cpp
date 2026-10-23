// Startup launcher: plain Win32 controls (no extra dependencies), shown before the game reads the NSP.
// Paths and options go to hwder_settings.ini; a mod is switched off by renaming its folder with a
// leading '_' (the RomFS overlay skips those, see fs/romfs.cpp).
#include "launcher.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/settings.h"
#include "hwder/runtime.h"

namespace launcher {

namespace {

enum : int {
    kIdNsp = 100, kIdNspBrowse, kIdKeys, kIdKeysBrowse, kIdMods, kIdModsOpen, kIdModsRefresh,
    kIdBorderless, kIdFps, kIdShow,
};

const wchar_t* tr(const wchar_t* en) {
    if (settings::ui_language() != 1) return en;
    static const std::unordered_map<std::wstring, const wchar_t*> pt = {
        {L"Game", L"Jogo"},
        {L"Game file (.nsp):", L"Arquivo do jogo (.nsp):"},
        {L"Keys (prod.keys):", L"Chaves (prod.keys):"},
        {L"Browse...", L"Procurar..."},
        {L"Leave empty to look next to hwder.exe (and the usual key folders).",
         L"Deixe vazio para procurar ao lado do hwder.exe (e nas pastas de chaves de sempre)."},
        {L"Mods and translations", L"Mods e traduções"},
        {L"Open mods folder", L"Abrir pasta de mods"},
        {L"Refresh", L"Atualizar"},
        {L"Unchecked = disabled (the folder gets a leading _).", L"Desmarcado = desativado (a pasta ganha _ no início do nome)."},
        {L"No mods yet: put each one in its own folder inside mods\\.", L"Nenhum mod ainda: coloque cada um numa pasta dentro de mods\\."},
        {L"Options", L"Opções"},
        {L"Borderless fullscreen", L"Tela cheia sem bordas"},
        {L"Show FPS", L"Mostrar FPS"},
        {L"Show this window at startup (hold Shift while opening to bring it back)",
         L"Mostrar esta janela ao abrir (segure Shift ao abrir para ela voltar)"},
        {L"Play", L"Jogar"},
        {L"Quit", L"Sair"},
        {L"The game file was not found:\n\n", L"O arquivo do jogo não foi encontrado:\n\n"},
        {L"The keys file was not found:\n\n", L"O arquivo de chaves não foi encontrado:\n\n"},
        {L"Could not rename the mod folder:\n\n", L"Não foi possível renomear a pasta do mod:\n\n"},
    };
    auto it = pt.find(en);
    return it != pt.end() ? it->second : en;
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct Mod {
    std::wstring folder;  // on disk (with '_' when disabled)
    std::wstring name;    // shown
    bool enabled;
};

std::wstring g_mods_dir;
std::vector<Mod> g_mods;
HWND g_wnd, g_list;
HFONT g_font;
UINT g_dpi = 96;
bool g_play = false;

int px(int v) { return MulDiv(v, (int)g_dpi, 96); }

void scan_mods() {
    g_mods.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_mods_dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            Mod m{fd.cFileName, fd.cFileName, fd.cFileName[0] != L'_'};
            if (!m.enabled) m.name = m.name.substr(1);
            g_mods.push_back(m);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
}

void fill_list() {
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_mods.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)g_mods[i].name.c_str();
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);  // explicit W: the build is not UNICODE
        ListView_SetCheckState(g_list, (int)i, g_mods[i].enabled);
    }
    if (g_mods.empty()) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.pszText = (LPWSTR)tr(L"No mods yet: put each one in its own folder inside mods\\.");
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);  // explicit W: the build is not UNICODE
    }
}

bool apply_mods() {
    for (size_t i = 0; i < g_mods.size(); i++) {
        Mod& m = g_mods[i];
        bool want = ListView_GetCheckState(g_list, (int)i) != 0;
        if (want == m.enabled) continue;
        std::wstring target = want ? m.name : L"_" + m.name;
        if (!MoveFileW((g_mods_dir + L"\\" + m.folder).c_str(), (g_mods_dir + L"\\" + target).c_str())) {
            MessageBoxW(g_wnd, (std::wstring(tr(L"Could not rename the mod folder:\n\n")) + m.folder).c_str(), L"HWDER", MB_ICONERROR);
            return false;
        }
        hw_log("launcher: mod '%s' %s", narrow(m.name).c_str(), want ? "enabled" : "disabled");
        m.folder = target;
        m.enabled = want;
    }
    return true;
}

std::wstring text_of(int id) {
    HWND c = GetDlgItem(g_wnd, id);
    std::wstring s(GetWindowTextLengthW(c) + 1, L'\0');
    GetWindowTextW(c, s.data(), (int)s.size());
    s.resize(wcslen(s.c_str()));
    return s;
}

void browse(int edit_id, const wchar_t* filter) {
    wchar_t file[MAX_PATH * 4] = {};
    std::wstring cur = text_of(edit_id);
    wcsncpy_s(file, cur.c_str(), _TRUNCATE);
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = filter;
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH * 4;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of)) SetDlgItemTextW(g_wnd, edit_id, file);
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void play() {
    std::wstring nsp = text_of(kIdNsp), keys = text_of(kIdKeys);
    if (!nsp.empty() && !exists(nsp)) {
        MessageBoxW(g_wnd, (std::wstring(tr(L"The game file was not found:\n\n")) + nsp).c_str(), L"HWDER", MB_ICONWARNING);
        return;
    }
    if (!keys.empty() && !exists(keys)) {
        MessageBoxW(g_wnd, (std::wstring(tr(L"The keys file was not found:\n\n")) + keys).c_str(), L"HWDER", MB_ICONWARNING);
        return;
    }
    if (!apply_mods()) return;
    settings::Game& gm = settings::game();
    gm.nsp = narrow(nsp);
    gm.keys = narrow(keys);
    gm.show_launcher = IsDlgButtonChecked(g_wnd, kIdShow) == BST_CHECKED;
    settings::video().borderless = IsDlgButtonChecked(g_wnd, kIdBorderless) == BST_CHECKED;
    settings::video().show_fps = IsDlgButtonChecked(g_wnd, kIdFps) == BST_CHECKED;
    settings::save();
    g_play = true;
    DestroyWindow(g_wnd);
}

HWND ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0,
         DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

void build(HWND w) {
    const settings::Game& gm = settings::game();
    ctl(w, L"BUTTON", tr(L"Game"), BS_GROUPBOX, 12, 8, 576, 136);
    ctl(w, L"STATIC", tr(L"Game file (.nsp):"), 0, 26, 30, 540, 18);
    ctl(w, L"EDIT", widen(gm.nsp).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 26, 49, 446, 24, kIdNsp, WS_EX_CLIENTEDGE);
    ctl(w, L"BUTTON", tr(L"Browse..."), WS_TABSTOP, 480, 48, 96, 26, kIdNspBrowse);
    ctl(w, L"STATIC", tr(L"Keys (prod.keys):"), 0, 26, 80, 540, 18);
    ctl(w, L"EDIT", widen(gm.keys).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 26, 99, 446, 24, kIdKeys, WS_EX_CLIENTEDGE);
    ctl(w, L"BUTTON", tr(L"Browse..."), WS_TABSTOP, 480, 98, 96, 26, kIdKeysBrowse);
    ctl(w, L"STATIC", tr(L"Leave empty to look next to hwder.exe (and the usual key folders)."), SS_LEFT, 26, 126, 550, 16);

    ctl(w, L"BUTTON", tr(L"Mods and translations"), BS_GROUPBOX, 12, 152, 576, 196);
    g_list = ctl(w, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 26, 174, 550, 116, kIdMods,
                 WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = px(520);
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    ctl(w, L"BUTTON", tr(L"Open mods folder"), WS_TABSTOP, 26, 298, 170, 28, kIdModsOpen);
    ctl(w, L"BUTTON", tr(L"Refresh"), WS_TABSTOP, 204, 298, 110, 28, kIdModsRefresh);
    ctl(w, L"STATIC", tr(L"Unchecked = disabled (the folder gets a leading _)."), SS_LEFT, 26, 330, 550, 16);

    ctl(w, L"BUTTON", tr(L"Options"), BS_GROUPBOX, 12, 356, 576, 84);
    ctl(w, L"BUTTON", tr(L"Borderless fullscreen"), WS_TABSTOP | BS_AUTOCHECKBOX, 26, 378, 260, 22, kIdBorderless);
    ctl(w, L"BUTTON", tr(L"Show FPS"), WS_TABSTOP | BS_AUTOCHECKBOX, 300, 378, 260, 22, kIdFps);
    ctl(w, L"BUTTON", tr(L"Show this window at startup (hold Shift while opening to bring it back)"), WS_TABSTOP | BS_AUTOCHECKBOX,
        26, 406, 550, 22, kIdShow);
    CheckDlgButton(w, kIdBorderless, settings::video().borderless ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, kIdFps, settings::video().show_fps ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, kIdShow, gm.show_launcher ? BST_CHECKED : BST_UNCHECKED);

    ctl(w, L"BUTTON", tr(L"Quit"), WS_TABSTOP, 376, 452, 100, 32, IDCANCEL);
    HWND playb = ctl(w, L"BUTTON", tr(L"Play"), WS_TABSTOP | BS_DEFPUSHBUTTON, 486, 452, 100, 32, IDOK);
    scan_mods();
    fill_list();
    SetFocus(playb);
}

LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = w;
        build(w);
        return 0;
    case WM_CTLCOLORSTATIC:  // labels on the window background, not grey boxes
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: play(); return 0;
        case IDCANCEL: DestroyWindow(w); return 0;
        case kIdNspBrowse: browse(kIdNsp, L"Nintendo Switch (*.nsp)\0*.nsp\0*.*\0*.*\0"); return 0;
        case kIdKeysBrowse: browse(kIdKeys, L"prod.keys\0*.keys\0*.*\0*.*\0"); return 0;
        case kIdModsOpen:
            CreateDirectoryW(g_mods_dir.c_str(), nullptr);
            ShellExecuteW(w, L"open", g_mods_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case kIdModsRefresh:
            scan_mods();
            fill_list();
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void apply_env() {
    // The NSP loader reads these (fs/nsp.cpp); an explicit environment variable still wins.
    const settings::Game& gm = settings::game();
    if (!gm.nsp.empty() && !_wgetenv(L"HWDER_NSP")) _wputenv_s(L"HWDER_NSP", widen(gm.nsp).c_str());
    if (!gm.keys.empty() && !_wgetenv(L"HWDER_KEYS")) _wputenv_s(L"HWDER_KEYS", widen(gm.keys).c_str());
}

}  // namespace

bool run(const std::string& exe_dir) {
    settings::load();
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool automated = getenv("HWDER_NO_LAUNCHER") || getenv("HWDER_AUTOPLAY") || getenv("HWDER_OFFSCREEN") || getenv("HWDER_LOG");
    if (automated || (!shift && !settings::game().show_launcher)) {
        apply_env();
        return true;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dpi = GetDpiForSystem();
    g_mods_dir = widen(exe_dir) + L"\\mods";
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_font = CreateFontW(-MulDiv(10, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                         L"Segoe UI");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"HWDER launcher";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, px(600), px(496)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"HWDER - Hyrule Warriors: Definitive Edition", style,
                             (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr,
                             nullptr, wc.hInstance, nullptr);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &m)) continue;  // Tab, Enter (Play) and Esc (Quit)
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    hw_log("launcher: %s", g_play ? "play" : "closed");
    if (g_play) apply_env();
    return g_play;
}

}  // namespace launcher
