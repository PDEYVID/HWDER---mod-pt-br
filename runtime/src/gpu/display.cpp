// Window + Vulkan device + swapchain presentation, with frame pacing.
#include "gpu/overlay.h"
#include "gpu/settings.h"
#include "display.h"

#include <timeapi.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <cmath>
#include <thread>
#include <vector>

namespace display {

static Device g_dev;
Device& device() { return g_dev; }

static HWND g_hwnd;
static std::atomic<bool> g_window_ready{false}, g_resize{false};
static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_swapchain;
static VkExtent2D g_extent;
static std::vector<VkImage> g_images;
// One "rendered" semaphore per swapchain image: a present holds its semaphore until that image is
// acquired again, so a per-frame-slot semaphore could be re-signaled while still in use.
static std::vector<VkSemaphore> g_rendered;
static VkCommandPool g_pool;
static std::mutex g_status_lock;
static std::string g_status = "booting";
static std::atomic<u64> g_last_guest_frame{0};
static double g_present_ms = 0, g_limiter_ms = 0;  // accumulated per 300 frames

#define VKCHECK(x)                                                                   \
    do {                                                                             \
        VkResult r_ = (x);                                                           \
        if (r_ != VK_SUCCESS) {                                                      \
            hw_log("Vulkan error %d at %s:%d: %s", (int)r_, __FILE__, __LINE__, #x); \
            hw_fatal("vulkan");                                                      \
        }                                                                            \
    } while (0)

static u64 now_ms() { return GetTickCount64(); }

// ------------------------------------------------------------------ window
// Borderless fullscreen on the window's monitor (F11 / overlay option).
static void set_borderless(HWND h, bool on) {
    static WINDOWPLACEMENT prev = {sizeof(prev)};
    DWORD style = GetWindowLong(h, GWL_STYLE);
    bool is_on = !(style & WS_OVERLAPPEDWINDOW);
    if (on == is_on) return;
    if (on) {
        MONITORINFO mi = {sizeof(mi)};
        GetWindowPlacement(h, &prev);
        GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &prev);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    settings::video().borderless = on;
}

// Temporary (CDS_FULLSCREEN) 60 Hz mode on the window's monitor; Windows restores it when we exit.
static void set_display_60hz(HWND h, bool on) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (!on) {
        ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
        hw_log("display: monitor refresh restored");
        return;
    }
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur);
    if (cur.dmDisplayFrequency == 60) return;
    DEVMODEW dm = cur;
    dm.dmDisplayFrequency = 60;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    LONG r = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr);
    hw_log("display: monitor %ls -> 60 Hz (%s)", mi.szDevice, r == DISP_CHANGE_SUCCESSFUL ? "ok" : "failed");
}

static void set_client_size(HWND h, int w, int h_) {
    RECT r{0, 0, w, h_};
    AdjustWindowRect(&r, GetWindowLong(h, GWL_STYLE), FALSE);
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (overlay::wndproc(h, msg, wp, lp)) return 0;
    switch (msg) {
    case WM_SIZE: g_resize = true; return 0;
    case WM_CLOSE: hw_log("window closed - exiting"); ExitProcess(0);
    case overlay::kMsgBorderless: set_borderless(h, lp != 0); return 0;
    case overlay::kMsgClientSize: set_client_size(h, (int)(lp >> 16), (int)(lp & 0xFFFF)); return 0;
    case overlay::kMsgDisplay60: set_display_60hz(h, lp != 0); return 0;
    case WM_KEYDOWN:
        if (wp == VK_F1) overlay::toggle();
        if (wp == VK_F2 && !(lp & (1 << 30))) set_turbo(!turbo());  // not on auto-repeat
        if (wp == VK_F12 && !(lp & (1 << 30))) request_screenshot();
        if (wp == VK_F11) {
            set_borderless(h, !settings::video().borderless);
            settings::save();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void window_thread() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));  // data/hwder.ico when the build embedded one
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"HWDER";
    RegisterClassExW(&wc);
    RECT r = {0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // HWDER_OFFSCREEN=1: a normal visible window (real swapchain, real presents) parked beyond the
    // desktop edge and never activated, so automated runs exercise the present path without
    // appearing on the user's screen or taking their focus.
    const char* offscreen_env = getenv("HWDER_OFFSCREEN");
    const bool offscreen = offscreen_env != nullptr;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (offscreen) {
        RECT vd{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        x = vd.left + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;  // right of every monitor
        y = vd.top + 64;
        // HWDER_OFFSCREEN=corner: a small window in the primary monitor's bottom-right corner instead.
        // A window outside every monitor presents very slowly on some laptops (hybrid graphics), which
        // makes timing measurements meaningless.
        if (!strcmp(offscreen_env, "corner")) {
            r = {0, 0, 480, 270};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            RECT wa;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            x = wa.right - (r.right - r.left);
            y = wa.bottom - (r.bottom - r.top);
        }
    }
    g_hwnd = CreateWindowExW(offscreen ? WS_EX_NOACTIVATE : 0, L"HWDER", L"Hyrule Warriors: Definitive Edition (HWDER)",
                             WS_OVERLAPPEDWINDOW, x, y, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance,
                             nullptr);
    // Automated test runs (they set HWDER_LOG) open minimized without stealing focus.
    ShowWindow(g_hwnd, offscreen ? SW_SHOWNOACTIVATE : getenv("HWDER_LOG") && !getenv("HWDER_SHOW") ? SW_SHOWMINNOACTIVE : SW_SHOW);
    g_window_ready = true;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// ------------------------------------------------------------------ Vulkan
static bool has_ext(const std::vector<VkExtensionProperties>& v, const char* n) {
    for (auto& e : v)
        if (!strcmp(e.extensionName, n)) return true;
    return false;
}

static void create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_dev.phys, g_surface, &caps);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFF) ext = {1280, 720};
    if (ext.width == 0 || ext.height == 0) return;
    u32 n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_dev.phys, g_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_dev.phys, g_surface, &n, modes.data());
    // Our frame limiter paces to the game's rate; MAILBOX avoids double pacing and adds no latency.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    if (settings::video().vsync) mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = g_surface;
    sci.minImageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = g_swapchain;
    VkSwapchainKHR sc;
    VKCHECK(vkCreateSwapchainKHR(g_dev.dev, &sci, nullptr, &sc));
    if (g_swapchain) vkDestroySwapchainKHR(g_dev.dev, g_swapchain, nullptr);
    g_swapchain = sc;
    g_extent = ext;
    vkGetSwapchainImagesKHR(g_dev.dev, sc, &n, nullptr);
    g_images.resize(n);
    vkGetSwapchainImagesKHR(g_dev.dev, sc, &n, g_images.data());
    while (g_rendered.size() < n) {  // callers recreate the swapchain only after vkDeviceWaitIdle
        VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem;
        VKCHECK(vkCreateSemaphore(g_dev.dev, &sci2, nullptr, &sem));
        g_rendered.push_back(sem);
    }
    overlay::on_swapchain(g_images, sci.imageFormat);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    // HWDER_VALIDATION: validation layer output goes to hwder.log (each distinct message id a few times).
    static std::mutex lock;
    static std::map<s32, u32> seen;
    std::lock_guard<std::mutex> g(lock);
    if (seen[data->messageIdNumber]++ >= 3) return VK_FALSE;
    hw_log("vk-validation %s: %s", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
           data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

static void log_loader() {
    HMODULE lib = GetModuleHandleA("vulkan-1.dll");
    wchar_t path[MAX_PATH] = L"?";
    if (lib) GetModuleFileNameW(lib, path, MAX_PATH);
    u32 ver = VK_API_VERSION_1_0;
    auto enum_ver = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");
    if (enum_ver) enum_ver(&ver);
    hw_log("vulkan: loader %ls (instance api %u.%u.%u)", path, VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver),
           VK_API_VERSION_PATCH(ver));
    // Implicit layers (overlays, recorders) load into every Vulkan app and are the usual suspects when
    // instance creation fails or crashes: list what the loader sees.
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    std::string names;
    for (auto& l : layers) names += std::string(" ") + l.layerName;
    hw_log("vulkan: %u layer(s) available:%s", n, names.c_str());
}

static const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

static void init_vulkan() {
    if (!hw_vk_load_global()) hw_fatal("vulkan-1.dll not found - a Vulkan capable GPU driver is required");
    log_loader();
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "HWDER";
    app.apiVersion = VK_API_VERSION_1_3;
    const bool validation = getenv("HWDER_VALIDATION") != nullptr;
    std::vector<const char*> inst_ext = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (validation) inst_ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (u32)inst_ext.size();
    ici.ppEnabledExtensionNames = inst_ext.data();
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validation) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    VkResult ir = vkCreateInstance(&ici, nullptr, &g_dev.instance);
    if (ir != VK_SUCCESS) {
        hw_log("vulkan: vkCreateInstance failed (%d)", (int)ir);
        hw_fatal("Vulkan instance creation failed - update your GPU driver (Vulkan 1.3 is required)");
    }
    hw_vk_load_instance(g_dev.instance);
    if (validation && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mci.pfnUserCallback = validation_message;
        VkDebugUtilsMessengerEXT messenger;
        vkCreateDebugUtilsMessengerEXT(g_dev.instance, &mci, nullptr, &messenger);
    }
    u32 n = 0;
    vkEnumeratePhysicalDevices(g_dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g_dev.instance, &n, pds.data());
    pds.resize(n);
    // HWDER_GPU=<index or part of the name> overrides the choice (laptops with two GPUs).
    const char* want = getenv("HWDER_GPU");
    int best_score = -1;
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        hw_log("vulkan: device %u: %s (%s, api %u.%u.%u, driver 0x%x, vendor 0x%04x)", i, p.deviceName, device_type_name(p.deviceType),
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               p.driverVersion, p.vendorID);
        int score = p.apiVersion >= VK_API_VERSION_1_3 ? 2 : 0;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 4;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 1;
        if (want && *want && ((isdigit((u8)want[0]) && (u32)atoi(want) == i) || strstr(p.deviceName, want))) score += 100;
        if (score > best_score) {
            best_score = score;
            g_dev.phys = pds[i];
            g_dev.props = p;
        }
    }
    if (!g_dev.phys) hw_fatal("No Vulkan GPU found - install or update your GPU driver");
    if (g_dev.props.apiVersion < VK_API_VERSION_1_3)
        hw_log("vulkan: WARNING: %s only supports Vulkan %u.%u; HWDER needs 1.3 - update the GPU driver", g_dev.props.deviceName,
               VK_API_VERSION_MAJOR(g_dev.props.apiVersion), VK_API_VERSION_MINOR(g_dev.props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(g_dev.phys, &g_dev.mem);
    hw_log("vulkan: %s (api %u.%u)", g_dev.props.deviceName, VK_API_VERSION_MAJOR(g_dev.props.apiVersion),
           VK_API_VERSION_MINOR(g_dev.props.apiVersion));
    VkWin32SurfaceCreateInfoKHR wsci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wsci.hinstance = GetModuleHandleW(nullptr);
    wsci.hwnd = g_hwnd;
    VKCHECK(vkCreateWin32SurfaceKHR(g_dev.instance, &wsci, nullptr, &g_surface));
    vkGetPhysicalDeviceQueueFamilyProperties(g_dev.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g_dev.phys, &n, qfs.data());
    for (u32 i = 0; i < n; i++) {
        VkBool32 present = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_dev.phys, i, g_surface, &present);
        if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_dev.queue_family = i;
            break;
        }
    }
    vkEnumerateDeviceExtensionProperties(g_dev.phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(g_dev.phys, nullptr, &n, exts.data());
    std::vector<const char*> dev_ext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    for (const char* opt : {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
                            VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME})
        if (has_ext(exts, opt)) dev_ext.push_back(opt);
    // Two queues when available: presentation must never sit in front of renderer work (a present
    // waiting for a swapchain image would otherwise block the fences the game waits on).
    u32 nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_dev.phys, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(g_dev.phys, &nqf, qfp.data());
    u32 qcount = std::min(2u, qfp[g_dev.queue_family].queueCount);
    float prio[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_dev.queue_family;
    qci.queueCount = qcount;
    qci.pQueuePriorities = prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.shaderInt8 = VK_TRUE;
    f12.storageBuffer8BitAccess = VK_TRUE;
    f12.uniformAndStorageBuffer8BitAccess = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.shaderFloat16 = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    f11.storageBuffer16BitAccess = VK_TRUE;
    f11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    f11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    VkPhysicalDeviceVulkan13Features a13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features a12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &a13};
    VkPhysicalDeviceVulkan11Features a11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &a12};
    VkPhysicalDeviceFeatures2 avail{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &a11};
    vkGetPhysicalDeviceFeatures2(g_dev.phys, &avail);
    {
        // Name what the driver lacks instead of failing vkCreateDevice with a bare error code.
        std::string missing;
        auto need = [&](VkBool32 have, const char* what) {
            if (!have) missing += std::string(" ") + what;
        };
        need(a13.dynamicRendering, "dynamicRendering");
        need(a13.synchronization2, "synchronization2");
        need(a13.maintenance4, "maintenance4");
        need(a12.shaderInt8, "shaderInt8");
        need(a12.storageBuffer8BitAccess, "storageBuffer8BitAccess");
        need(a12.uniformAndStorageBuffer8BitAccess, "uniformAndStorageBuffer8BitAccess");
        need(a12.scalarBlockLayout, "scalarBlockLayout");
        need(a12.shaderFloat16, "shaderFloat16");
        need(a12.timelineSemaphore, "timelineSemaphore");
        need(a11.storageBuffer16BitAccess, "storageBuffer16BitAccess");
        need(a11.uniformAndStorageBuffer16BitAccess, "uniformAndStorageBuffer16BitAccess");
        need(a11.shaderDrawParameters, "shaderDrawParameters");
        if (a13.shaderDemoteToHelperInvocation) {
            f13.shaderDemoteToHelperInvocation = VK_TRUE;
            g_dev.demote_to_helper = true;
        }
        if (!missing.empty()) {
            hw_log("vulkan: %s lacks required features:%s", g_dev.props.deviceName, missing.c_str());
            hw_fatal("The GPU driver lacks required Vulkan 1.3 features - update the GPU driver (see hwder.log)");
        }
    }
    feat.features = avail.features;
    feat.features.robustBufferAccess = VK_FALSE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feat};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (u32)dev_ext.size();
    dci.ppEnabledExtensionNames = dev_ext.data();
    VKCHECK(vkCreateDevice(g_dev.phys, &dci, nullptr, &g_dev.dev));
    hw_vk_load_device(g_dev.dev);
    vkGetDeviceQueue(g_dev.dev, g_dev.queue_family, 0, &g_dev.queue);
    vkGetDeviceQueue(g_dev.dev, g_dev.queue_family, qcount > 1 ? 1 : 0, &g_dev.present_queue);
    if (qcount > 1) hw_log("display: presentation uses a separate queue");
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g_dev.queue_family;
    VKCHECK(vkCreateCommandPool(g_dev.dev, &cpi, nullptr, &g_pool));
    create_swapchain();
}

// ------------------------------------------------------------------ presentation
struct FrameSync {
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
};
static FrameSync g_frames[3];
static u32 g_frame_idx;
static std::mutex g_present_lock;
static double g_trace_fence_ms, g_trace_acq_ms;  // HWDER_PACE_TRACE: last present's fence wait / acquire time
static double g_trace_overshoot_ms;               // HWDER_PACE_TRACE: last limiter sleep's wake-up lateness

static double g_output_hz = 0;
void set_output_hz(double hz) { g_output_hz = hz; }
void client_size(unsigned* w, unsigned* h) {
    *w = g_extent.width;
    *h = g_extent.height;
}
double monitor_hz() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return (double)dm.dmDisplayFrequency;
    return 60.0;
}
double game_hz() {
    static const double hz = [] {
        const char* e = getenv("HWDER_PATCH_DT");
        double v = e ? atof(e) : 0.0;
        return v >= 30.0 ? v : 60.0;
    }();
    return hz;
}

static void frame_limit(u32 interval) {
    static LARGE_INTEGER freq, next;
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const char* env = getenv("HWDER_FPS");
    // The presenter runs at a fixed 60 Hz like the console's display. A swap interval of 2 means the
    // frame stays up for two vsyncs: pace such a present two slots ahead instead of re-targeting the
    // limiter to 30 fps (the title screen alternates 1/2 every second, which made that stutter).
    // The game is frame-locked (its logic advances per released frame), so the release grid stays at
    // 60 Hz whatever the display runs at; HWDER_FPS exists only for experiments.
    double fps = env ? atof(env) : (g_output_hz > 0 ? g_output_hz : game_hz() * speed());
    static u32 last_interval = 0;
    if (interval != last_interval) {
        last_interval = interval;
        hw_log("display: swap interval %u", interval);
    }
    u32 slots = g_output_hz > 0 ? 1 : std::max(1u, std::min(interval, 4u));
    // Experiment: release every frame at the 60 Hz grid even when the game asks for interval 2.
    static const bool force1 = getenv("HWDER_FORCE_INTERVAL1") != nullptr;
    if (force1) slots = 1;
    if (fps <= 0) return;
    // The compositor's timer wake-ups must not be delayed by other busy threads (a late wake-up shows
    // up directly as a late frame followed by a short one).
    static bool prio = false;
    if (!prio) {
        prio = true;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s64 period = (s64)(freq.QuadPart / fps);
    // `next` is when the previous frame was released; this one may go out `slots` periods later. A frame
    // that arrives after its slot is released at once and the grid re-anchors to it, so the following
    // frame again stays up a full period (a display never shows frames faster than its refresh). The
    // grid used to re-anchor to now + one period here, which made the frame after every late one wait
    // two periods: heavy scenes alternated 17/33 ms frames.
    g_trace_overshoot_ms = 0;
    if (!next.QuadPart || now.QuadPart >= next.QuadPart + period * (s64)slots) {
        next = now;
        return;
    }
    next.QuadPart += period * slots;
    s64 wait = next.QuadPart - now.QuadPart;
    if (wait <= 0) return;
    s64 sleep_100ns = wait * 10000000 / freq.QuadPart - 10000;
    if (sleep_100ns > 0 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -sleep_100ns;
        SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
        WaitForSingleObject(timer, INFINITE);
        LARGE_INTEGER woke;
        QueryPerformanceCounter(&woke);
        // How late the timer woke us relative to the requested due time (pacing diagnostics).
        g_trace_overshoot_ms = ((woke.QuadPart - now.QuadPart) * 1000.0 / freq.QuadPart) - sleep_100ns / 10000.0;
    }
    do QueryPerformanceCounter(&now);
    while (now.QuadPart < next.QuadPart);
}

// ------------------------------------------------------------------ turbo (F2)
static std::atomic<bool> g_turbo{false};
void set_turbo(bool on) {
    g_turbo = on;
    hw_set_time_scale(speed());  // the game paces itself on its own clock: speed that up too
    hw_log("display: turbo %s (%.0fx)", on ? "on" : "off", speed());
}
bool turbo() { return g_turbo; }
double speed() { return g_turbo ? (double)std::clamp(settings::video().turbo_speed, 2, 4) : 1.0; }

// ------------------------------------------------------------------ screenshots (F12)
// The game image (no overlay) is blitted into a B8G8R8A8 image, copied to a host buffer by the present
// command buffer, and encoded as PNG by WIC on a worker thread.
static std::atomic<bool> g_shot_request{false};
struct Shot {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory buf_mem = VK_NULL_HANDLE;
    void* map = nullptr;
    u32 w = 0, h = 0;
};
static Shot g_shot;

void request_screenshot() { g_shot_request = true; }

static u32 mem_type(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < g_dev.mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_dev.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

// Staging image + buffer for a w x h capture (kept between screenshots of the same size).
static bool shot_resources(u32 w, u32 h) {
    if (g_shot.img && g_shot.w == w && g_shot.h == h) return true;
    if (g_shot.img) {  // the previous capture finished (its fence was waited on)
        vkUnmapMemory(g_dev.dev, g_shot.buf_mem);
        vkDestroyBuffer(g_dev.dev, g_shot.buf, nullptr);
        vkFreeMemory(g_dev.dev, g_shot.buf_mem, nullptr);
        vkDestroyImage(g_dev.dev, g_shot.img, nullptr);
        vkFreeMemory(g_dev.dev, g_shot.img_mem, nullptr);
        g_shot = {};
    }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(g_dev.dev, &ici, nullptr, &g_shot.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev.dev, g_shot.img, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(g_dev.dev, &mai, nullptr, &g_shot.img_mem) != VK_SUCCESS) return false;
    vkBindImageMemory(g_dev.dev, g_shot.img, g_shot.img_mem, 0);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g_dev.dev, &bci, nullptr, &g_shot.buf) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(g_dev.dev, g_shot.buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mai.memoryTypeIndex == ~0u)
        mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(g_dev.dev, &mai, nullptr, &g_shot.buf_mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_dev.dev, g_shot.buf, g_shot.buf_mem, 0);
    if (vkMapMemory(g_dev.dev, g_shot.buf_mem, 0, VK_WHOLE_SIZE, 0, &g_shot.map) != VK_SUCCESS) return false;
    g_shot.w = w;
    g_shot.h = h;
    return true;
}

static void shot_record(VkCommandBuffer cb, VkImage src, u32 w, u32 h) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g_shot.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = blit.dstOffsets[1] = {(s32)w, (s32)h, 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, g_shot.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, g_shot.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shot.buf, 1, &c);
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = g_shot.buf;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
}

static void shot_save(std::vector<u8> px, u32 w, u32 h) {
    std::thread([px = std::move(px), w, h]() mutable {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // the game's alpha channel is not coverage
        CreateDirectoryW(L"screenshots", nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t path[96];
        swprintf(path, 96, L"screenshots\\HWDER_%04u-%02u-%02u_%02u-%02u-%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond);
        bool ok = false;
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* fac = nullptr;
        IWICStream* st = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* fr = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
            SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
            SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
            SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h))) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            ok = SUCCEEDED(fr->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
                 SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)px.size(), px.data())) && SUCCEEDED(fr->Commit()) &&
                 SUCCEEDED(enc->Commit());
        }
        if (fr) fr->Release();
        if (enc) enc->Release();
        if (st) st->Release();
        if (fac) fac->Release();
        if (SUCCEEDED(co)) CoUninitialize();
        char u8[192];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, sizeof(u8), nullptr, nullptr);
        hw_log("display: screenshot %s %s", u8, ok ? "saved" : "FAILED");
        overlay::notify(ok ? std::string(u8) : std::string("screenshot failed"));
    }).detach();
}

static void update_title(bool guest) {
    static u64 frames, last = now_ms();
    frames++;
    u64 t = now_ms();
    if (t - last < 500) return;
    double fps = frames * 1000.0 / (t - last);
    frames = 0;
    last = t;
    std::string status;
    {
        std::lock_guard<std::mutex> g(g_status_lock);
        status = g_status;
    }
    wchar_t title[256];
    if (guest) swprintf(title, 256, L"Hyrule Warriors: Definitive Edition (HWDER) - %.1f fps", fps);
    else swprintf(title, 256, L"Hyrule Warriors: Definitive Edition (HWDER) - %hs", status.c_str());
    SetWindowTextW(g_hwnd, title);
}

// Record + submit one frame: clear, optionally blit `src`, present.
static std::mutex g_present_queue_lock;

static void present(VkImage src, u32 w, u32 h, const VkClearColorValue& bg, VkSemaphore wait_sem, u64 wait_value) {
    std::lock_guard<std::mutex> pl(g_present_lock);
    if (overlay::swapchain_dirty()) g_resize = true;
    if (g_resize.exchange(false)) {
        std::lock_guard<std::mutex> q(g_dev.queue_lock);
        std::lock_guard<std::mutex> q2(g_present_queue_lock);
        vkDeviceWaitIdle(g_dev.dev);
        create_swapchain();
    }
    if (!g_swapchain || !g_extent.width) return;
    FrameSync& fs = g_frames[g_frame_idx];
    g_frame_idx = (g_frame_idx + 1) % 3;
    if (!fs.fence) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(g_dev.dev, &sci, nullptr, &fs.acquired);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(g_dev.dev, &fci, nullptr, &fs.fence);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_pool;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g_dev.dev, &ai, &fs.cb);
    }
    LARGE_INTEGER tf, ta, tb, tc;
    QueryPerformanceFrequency(&tf);
    QueryPerformanceCounter(&ta);
    vkWaitForFences(g_dev.dev, 1, &fs.fence, VK_TRUE, UINT64_MAX);
    QueryPerformanceCounter(&tb);
    u32 idx;
    // Bounded: a hidden/minimized window may never hand out an image; the game must not stall on it.
    VkResult r = vkAcquireNextImageKHR(g_dev.dev, g_swapchain, 100000000ull, fs.acquired, VK_NULL_HANDLE, &idx);
    QueryPerformanceCounter(&tc);
    g_trace_fence_ms = (tb.QuadPart - ta.QuadPart) * 1000.0 / tf.QuadPart;
    g_trace_acq_ms = (tc.QuadPart - tb.QuadPart) * 1000.0 / tf.QuadPart;
    if (g_trace_fence_ms > 20 || g_trace_acq_ms > 20)
        hw_log("display: slow: swapchain fence wait %.0f ms, acquire %.0f ms (result %d)", g_trace_fence_ms, g_trace_acq_ms, (int)r);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_resize = true;
        return;
    }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) return;
    vkResetFences(g_dev.dev, 1, &fs.fence);
    vkResetCommandBuffer(fs.cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(fs.cb, &bi);
    VkImage dst = g_images[idx];
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(fs.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(fs.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bg, 1, &rr);
    if (src) {
        // The clear and the blit both write the swapchain image: without this barrier they are a
        // write-after-write hazard, and on AMD the clear landed after the blit in some 64x64 tiles -
        // black boxes on the presented picture (issue #2; found by Zucchyny63's frame dumps).
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(fs.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        float s = std::min((float)g_extent.width / w, (float)g_extent.height / h);
        s32 dw = (s32)(w * s), dh = (s32)(h * s);
        s32 x0 = ((s32)g_extent.width - dw) / 2, y0 = ((s32)g_extent.height - dh) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(s32)w, (s32)h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x0 + dw, y0 + dh, 1};
        vkCmdBlitImage(fs.cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
    const bool shot = src && g_shot_request.exchange(false) && shot_resources(w, h);
    if (shot) shot_record(fs.cb, src, w, h);
    // overlay (F1 menu / frame-time readout) on top of the game image
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(fs.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    LARGE_INTEGER pq0, pq1, pq2, pq3;
    QueryPerformanceCounter(&pq0);
    overlay::render(fs.cb, dst, g_extent);
    QueryPerformanceCounter(&pq1);
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(fs.cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(fs.cb);
    VkPipelineStageFlags ws[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSemaphore waits[2] = {fs.acquired, wait_sem};
    u64 wait_values[2] = {0, wait_value};
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.waitSemaphoreValueCount = wait_sem ? 2 : 1;
    tsi.pWaitSemaphoreValues = wait_values;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi};
    si.waitSemaphoreCount = wait_sem ? 2 : 1;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fs.cb;
    VkSemaphore rendered = g_rendered[idx];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &idx;
    bool shared = g_dev.present_queue == g_dev.queue;
    std::unique_lock<std::mutex> q(g_dev.queue_lock, std::defer_lock);
    std::unique_lock<std::mutex> q2(g_present_queue_lock, std::defer_lock);
    if (shared) q.lock();
    else q2.lock();
    VKCHECK(vkQueueSubmit(g_dev.present_queue, 1, &si, fs.fence));
    QueryPerformanceCounter(&pq2);
    r = vkQueuePresentKHR(g_dev.present_queue, &pi);
    QueryPerformanceCounter(&pq3);
    if ((pq3.QuadPart - pq0.QuadPart) * 1000.0 / tf.QuadPart > 20.0)
        hw_log("display: slow: present body %.0f ms (overlay %.0f, record+submit %.0f, queue-present %.0f)",
               (pq3.QuadPart - pq0.QuadPart) * 1000.0 / tf.QuadPart, (pq1.QuadPart - pq0.QuadPart) * 1000.0 / tf.QuadPart,
               (pq2.QuadPart - pq1.QuadPart) * 1000.0 / tf.QuadPart, (pq3.QuadPart - pq2.QuadPart) * 1000.0 / tf.QuadPart);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_resize = true;
    if (shot) {  // a one-off wait of a few ms; the encoding runs on its own thread
        vkWaitForFences(g_dev.dev, 1, &fs.fence, VK_TRUE, UINT64_MAX);
        const u8* p = (const u8*)g_shot.map;
        shot_save(std::vector<u8>(p, p + (size_t)w * h * 4), w, h);
    }
}

void present_image(VkImage image, u32 width, u32 height, u32 swap_interval, VkSemaphore wait_sem, u64 wait_value) {
    {
        // Frame pacing statistics: interval between guest presents, logged every 300 frames.
        static LARGE_INTEGER freq, last;
        static double sum = 0, worst = 0;
        static u32 n = 0, slow = 0;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            sum += ms;
            worst = std::max(worst, ms);
            if (ms > 20.0) slow++;
            if (ms > 100.0) hw_log("display: slow frame (%.0f ms since the previous present)", ms);
            if (++n == 300) {
                hw_log("display: 300 frames: avg %.2f ms, worst %.1f ms, %u frames over 20 ms | per frame: present %.1f ms, limiter %.1f ms",
                       sum / n, worst, slow, g_present_ms / n, g_limiter_ms / n);
                sum = worst = 0;
                n = slow = 0;
                g_present_ms = g_limiter_ms = 0;
            }
        }
        last = now;
    }
    g_last_guest_frame = now_ms();
    VkClearColorValue black{};
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    // Wait for the slot first, then present: the caller's work for the next output (frame
    // interpolation replays) overlaps the wait instead of delaying the present.
    frame_limit(swap_interval);
    QueryPerformanceCounter(&t1);
    // HWDER_HEADLESS=1 (testing): everything is rendered and paced, but nothing is shown. Presenting a
    // window costs ~40 ms per frame on some hybrid-graphics laptops, which hides the game's own timing.
    static const bool headless = getenv("HWDER_HEADLESS") != nullptr;
    if (headless) {
        if (wait_sem) {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &wait_sem;
            wi.pValues = &wait_value;
            vkWaitSemaphores(g_dev.dev, &wi, 1000000000ull);  // as a present would: the frame is finished
        }
    } else {
        present(image, width, height, black, wait_sem, wait_value);
    }
    update_title(true);
    QueryPerformanceCounter(&t2);
    g_limiter_ms += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    g_present_ms += (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart;
    // HWDER_PACE_TRACE=1: one line per present (interval since the previous present call, requested swap
    // interval, fence wait / acquire / whole present() / limiter time) for frame pacing analysis.
    static const bool trace = getenv("HWDER_PACE_TRACE") != nullptr;
    if (trace) {
        static LARGE_INTEGER prev;
        double dt = prev.QuadPart ? (t0.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart : 0;
        prev = t0;
        hw_log("pace: present dt %.2f iv %u fence %.2f acq %.2f present %.2f limit %.2f overshoot %.2f", dt, swap_interval,
               g_trace_fence_ms, g_trace_acq_ms, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
               (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart, g_trace_overshoot_ms);
    }
}

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> g(g_status_lock);
    g_status = s;
}

// Boot screen: shown until the game presents its first frame.
static void boot_screen_thread() {
    for (;;) {
        // Only before the first guest frame: afterwards a stall (loading, shader compiles) keeps the
        // last game frame on screen instead of flashing back to the boot colour.
        if (g_last_guest_frame.load() == 0 && now_ms() - g_last_guest_frame > 1000) {
            float t = (float)(now_ms() % 4000) / 4000.0f;
            float pulse = 0.5f + 0.5f * sinf(t * 6.2831853f);
            VkClearColorValue c{};
            c.float32[0] = 0.02f + 0.03f * pulse;
            c.float32[1] = 0.05f + 0.05f * pulse;
            c.float32[2] = 0.10f + 0.08f * pulse;
            c.float32[3] = 1.0f;
            present(VK_NULL_HANDLE, 0, 0, c, VK_NULL_HANDLE, 0);
            update_title(false);
        }
        Sleep(16);
    }
}

void init() {
    // Frame pacing relies on ~1 ms timer accuracy. Request it, and opt out of Windows 11's timer
    // coarsening for processes whose window is not in the foreground / minimized (test runs, or the
    // user alt-tabbing away while the game keeps running).
    timeBeginPeriod(1);
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
#endif
    settings::load();
    std::thread(window_thread).detach();
    while (!g_window_ready) Sleep(1);
    init_vulkan();
    overlay::init(g_hwnd, g_dev.instance, g_dev.phys, g_dev.dev, g_dev.queue_family, g_dev.present_queue, VK_FORMAT_B8G8R8A8_UNORM);
    if (!g_images.empty()) overlay::on_swapchain(g_images, VK_FORMAT_B8G8R8A8_UNORM);
    // Test windows (HWDER_OFFSCREEN) stay where they were put: no fullscreen, resize or refresh switch.
    if (!getenv("HWDER_OFFSCREEN")) {
        if (settings::video().display_60hz) PostMessageW(g_hwnd, overlay::kMsgDisplay60, 0, 1);
        if (settings::video().borderless) PostMessageW(g_hwnd, overlay::kMsgBorderless, 0, 1);
        else if (settings::video().window_w && settings::video().window_h)
            PostMessageW(g_hwnd, overlay::kMsgClientSize, 0, (LPARAM)((settings::video().window_w << 16) | settings::video().window_h));
    }
    std::thread(boot_screen_thread).detach();
    hw_log("display: window open");
}

}  // namespace display
