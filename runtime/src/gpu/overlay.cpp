#include "gpu/overlay.h"

#include "gpu/display.h"
#include "gpu/settings.h"
#include "hid/host_input.h"
#include "hwder/runtime.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>
#include <string_view>
#include <filesystem>
#include <mutex>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

float hwder_render_scale_x();  // texture_cache.cpp: active internal resolution scale
float hwder_render_scale_y();
extern "C" std::atomic<int> hw_shader_precompile_left;  // vk_renderer.cpp: cached pipelines still building

namespace overlay {

// Recursive: ImGui's Win32 handler calls ReleaseCapture()/SetCursor(), which re-enter wnd_proc synchronously.
static std::recursive_mutex g_m;
static bool g_inited = false;
static bool g_visible = false;
static bool g_swapchain_dirty = false;
static HWND g_hwnd;
static VkDevice g_dev;
static VkInstance g_instance;
static VkFormat g_fmt;
static std::unordered_map<VkImage, VkImageView> g_views;
// Controls tab: binding being captured (-1 = none). Slot 0/1 = keyboard primary/alternate, 2 = controller.
static int g_capture_action = -1;
static int g_capture_slot = 0;
static u32 g_capture_pad_held = 0;  // pad sources already held when the capture started
static double g_capture_start = 0;
static double g_frame_ms[120];
static int g_frame_i = 0;
static LARGE_INTEGER g_last_t;

// F1 menu text: English in the code, Portuguese (Brazil) from this table (settings::ui_language()).
static const char* tr(const char* en) {
    if (settings::ui_language() != 1) return en;
    static const std::unordered_map<std::string_view, const char*> pt = {
        {"press a key...", "aperte uma tecla..."},
        {"press a button...", "aperte um botão..."},
        {"Click to rebind, right-click to clear", "Clique para trocar, botão direito para limpar"},
        {"Controller", "Controle"},
        {"Automatic (first connected)", "Automático (primeiro conectado)"},
        {"Controller %d%s", "Controle %d%s"},
        {" (not connected)", " (desconectado)"},
        {"Use", "Usar"},
        {"  (connected)", "  (conectado)"},
        {"Controller %d in use", "Controle %d em uso"},
        {"No XInput controller detected - keyboard only", "Nenhum controle XInput detectado - só teclado"},
        {"Face button layout:", "Layout dos botões:"},
        {"Switch positions", "Posições do Switch"},
        {"Bottom button = Switch B, right = A (same place as on a Switch pad). Default.", "Botão de baixo = B do Switch, da direita = A (mesma posição do controle do Switch). Padrão."},
        {"Xbox labels", "Letras do Xbox"},
        {"Xbox A = Switch A, B = B, X = X, Y = Y (follows the printed letters).", "A do Xbox = A do Switch, B = B, X = X, Y = Y (segue as letras impressas)."},
        {"Left stick deadzone", "Zona morta do analógico esquerdo"},
        {"Right stick deadzone", "Zona morta do analógico direito"},
        {"ZL/ZR trigger threshold", "Sensibilidade do ZL/ZR"},
        {"Vibration", "Vibração"},
        {"off", "desligada"},
        {"Invert camera X", "Inverter câmera X"},
        {"Invert camera Y", "Inverter câmera Y"},
        {"Bindings", "Comandos"},
        {"Esc cancels a capture. F1, F2, F11 and F12 are reserved.", "Esc cancela a captura. F1, F2, F11 e F12 são reservadas."},
        {"Keyboard", "Teclado"},
        {"Keyboard (alt)", "Teclado (alt.)"},
        {"left stick", "analógico esquerdo"},
        {"right stick", "analógico direito"},
        {"Reset keyboard", "Restaurar teclado"},
        {"Reset controller", "Restaurar controle"},
        {"Input test", "Teste de comandos"},
        {"Left stick %+6.2f %+6.2f   Right stick %+6.2f %+6.2f", "Analógico esq. %+6.2f %+6.2f   Analógico dir. %+6.2f %+6.2f"},
        {"The game does not receive keyboard input while this menu is open.", "O jogo não recebe o teclado enquanto este menu está aberto."},
        {"Preparing shaders... %d left", "Preparando shaders... faltam %d"},
        {"HWDER - Options   (F1 closes)", "HWDER - Opções   (F1 fecha)"},
        {"Controls", "Controles"},
        {"Video", "Vídeo"},
        {"Display", "Tela"},
        {"Off (60 fps)", "Desligada (60 fps)"},
        {"Match display refresh", "Igual à taxa do monitor"},
        {"Frame interpolation", "Interpolação de quadros"},
        {"The game runs at 60 fps. With interpolation each frame is re-rendered in between with blended\n3D positions (camera, characters), so motion is smooth on 120/144 Hz panels. The HUD and 2D\nelements show the newest frame. Costs extra GPU/CPU work per output frame, and adds one\nframe of latency. Turn it off if your PC can't hold the rate.", "O jogo roda a 60 fps. Com a interpolação, quadros intermediários são desenhados com as posições\n3D misturadas (câmera, personagens), deixando o movimento suave em telas de 120/144 Hz. O HUD e\nos elementos 2D mostram o quadro mais novo. Custa trabalho extra de GPU/CPU por quadro e\nadiciona um quadro de atraso. Desligue se o seu PC não aguentar a taxa."},
        {"Switch monitor to 60 Hz while playing", "Mudar o monitor para 60 Hz durante o jogo"},
        {"The game renders at 60 fps and its movies at 30 fps. On a 144 Hz panel each frame is shown for\n2 or 3 (movies: 4 or 5) refreshes in turn, which looks like stutter. At 60 Hz every frame is\nshown exactly once (movies: twice). Reverts when the game exits. Not with frame interpolation.", "O jogo desenha a 60 fps e os vídeos a 30 fps. Numa tela de 144 Hz cada quadro fica 2 ou 3\natualizações na tela (vídeos: 4 ou 5), alternando, o que parece engasgo. A 60 Hz cada quadro\naparece exatamente uma vez (vídeos: duas). Volta ao normal quando o jogo fecha. Não funciona com interpolação."},
        {"Game logic runs at a fixed 60 fps (the game advances one step per frame). Higher refresh rates come from frame interpolation above.", "A lógica do jogo roda fixa a 60 fps (o jogo avança um passo por quadro). Taxas maiores vêm da interpolação de quadros acima."},
        {"VSync (FIFO)", "VSync (FIFO)"},
        {"Off = mailbox presentation (lowest latency).\nOn = classic vsync; use it if you see tearing.", "Desligado = apresentação mailbox (menor atraso).\nLigado = vsync clássico; use se a imagem rasgar."},
        {"Window", "Janela"},
        {"Borderless fullscreen (F11)", "Tela cheia sem bordas (F11)"},
        {"Window size", "Tamanho da janela"},
        {"Rendering", "Renderização"},
        {"Game default", "Padrão do jogo"},
        {"2x anisotropic", "Anisotrópico 2x"},
        {"4x anisotropic", "Anisotrópico 4x"},
        {"8x anisotropic", "Anisotrópico 8x"},
        {"16x anisotropic", "Anisotrópico 16x"},
        {"Texture filtering", "Filtro de texturas"},
        {"Internal resolution", "Resolução interna"},
        {"Unlocked resolution (render at the window's size)", "Resolução livre (desenha no tamanho da janela)"},
        {"Scene render targets are allocated at this scale (UI and small post-process targets stay native).\nUnlocked = the window's own size and aspect (ultrawide fills the screen); it overrides the scale above.\nOn a 1920x1080 window that equals 1.00x, i.e. the game's native resolution. Applies immediately.", "As imagens da cena são criadas nesta escala (a interface e pequenos efeitos ficam na resolução nativa).\nLivre = o tamanho e o formato da própria janela (ultrawide ocupa a tela toda); substitui a escala acima.\nNuma janela 1920x1080 isso equivale a 1,00x, a resolução nativa do jogo. Aplica na hora."},
        {"Rendering 3D at %dx%d (%.2fx x %.2fx)%s", "3D sendo desenhado em %dx%d (%.2fx x %.2fx)%s"},
        {"  [unlocked: follows the window]", "  [livre: segue a janela]"},
        {"Show frame time", "Mostrar tempo de quadro"},
        {"Cheats (save file)", "Cheats (arquivo de save)"},
        {"no save found under ./save", "nenhum save encontrado em ./save"},
        {"Snapshot save", "Guardar cópia do save"},
        {"snapshot taken (", "cópia guardada ("},
        {" bytes)", " bytes)"},
        {"Diff vs snapshot", "Comparar com a cópia"},
        {" bytes changed in ", " bytes mudaram em "},
        {" regions", " regiões"},
        {"Import unlocks from reference", "Importar desbloqueios da referência"},
        {" flags set - restart the game (backup: zmha.bin.bak)", " flags ativadas - reinicie o jogo (backup: zmha.bin.bak)"},
        {"no reference save at save_reference/zmha.bin", "nenhum save de referência em save_reference/zmha.bin"},
        {"could not read the save", "não foi possível ler o save"},
        {"Settings are saved to hwder_settings.ini when changed.", "As configurações são salvas no hwder_settings.ini ao mudar."},
        {"Turbo %dx  (F2)", "Turbo %dx  (F2)"},
        {"Post-processing", "Pós-processamento"},
        {"FXAA anti-aliasing", "Antisserrilhado FXAA"},
        {"Smooths jagged edges of the whole picture (cheap, slightly soft). Applies immediately.",
         "Suaviza os serrilhados da imagem inteira (leve, deixa um pouco mais macio). Aplica na hora."},
        {"Sharpening", "Nitidez"},
        {"Restores crispness (good after FXAA or a resolution below the window's). 0 = off.",
         "Devolve a definição (bom depois do FXAA ou com resolução menor que a janela). 0 = desligada."},
        {"Turbo speed (F2)", "Velocidade do turbo (F2)"},
        {"F2 speeds the whole game up (menus, cutscenes, grinding). Sound may crackle while it is on.",
         "F2 acelera o jogo inteiro (menus, cenas, farm). O som pode falhar enquanto estiver ligado."},
        {"Screenshot: %s", "Captura: %s"},
        {"F12 saves a screenshot to the screenshots folder.", "F12 salva uma captura de tela na pasta screenshots."},
        {"Language", "Idioma"},
        {"Automatic", "Automático"},
        {"Left stick up", "Analógico esquerdo cima"},
        {"Left stick down", "Analógico esquerdo baixo"},
        {"Left stick left", "Analógico esquerdo esquerda"},
        {"Left stick right", "Analógico esquerdo direita"},
        {"Right stick up", "Analógico direito cima"},
        {"Right stick down", "Analógico direito baixo"},
        {"Right stick left", "Analógico direito esquerda"},
        {"Right stick right", "Analógico direito direita"},
        {"+ (Plus)", "+ (Mais)"},
        {"- (Minus)", "- (Menos)"},
        {"Left stick click", "Clique do analógico esquerdo"},
        {"Right stick click", "Clique do analógico direito"},
        {"D-pad up", "Direcional cima"},
        {"D-pad down", "Direcional baixo"},
        {"D-pad left", "Direcional esquerda"},
        {"D-pad right", "Direcional direita"},
        {"LS click", "Clique LS"},
        {"RS click", "Clique RS"},
    };
    auto it = pt.find(en);
    return it != pt.end() ? it->second : en;
}

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) hw_log("overlay: vulkan error %d", (int)r);
}

void init(HWND hwnd, VkInstance instance, VkPhysicalDevice phys, VkDevice dev, uint32_t queue_family, VkQueue queue,
          VkFormat swapchain_format) {
    std::lock_guard<std::recursive_mutex> l(g_m);
    g_hwnd = hwnd;
    g_dev = dev;
    g_instance = instance;
    g_fmt = swapchain_format;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window positions are not worth a file
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // ImGui's built-in font is ASCII only: Portuguese text and localized key names need Latin-1.
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr, io.Fonts->GetGlyphRangesDefault());
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplVulkan_LoadFunctions(
        VK_API_VERSION_1_3,
        [](const char* name, void* ud) { return vkGetInstanceProcAddr((VkInstance)ud, name); }, instance);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = instance;
    ii.PhysicalDevice = phys;
    ii.Device = dev;
    ii.QueueFamily = queue_family;
    ii.Queue = queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 3;
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_fmt;
    ii.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        hw_log("overlay: ImGui Vulkan backend init failed - overlay disabled");
        return;
    }
    g_inited = true;
    g_visible = getenv("HWDER_OVERLAY") != nullptr;
    QueryPerformanceCounter(&g_last_t);
    hw_log("overlay: ready (F1 opens the video options)");
}

bool wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_inited) return false;
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (g_visible && g_capture_action >= 0 && g_capture_slot < 2 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        int vk = (int)wp;
        // Shift/Ctrl/Alt arrive as the generic key; bind the left/right one actually pressed.
        if (vk == VK_SHIFT) vk = (int)MapVirtualKeyW((lp >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL) vk = (lp & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = (lp & (1 << 24)) ? VK_RMENU : VK_LMENU;
        if (vk != VK_F1 && vk != VK_F2 && vk != VK_F11 && vk != VK_F12) {
            if (vk != VK_ESCAPE) {
                settings::input().keys[g_capture_action][g_capture_slot] = vk;
                settings::save();
            }
            g_capture_action = -1;
            return true;
        }
    }
    ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
    if (!g_visible) return false;
    switch (msg) {
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MOUSEWHEEL: case WM_LBUTTONDBLCLK: case WM_CHAR: case WM_KEYUP:
        return true;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        return wp != VK_F1 && wp != VK_F2 && wp != VK_F11 && wp != VK_F12;
    }
    return false;
}

void toggle() {
    std::lock_guard<std::recursive_mutex> l(g_m);
    g_visible = !g_visible;
    g_capture_action = -1;
}
bool visible() { return g_visible; }

static std::string g_note;
static double g_note_until = 0;
static double now_seconds() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / f.QuadPart;
}
void notify(const std::string& text) {
    std::lock_guard<std::recursive_mutex> l(g_m);
    g_note = text;
    g_note_until = now_seconds() + 3.0;
}
bool swapchain_dirty() {
    bool d = g_swapchain_dirty;
    g_swapchain_dirty = false;
    return d;
}

void on_swapchain(const std::vector<VkImage>& images, VkFormat fmt) {
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (!g_dev) return;  // swapchain created before the overlay exists: display re-registers it after init()
    for (auto& [img, view] : g_views) vkDestroyImageView(g_dev, view, nullptr);
    g_views.clear();
    g_fmt = fmt;
    for (VkImage img : images) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = fmt;
        ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(g_dev, &ci, nullptr, &v) == VK_SUCCESS) g_views[img] = v;
    }
}

static void post_window_change() {
    settings::Video& v = settings::video();
    PostMessageW(g_hwnd, kMsgBorderless, 0, v.borderless ? 1 : 0);
    if (!v.borderless && v.window_w && v.window_h) PostMessageW(g_hwnd, kMsgClientSize, 0, (LPARAM)((v.window_w << 16) | v.window_h));
}

static std::string key_name(int vk) {
    if (!vk) return "-";
    UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk) {  // extended keys share scan codes with the numpad unless flagged
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU: case VK_LWIN:
    case VK_RWIN: case VK_APPS:
        scan |= 0x100;
    }
    wchar_t w[64];
    int n = GetKeyNameTextW((LONG)(((scan & 0xFF) << 16) | ((scan & 0x100) << 16)), w, 64);
    if (n <= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
        return buf;
    }
    char u8[192];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof(u8), nullptr, nullptr);
    return u8;
}

static double now_s() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / f.QuadPart;
}

// One clickable binding cell: left click captures a new key/button, right click clears it.
static bool binding_button(int action, int slot, const std::string& label) {
    bool changed = false;
    bool capturing = g_capture_action == action && g_capture_slot == slot;
    char id[96];
    snprintf(id, sizeof(id), "%s##b%d_%d", capturing ? (slot < 2 ? tr("press a key...") : tr("press a button...")) : label.c_str(),
             action, slot);
    if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(id, ImVec2(-FLT_MIN, 0))) {
        g_capture_action = action;
        g_capture_slot = slot;
        g_capture_pad_held = hid::pad_sources_down();
        g_capture_start = now_s();
    }
    if (capturing) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        settings::Input& in = settings::input();
        if (slot < 2) in.keys[action][slot] = 0;
        else in.pad[action] = settings::kPadNone;
        changed = true;
    }
    if (ImGui::IsItemHovered() && !capturing) ImGui::SetTooltip(tr("Click to rebind, right-click to clear"));
    return changed;
}

static bool controls_tab() {
    settings::Input& in = settings::input();
    bool changed = false;

    // Controller capture: the first pad button pressed that was not already held when the capture began.
    if (g_capture_action >= 0 && g_capture_slot == 2) {
        u32 down = hid::pad_sources_down();
        u32 fresh = down & ~g_capture_pad_held;
        g_capture_pad_held &= down;
        if (fresh) {
            for (int s = 0; s < settings::kPadSourceCount; s++)
                if (fresh & (1u << s)) {
                    in.pad[g_capture_action] = s;
                    break;
                }
            g_capture_action = -1;
            changed = true;
        } else if (now_s() - g_capture_start > 8.0) {
            g_capture_action = -1;
        }
    }

    ImGui::SeparatorText(tr("Controller"));
    {
        int active = hid::active_pad();
        char cur[48];
        if (in.pad_index < 0) snprintf(cur, sizeof(cur), tr("Automatic (first connected)"));
        else snprintf(cur, sizeof(cur), tr("Controller %d%s"), in.pad_index + 1, hid::pad_connected(in.pad_index) ? "" : tr(" (not connected)"));
        if (ImGui::BeginCombo(tr("Use"), cur)) {
            if (ImGui::Selectable(tr("Automatic (first connected)"), in.pad_index < 0)) in.pad_index = -1, changed = true;
            for (int i = 0; i < 4; i++) {
                char item[48];
                snprintf(item, sizeof(item), tr("Controller %d%s"), i + 1, hid::pad_connected(i) ? tr("  (connected)") : "");
                if (ImGui::Selectable(item, in.pad_index == i)) in.pad_index = i, changed = true;
            }
            ImGui::EndCombo();
        }
        if (active >= 0) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), tr("Controller %d in use"), active + 1);
        else ImGui::TextDisabled(tr("No XInput controller detected - keyboard only"));
    }
    ImGui::TextUnformatted(tr("Face button layout:"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Switch positions"))) settings::default_pad(in, false), changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Bottom button = Switch B, right = A (same place as on a Switch pad). Default."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Xbox labels"))) settings::default_pad(in, true), changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Xbox A = Switch A, B = B, X = X, Y = Y (follows the printed letters)."));
    changed |= ImGui::SliderInt(tr("Left stick deadzone"), &in.deadzone_l, 0, 60, "%d%%");
    changed |= ImGui::SliderInt(tr("Right stick deadzone"), &in.deadzone_r, 0, 60, "%d%%");
    changed |= ImGui::SliderInt(tr("ZL/ZR trigger threshold"), &in.trigger, 5, 95, "%d%%");
    changed |= ImGui::SliderInt(tr("Vibration"), &in.rumble, 0, 100, in.rumble ? "%d%%" : tr("off"));
    changed |= ImGui::Checkbox(tr("Invert camera X"), &in.invert_rx);
    ImGui::SameLine();
    changed |= ImGui::Checkbox(tr("Invert camera Y"), &in.invert_ry);

    ImGui::SeparatorText(tr("Bindings"));
    ImGui::TextDisabled(tr("Esc cancels a capture. F1, F2, F11 and F12 are reserved."));
    if (ImGui::BeginTable("##binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 230))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(tr("Keyboard"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Keyboard (alt)"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Controller"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int a = 0; a < settings::kActCount; a++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(settings::action_name(a)));
            for (int slot = 0; slot < 2; slot++) {
                ImGui::TableNextColumn();
                changed |= binding_button(a, slot, key_name(in.keys[a][slot]));
            }
            ImGui::TableNextColumn();
            if (settings::is_stick_action(a)) ImGui::TextDisabled(a < settings::kActRUp ? tr("left stick") : tr("right stick"));
            else changed |= binding_button(a, 2, tr(settings::pad_source_name(in.pad[a])));
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(tr("Reset keyboard"))) settings::default_keys(in), changed = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Reset controller"))) {
        settings::default_pad(in, false);
        in.deadzone_l = 24, in.deadzone_r = 26, in.trigger = 25, in.rumble = 100;
        in.invert_rx = in.invert_ry = false;
        changed = true;
    }

    ImGui::SeparatorText(tr("Input test"));
    {
        hid::PadInput p = hid::last_input();
        static const struct {
            u64 bit;
            const char* name;
        } btns[] = {
            {hid::kBtnA, "A"}, {hid::kBtnB, "B"}, {hid::kBtnX, "X"}, {hid::kBtnY, "Y"}, {hid::kBtnL, "L"},
            {hid::kBtnR, "R"}, {hid::kBtnZL, "ZL"}, {hid::kBtnZR, "ZR"}, {hid::kBtnPlus, "+"}, {hid::kBtnMinus, "-"},
            {hid::kBtnStickL, "LS"}, {hid::kBtnStickR, "RS"}, {hid::kBtnUp, "Up"}, {hid::kBtnDown, "Down"},
            {hid::kBtnLeft, "Left"}, {hid::kBtnRight, "Right"},
        };
        for (auto& b : btns) {
            bool on = (p.buttons & b.bit) != 0;
            ImGui::TextColored(on ? ImVec4(0.3f, 0.9f, 0.3f, 1) : ImVec4(0.45f, 0.45f, 0.45f, 1), "%s", b.name);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::Text(tr("Left stick %+6.2f %+6.2f   Right stick %+6.2f %+6.2f"), p.lx / (float)hid::kStickMax,
                    p.ly / (float)hid::kStickMax, p.rx / (float)hid::kStickMax, p.ry / (float)hid::kStickMax);
        ImGui::TextDisabled(tr("The game does not receive keyboard input while this menu is open."));
    }
    return changed;
}

static void build_ui(VkExtent2D extent) {
    settings::Video& v = settings::video();
    bool changed = false;
    ImGuiIO& io = ImGui::GetIO();

    if (v.show_fps) {
        double sum = 0, worst = 0;
        for (double f : g_frame_ms) sum += f, worst = f > worst ? f : worst;
        double avg = sum / 120.0;
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text("%.0f fps  %.1f ms (worst %.0f)", avg > 0 ? 1000.0 / avg : 0.0, avg, worst);
        ImGui::End();
    }
    if (int left = hw_shader_precompile_left.load(); left > 0) {
        ImGui::SetNextWindowPos(ImVec2(8, (float)extent.height - 8), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##shaders", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text(tr("Preparing shaders... %d left"), left);
        ImGui::End();
    }
    if (display::turbo()) {
        ImGui::SetNextWindowPos(ImVec2((float)extent.width - 8, 8), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##turbo", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1), tr("Turbo %dx  (F2)"), (int)display::speed());
        ImGui::End();
    }
    if (now_seconds() < g_note_until) {
        ImGui::SetNextWindowPos(ImVec2((float)extent.width - 8, (float)extent.height - 8), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##note", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text(tr("Screenshot: %s"), g_note.c_str());
        ImGui::End();
    }
    if (!g_visible) return;

    ImGui::SetNextWindowPos(ImVec2(extent.width * 0.5f, extent.height * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 0), ImGuiCond_Appearing);
    ImGui::Begin(tr("HWDER - Options   (F1 closes)"), &g_visible, ImGuiWindowFlags_NoCollapse);
    {
        const char* langs[] = {tr("Automatic"), "English", "Português (Brasil)"};
        int cur = v.language + 1;
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo(tr("Language"), &cur, langs, 3)) {
            v.language = cur - 1;
            changed = true;
        }
    }
    if (!ImGui::BeginTabBar("##tabs")) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabItem(tr("Controls"))) {
        changed |= controls_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tr("Video"))) {
        ImGui::SeparatorText(tr("Display"));
        {
            // interp_hz: 0 off, -1 match the display, else a fixed output rate.
            const int values[] = {0, -1, 120, 144};
            const char* items[] = {tr("Off (60 fps)"), tr("Match display refresh"), "120 Hz", "144 Hz"};
            int cur = 0;
            for (int i = 0; i < 4; i++)
                if (values[i] == v.interp_hz) cur = i;
            if (ImGui::Combo(tr("Frame interpolation"), &cur, items, 4)) {
                v.interp_hz = values[cur];
                changed = true;
                // Interpolation needs the panel's full refresh rate: drop the 60 Hz monitor switch.
                if (v.interp_hz != 0 && v.display_60hz) {
                    v.display_60hz = false;
                    PostMessageW(g_hwnd, kMsgDisplay60, 0, 0);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(tr("The game runs at 60 fps. With interpolation each frame is re-rendered in between with blended\n"
                                  "3D positions (camera, characters), so motion is smooth on 120/144 Hz panels. The HUD and 2D\n"
                                  "elements show the newest frame. Costs extra GPU/CPU work per output frame, and adds one\n"
                                  "frame of latency. Turn it off if your PC can't hold the rate."));
        }
        ImGui::BeginDisabled(v.interp_hz != 0);
        if (ImGui::Checkbox(tr("Switch monitor to 60 Hz while playing"), &v.display_60hz)) {
            changed = true;
            PostMessageW(g_hwnd, kMsgDisplay60, 0, v.display_60hz ? 1 : 0);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(tr("The game renders at 60 fps and its movies at 30 fps. On a 144 Hz panel each frame is shown for\n"
                              "2 or 3 (movies: 4 or 5) refreshes in turn, which looks like stutter. At 60 Hz every frame is\n"
                              "shown exactly once (movies: twice). Reverts when the game exits. Not with frame interpolation."));
        ImGui::EndDisabled();
        ImGui::TextWrapped(tr("Game logic runs at a fixed 60 fps (the game advances one step per frame). Higher refresh rates "
                           "come from frame interpolation above."));
        if (ImGui::Checkbox(tr("VSync (FIFO)"), &v.vsync)) {
            changed = true;
            g_swapchain_dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Off = mailbox presentation (lowest latency).\nOn = classic vsync; use it if you see tearing."));

        ImGui::SeparatorText(tr("Window"));
        if (ImGui::Checkbox(tr("Borderless fullscreen (F11)"), &v.borderless)) {
            changed = true;
            post_window_change();
        }
        {
            static const int sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
            char cur[32];
            snprintf(cur, sizeof(cur), "%ux%u", extent.width, extent.height);
            ImGui::BeginDisabled(v.borderless);
            if (ImGui::BeginCombo(tr("Window size"), cur)) {
                for (auto& s : sizes) {
                    char item[32];
                    snprintf(item, sizeof(item), "%dx%d", s[0], s[1]);
                    if (ImGui::Selectable(item, (int)extent.width == s[0] && (int)extent.height == s[1])) {
                        v.window_w = s[0];
                        v.window_h = s[1];
                        changed = true;
                        post_window_change();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText(tr("Rendering"));
        {
            static const int levels[] = {0, 2, 4, 8, 16};
            const char* names[] = {tr("Game default"), tr("2x anisotropic"), tr("4x anisotropic"), tr("8x anisotropic"), tr("16x anisotropic")};
            int cur = 0;
            for (int i = 0; i < 5; i++)
                if (levels[i] == v.aniso) cur = i;
            if (ImGui::BeginCombo(tr("Texture filtering"), names[cur])) {
                for (int i = 0; i < 5; i++)
                    if (ImGui::Selectable(names[i], cur == i)) {
                        v.aniso = levels[i];
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        }
        {
            static const float steps[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
            int cur = 2;
            for (int i = 0; i < 7; i++)
                if (fabsf(steps[i] - v.res_scale) < 0.01f) cur = i;
            char label[64];
            snprintf(label, sizeof(label), "%.2fx  (%dx%d)", steps[cur], (int)(1920 * steps[cur]), (int)(1080 * steps[cur]));
            if (ImGui::BeginCombo(tr("Internal resolution"), label)) {
                for (int i = 0; i < 7; i++) {
                    char item[64];
                    snprintf(item, sizeof(item), "%.2fx  (%dx%d)", steps[i], (int)(1920 * steps[i]), (int)(1080 * steps[i]));
                    if (ImGui::Selectable(item, cur == i)) {
                        v.res_scale = steps[i];
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Checkbox(tr("Unlocked resolution (render at the window's size)"), &v.unlocked_res)) changed = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(tr("Scene render targets are allocated at this scale (UI and small post-process targets stay native).\n"
                                  "Unlocked = the window's own size and aspect (ultrawide fills the screen); it overrides the scale above.\n"
                                  "On a 1920x1080 window that equals 1.00x, i.e. the game's native resolution. Applies immediately."));
            {
                float sx = ::hwder_render_scale_x(), sy = ::hwder_render_scale_y();
                ImGui::TextDisabled(tr("Rendering 3D at %dx%d (%.2fx x %.2fx)%s"), (int)(1920 * sx + 0.5f), (int)(1080 * sy + 0.5f), sx, sy,
                                    v.unlocked_res ? tr("  [unlocked: follows the window]") : "");
            }
        }

        ImGui::SeparatorText(tr("Post-processing"));
        if (ImGui::Checkbox(tr("FXAA anti-aliasing"), &v.fxaa)) changed = true;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Smooths jagged edges of the whole picture (cheap, slightly soft). Applies immediately."));
        if (ImGui::SliderInt(tr("Sharpening"), &v.sharpen, 0, 100, "%d%%")) changed = true;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Restores crispness (good after FXAA or a resolution below the window's). 0 = off."));

        ImGui::SeparatorText("Overlay");
        if (ImGui::Checkbox(tr("Show frame time"), &v.show_fps)) changed = true;
        {
            const char* speeds[] = {"2x", "3x", "4x"};
            int cur = std::clamp(v.turbo_speed, 2, 4) - 2;
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo(tr("Turbo speed (F2)"), &cur, speeds, 3)) {
                v.turbo_speed = cur + 2;
                changed = true;
                if (display::turbo()) display::set_turbo(true);  // apply the new speed now
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("F2 speeds the whole game up (menus, cutscenes, grinding). Sound may crackle while it is on."));
            ImGui::TextDisabled(tr("F12 saves a screenshot to the screenshots folder."));
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Cheats")) {
        ImGui::SeparatorText(tr("Cheats (save file)"));
        {
            static std::string status;
            static std::vector<std::pair<size_t, size_t>> clusters;
            auto save_path = []() -> std::string {
                namespace fs = std::filesystem;
                std::error_code ec;
                for (auto& t : fs::directory_iterator("save", ec))
                    for (auto& u : fs::directory_iterator(t.path(), ec)) {
                        fs::path p = u.path() / "zmha.bin";
                        if (fs::exists(p, ec)) return p.string();
                    }
                return "";
            };
            auto read_all = [](const std::string& p) {
                std::vector<unsigned char> d;
                if (FILE* f = fopen(p.c_str(), "rb")) {
                    fseek(f, 0, SEEK_END);
                    long n = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    d.resize(n > 0 ? n : 0);
                    if (n > 0) fread(d.data(), 1, n, f);
                    fclose(f);
                }
                return d;
            };
            std::string sp = save_path();
            ImGui::TextDisabled("%s", sp.empty() ? tr("no save found under ./save") : sp.c_str());
            if (ImGui::Button(tr("Snapshot save"))) {
                auto d = read_all(sp);
                if (FILE* f = fopen("save_snapshot.bin", "wb")) { fwrite(d.data(), 1, d.size(), f); fclose(f); }
                status = tr("snapshot taken (") + std::to_string(d.size()) + tr(" bytes)");
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Diff vs snapshot"))) {
                auto a = read_all("save_snapshot.bin"), b = read_all(sp);
                clusters.clear();
                size_t n = std::min(a.size(), b.size()), start = SIZE_MAX, prev = 0, total = 0;
                for (size_t i = 0; i < n; i++) {
                    if (a[i] == b[i]) continue;
                    total++;
                    if (start == SIZE_MAX) start = i;
                    else if (i - prev > 64) { clusters.push_back({start, prev}); start = i; }
                    prev = i;
                }
                if (start != SIZE_MAX) clusters.push_back({start, prev});
                status = std::to_string(total) + tr(" bytes changed in ") + std::to_string(clusters.size()) + tr(" regions");
                for (auto& c : clusters) hw_log("cheats: save changed 0x%zx-0x%zx (%zu bytes)", c.first, c.second, c.second - c.first + 1);
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Import unlocks from reference"))) {
                // Reference = a 100% save at save_reference/zmha.bin. Every byte that is 0 here and 1 there
                // is set (unlock/clear flags); counters and other values are left alone. The game reads its
                // save at boot, so this takes effect on the next launch.
                auto ref = read_all("save_reference/zmha.bin"), cur = read_all(sp);
                size_t n = std::min(ref.size(), cur.size()), changed = 0;
                for (size_t i = 0x40; i < n; i++)
                    if (cur[i] == 0 && ref[i] == 1) { cur[i] = 1; changed++; }
                if (!ref.empty() && !cur.empty()) {
                    if (FILE* f = fopen((sp + ".bak").c_str(), "wb")) { auto o = read_all(sp); fwrite(o.data(), 1, o.size(), f); fclose(f); }
                    if (FILE* f = fopen(sp.c_str(), "wb")) { fwrite(cur.data(), 1, cur.size(), f); fclose(f); }
                    status = std::to_string(changed) + tr(" flags set - restart the game (backup: zmha.bin.bak)");
                } else {
                    status = ref.empty() ? tr("no reference save at save_reference/zmha.bin") : tr("could not read the save");
                }
            }
            if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
            for (size_t i = 0; i < clusters.size() && i < 8; i++)
                ImGui::TextDisabled("  0x%zx - 0x%zx (%zu bytes)", clusters[i].first, clusters[i].second, clusters[i].second - clusters[i].first + 1);
        }

        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    ImGui::Separator();
    ImGui::TextDisabled(tr("Settings are saved to hwder_settings.ini when changed."));
    ImGui::End();
    if (changed) settings::save();
    (void)io;
}

void render(VkCommandBuffer cb, VkImage image, VkExtent2D extent) {
    if (!g_inited) return;
    std::lock_guard<std::recursive_mutex> l(g_m);
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    g_frame_ms[g_frame_i++ % 120] = (t.QuadPart - g_last_t.QuadPart) * 1000.0 / f.QuadPart;
    g_last_t = t;
    settings::Video& v = settings::video();
    if (!g_visible) g_capture_action = -1;
    hid::set_input_block(!g_visible ? 0 : g_capture_action >= 0 ? 2 : 1);
    if (!g_visible && !v.show_fps && hw_shader_precompile_left.load() <= 0 && !display::turbo() && now_seconds() > g_note_until)
        return;
    auto vit = g_views.find(image);
    if (vit == g_views.end()) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    build_ui(extent);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount == 0) return;
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = vit->second;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    vkCmdEndRendering(cb);
}

}  // namespace overlay
