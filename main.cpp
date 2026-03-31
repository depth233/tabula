#define VK_USE_PLATFORM_WIN32_KHR

#include <vulkan/vulkan.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void vkCheck(VkResult r, const char* what) {
  if (r != VK_SUCCESS) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s (VkResult=%d)", what, (int)r);
    throw std::runtime_error(buf);
  }
}

static void copyTextToClipboard(HWND hwnd, const char* text) {
  if (!text) return;
  if (!OpenClipboard(hwnd)) return;
  EmptyClipboard();
  const size_t len = std::strlen(text) + 1;
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
  if (h) {
    void* p = GlobalLock(h);
    if (p) {
      std::memcpy(p, text, len);
      GlobalUnlock(h);
      SetClipboardData(CF_TEXT, h);
      // Ownership transferred to clipboard on success.
      h = nullptr;
    }
  }
  if (h) GlobalFree(h);
  CloseClipboard();
}

static std::string getExeDirUtf8() {
  wchar_t pathW[MAX_PATH]{};
  DWORD n = GetModuleFileNameW(nullptr, pathW, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return ".";
  std::wstring ws(pathW, pathW + n);
  size_t slash = ws.find_last_of(L"\\/");
  std::wstring dirW = (slash == std::wstring::npos) ? L"." : ws.substr(0, slash);
  int bytes = WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), (int)dirW.size(), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return ".";
  std::string out(bytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), (int)dirW.size(), out.data(), bytes, nullptr, nullptr);
  return out;
}

static std::string joinPath(const std::string& a, const char* b) {
  if (a.empty()) return b ? std::string(b) : std::string();
  if (!b || !*b) return a;
  char last = a.back();
  if (last == '\\' || last == '/') return a + b;
  return a + "\\" + b;
}

// ---- Minimal tracing (opt-in via env TABULA_TRACE=1) ----
static bool g_traceEnabled = false;
static std::mutex g_traceMu;
static LARGE_INTEGER g_qpcFreq{};
static LARGE_INTEGER g_qpcStart{};
static std::string g_tracePath;

static bool envIsOne(const char* name) {
  char buf[8]{};
  DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return false;
  return (buf[0] == '1' && buf[1] == '\0');
}

static void traceInit() {
  if (!envIsOne("TABULA_TRACE")) return;
  g_traceEnabled = true;
  QueryPerformanceFrequency(&g_qpcFreq);
  QueryPerformanceCounter(&g_qpcStart);
  const std::string exeDir = getExeDirUtf8();
  g_tracePath = joinPath(exeDir, "tabula_trace.log");
  std::lock_guard<std::mutex> lock(g_traceMu);
  std::ofstream f(g_tracePath.c_str(), std::ios::binary | std::ios::trunc);
  if (f) f << "tabula trace start\r\n";
}

static void traceEvent(const char* tag) {
  if (!g_traceEnabled) return;
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const double ms = (double)(now.QuadPart - g_qpcStart.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
  const DWORD tid = GetCurrentThreadId();
  std::lock_guard<std::mutex> lock(g_traceMu);
  std::ofstream f(g_tracePath.c_str(), std::ios::binary | std::ios::app);
  if (!f) return;
  char line[256];
  int len = std::snprintf(line, sizeof(line), "[%9.3f ms][tid %lu] %s\r\n", ms, (unsigned long)tid, tag);
  if (len > 0) f.write(line, len);
}

static std::atomic<bool> g_framebufferResized{false};
static std::atomic<uint32_t> g_fbWidth{800};
static std::atomic<uint32_t> g_fbHeight{600};
static std::atomic<bool> g_requestQuit{false};

static constexpr int kTitleBarHeightPx = 18;
static constexpr int kResizeBorderPx = 8;

static std::atomic<int> g_titleHover{0}; // 0 none, 1 min, 2 max, 3 close
static std::atomic<bool> g_inSizeMove{false};
static HANDLE g_workerDoneEvent = nullptr;
static std::atomic<bool> g_firstFramePresented{false};

struct TitleButtonRects {
  RECT min{};
  RECT max{};
  RECT close{};
};

static TitleButtonRects getTitleButtonRects(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int btn = (kTitleBarHeightPx >= 6) ? (kTitleBarHeightPx - 3) : 3;
  const int pad = 2;
  const int y0 = (kTitleBarHeightPx - btn) / 2;
  const int xClose = (rc.right - rc.left) - pad - btn;
  const int xMax = xClose - pad - btn;
  const int xMin = xMax - pad - btn;
  TitleButtonRects out;
  out.min = {xMin, y0, xMin + btn, y0 + btn};
  out.max = {xMax, y0, xMax + btn, y0 + btn};
  out.close = {xClose, y0, xClose + btn, y0 + btn};
  return out;
}

static void updateTitleHover(HWND hwnd, int xClient, int yClient) {
  int hover = 0;
  if (yClient >= 0 && yClient < kTitleBarHeightPx) {
    const auto br = getTitleButtonRects(hwnd);
    POINT pt{xClient, yClient};
    if (PtInRect(&br.min, pt)) hover = 1;
    else if (PtInRect(&br.max, pt)) hover = 2;
    else if (PtInRect(&br.close, pt)) hover = 3;
  }
  g_titleHover.store(hover, std::memory_order_relaxed);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CLOSE:
      // Graceful shutdown path.
      g_requestQuit.store(true, std::memory_order_relaxed);
      ShowWindow(hwnd, SW_HIDE);
      traceEvent("WM_CLOSE: requestQuit=1, hide window");
      return 0;
    case WM_DESTROY:
      return 0;
    case WM_SETCURSOR:
      // Prevent Windows from showing the "busy" cursor while Vulkan is starting/stopping.
      SetCursor(LoadCursor(nullptr, IDC_ARROW));
      return TRUE;
    case WM_ERASEBKGND:
      // Prevent default white background erase (we render everything with Vulkan).
      return 1;
    case WM_PAINT:
      // Keep paint handling minimal. Vulkan is the primary renderer.
      {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
      }
      return 0;
    case WM_ENTERSIZEMOVE: g_inSizeMove.store(true, std::memory_order_relaxed); return 0;
    case WM_EXITSIZEMOVE: g_inSizeMove.store(false, std::memory_order_relaxed); return 0;
    case WM_NCACTIVATE:
      return TRUE;
    case WM_NCPAINT:
      return 0;
    case WM_NCCALCSIZE: {
      // Remove the standard non-client area so the client area covers the whole window.
      // We provide resizing/dragging via WM_NCHITTEST.
      return 0;
    }
    case WM_NCHITTEST: {
      // Custom hit-testing: resizable border + draggable "title bar" region.
      POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ScreenToClient(hwnd, &p);

      RECT rc{};
      GetClientRect(hwnd, &rc);

      const bool left   = p.x < kResizeBorderPx;
      const bool right  = p.x >= (rc.right - kResizeBorderPx);
      const bool bottom = p.y >= (rc.bottom - kResizeBorderPx);

      if (p.y < kTitleBarHeightPx) {
        // Make buttons clickable (client). Only keep a central region draggable (caption)
        // so the cursor doesn't show the move/drag hint near the title bar edges.
        const auto br = getTitleButtonRects(hwnd);
        if (PtInRect(&br.min, p) || PtInRect(&br.max, p) || PtInRect(&br.close, p)) return HTCLIENT;

        // Exclude a safety margin left of the buttons and near the left edge.
        const int safety = 10;
        const int dragLeft = kResizeBorderPx + 2;
        const int dragRight = br.min.left - safety;
        if (p.x >= dragLeft && p.x < dragRight) return HTCAPTION;
        return HTCLIENT;
      }

      // Resize hit-testing for the rest of the window.
      const bool top = p.y < kResizeBorderPx;
      if (top && left) return HTTOPLEFT;
      if (top && right) return HTTOPRIGHT;
      if (bottom && left) return HTBOTTOMLEFT;
      if (bottom && right) return HTBOTTOMRIGHT;
      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;

      return HTCLIENT;
    }
    case WM_SIZE: {
      g_fbWidth.store((uint32_t)LOWORD(lParam), std::memory_order_relaxed);
      g_fbHeight.store((uint32_t)HIWORD(lParam), std::memory_order_relaxed);
      g_framebufferResized.store(true, std::memory_order_relaxed);
      return 0;
    }
    case WM_MOUSEMOVE: {
      const int x = GET_X_LPARAM(lParam);
      const int y = GET_Y_LPARAM(lParam);
      updateTitleHover(hwnd, x, y);

      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd;
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_NCMOUSEMOVE: {
      // When the cursor is over the draggable caption area, mouse moves arrive as non-client messages.
      POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ScreenToClient(hwnd, &p);
      updateTitleHover(hwnd, p.x, p.y);

      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
      tme.hwndTrack = hwnd;
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE:
    case WM_NCMOUSELEAVE: {
      g_titleHover.store(0, std::memory_order_relaxed);
      return 0;
    }
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lParam);
      const int y = GET_Y_LPARAM(lParam);
      if (y >= 0 && y < kTitleBarHeightPx) {
        const auto br = getTitleButtonRects(hwnd);
        POINT pt{x, y};
        if (PtInRect(&br.min, pt)) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
        if (PtInRect(&br.max, pt)) { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
        if (PtInRect(&br.close, pt)) { PostMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
      }
      return 0;
    }
    case WM_KEYDOWN: {
      if (wParam == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
      }
      return 0;
    }
    default: return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

struct QueueFamilyIndices {
  std::optional<uint32_t> graphics;
  std::optional<uint32_t> present;
  bool complete() const { return graphics.has_value() && present.has_value(); }
};

static QueueFamilyIndices findQueueFamilies(VkPhysicalDevice phys, VkSurfaceKHR surface) {
  QueueFamilyIndices out;
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
  std::vector<VkQueueFamilyProperties> props(count);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props.data());

  for (uint32_t i = 0; i < count; ++i) {
    if (props[i].queueCount > 0 && (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) out.graphics = i;

    VkBool32 supported = VK_FALSE;
    vkCheck(vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &supported),
            "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (supported) out.present = i;

    if (out.complete()) break;
  }
  return out;
}

struct SwapchainSupport {
  VkSurfaceCapabilitiesKHR caps{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> presentModes;
};

static SwapchainSupport querySwapchainSupport(VkPhysicalDevice phys, VkSurfaceKHR surface) {
  SwapchainSupport out;
  vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &out.caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

  uint32_t fmtCount = 0;
  vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, nullptr),
          "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
  out.formats.resize(fmtCount);
  vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, out.formats.data()),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");

  uint32_t pmCount = 0;
  vkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pmCount, nullptr),
          "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");
  out.presentModes.resize(pmCount);
  vkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pmCount, out.presentModes.data()),
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
  return out;
}

static VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
  for (auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return f;
  }
  return formats[0];
}

static VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) {
  for (auto m : modes) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t w, uint32_t h) {
  if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
  VkExtent2D e{w, h};
  e.width = (std::max)(caps.minImageExtent.width, (std::min)(caps.maxImageExtent.width, e.width));
  e.height = (std::max)(caps.minImageExtent.height, (std::min)(caps.maxImageExtent.height, e.height));
  return e;
}

static bool hasDeviceExtension(VkPhysicalDevice phys, const char* name) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> exts(count);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, exts.data());
  for (auto& e : exts) {
    if (std::string(e.extensionName) == name) return true;
  }
  return false;
}

static std::vector<char> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error(std::string("Failed to open file: ") + path);
  auto size = (size_t)f.tellg();
  std::vector<char> data(size);
  f.seekg(0);
  f.read(data.data(), (std::streamsize)size);
  return data;
}

static bool tryReadFile(const char* path, std::vector<char>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  auto size = (size_t)f.tellg();
  out.resize(size);
  f.seekg(0);
  f.read(out.data(), (std::streamsize)size);
  return true;
}

static bool tryWriteFile(const char* path, const void* data, size_t size) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
  return (bool)f;
}

static VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& bytes) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
  VkShaderModule m{};
  vkCheck(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
  return m;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
  try {
    traceInit();
    traceEvent("WinMain: start");

    const wchar_t* kClassName = L"TabulaVulkanWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClassW failed");

    const int kWidth = 360;
    const int kHeight = 520;

    // Small borderless window with custom draggable top area and resizable borders.
    // WS_THICKFRAME enables resizing; WS_MINIMIZEBOX/WS_MAXIMIZEBOX/SYSMENU keep standard behaviors.
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    RECT wr{0, 0, kWidth, kHeight};
    AdjustWindowRectEx(&wr, style, FALSE, 0);

    const int winW = wr.right - wr.left;
    const int winH = wr.bottom - wr.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(
        0, kClassName, L"Vulkan Triangle (Esc quit)", style,
        x, y, winW, winH,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) throw std::runtime_error("CreateWindowExW failed");
    traceEvent("CreateWindowExW: ok");

    // Force non-client recalculation with our custom NCCALCSIZE.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    const std::string exeDir = getExeDirUtf8();
    const std::string shaderVertPath = joinPath(exeDir, "shaders\\triangle.vert.spv");
    const std::string shaderFragPath = joinPath(exeDir, "shaders\\triangle.frag.spv");
    // Shader bytes + pipeline cache are loaded in the worker thread.

    // Show immediately; Vulkan will draw the first frame shortly after.
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    traceEvent("ShowWindow/UpdateWindow: done");

    RECT cr{};
    GetClientRect(hwnd, &cr);
    g_fbWidth.store((uint32_t)(cr.right - cr.left), std::memory_order_relaxed);
    g_fbHeight.store((uint32_t)(cr.bottom - cr.top), std::memory_order_relaxed);

    std::exception_ptr workerError;
    std::atomic<bool> workerDone{false};
    g_workerDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_workerDoneEvent) throw std::runtime_error("CreateEventW failed");
    // Lower worker priority so UI stays responsive.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    std::thread worker([&]() {
      struct DoneGuard {
        std::atomic<bool>& done;
        ~DoneGuard() { done.store(true, std::memory_order_relaxed); }
      } doneGuard{workerDone};
      try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        traceEvent("worker: start");
        const std::string exeDir = getExeDirUtf8();
        const std::string shaderVertPath = joinPath(exeDir, "shaders\\triangle.vert.spv");
        const std::string shaderFragPath = joinPath(exeDir, "shaders\\triangle.frag.spv");
        const auto shaderVertBytes = readFile(shaderVertPath.c_str());
        const auto shaderFragBytes = readFile(shaderFragPath.c_str());
        traceEvent("worker: shaders loaded");
        const std::string pipelineCachePath = joinPath(exeDir, "pipeline_cache.bin");

        // ---- Vulkan instance ----
        std::vector<const char*> instExts = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "tabula_vulkan_triangle";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "none";
        app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = (uint32_t)instExts.size();
        ici.ppEnabledExtensionNames = instExts.data();

        VkInstance instance{};
        vkCheck(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
        traceEvent("worker: vkCreateInstance ok");

        // ---- Surface ----
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = hInstance;
        sci.hwnd = hwnd;
        VkSurfaceKHR surface{};
        vkCheck(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        traceEvent("worker: vkCreateWin32SurfaceKHR ok");

        // ---- Physical device ----
        uint32_t devCount = 0;
        vkCheck(vkEnumeratePhysicalDevices(instance, &devCount, nullptr), "vkEnumeratePhysicalDevices(count)");
        if (devCount == 0) throw std::runtime_error("No Vulkan physical devices found");
        std::vector<VkPhysicalDevice> phys(devCount);
        vkCheck(vkEnumeratePhysicalDevices(instance, &devCount, phys.data()), "vkEnumeratePhysicalDevices");

        VkPhysicalDevice physical = VK_NULL_HANDLE;
        QueueFamilyIndices q{};
        for (auto d : phys) {
          if (!hasDeviceExtension(d, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
          auto indices = findQueueFamilies(d, surface);
          if (!indices.complete()) continue;
          auto support = querySwapchainSupport(d, surface);
          if (support.formats.empty() || support.presentModes.empty()) continue;
          physical = d;
          q = indices;
          break;
        }
        if (physical == VK_NULL_HANDLE) throw std::runtime_error("No suitable Vulkan device (needs graphics+present+swapchain)");

        // ---- Logical device ----
        float prio = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> qcis;
        {
          std::vector<uint32_t> unique = {q.graphics.value()};
          if (q.present.value() != q.graphics.value()) unique.push_back(q.present.value());
          qcis.reserve(unique.size());
          for (auto fam : unique) {
            VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qci.queueFamilyIndex = fam;
            qci.queueCount = 1;
            qci.pQueuePriorities = &prio;
            qcis.push_back(qci);
          }
        }

        const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = (uint32_t)qcis.size();
        dci.pQueueCreateInfos = qcis.data();
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExts;

        VkDevice device{};
        vkCheck(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        traceEvent("worker: vkCreateDevice ok");
        VkQueue graphicsQueue{};
        VkQueue presentQueue{};
        vkGetDeviceQueue(device, q.graphics.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, q.present.value(), 0, &presentQueue);

        // ---- Pipeline cache ----
        VkPipelineCache pipelineCache = VK_NULL_HANDLE;
        {
          std::vector<char> cacheBytes;
          VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
          if (tryReadFile(pipelineCachePath.c_str(), cacheBytes) && !cacheBytes.empty()) {
            pcci.initialDataSize = cacheBytes.size();
            pcci.pInitialData = cacheBytes.data();
          }
          if (vkCreatePipelineCache(device, &pcci, nullptr, &pipelineCache) != VK_SUCCESS) {
            pcci.initialDataSize = 0;
            pcci.pInitialData = nullptr;
            vkCheck(vkCreatePipelineCache(device, &pcci, nullptr, &pipelineCache), "vkCreatePipelineCache");
          }
        }

        // ---- Command pool & swapchain resources ----
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = q.graphics.value();
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool cmdPool{};
        vkCheck(vkCreateCommandPool(device, &cpci, nullptr, &cmdPool), "vkCreateCommandPool");

        VkSwapchainKHR swapchain{};
        VkExtent2D extent{};
        VkSurfaceFormatKHR surfaceFormat{};
        std::vector<VkImage> swapImages;
        std::vector<VkImageView> swapViews;
        VkRenderPass renderPass{};
        VkPipelineLayout pipelineLayout{};
        VkPipeline pipeline{};
        std::vector<VkFramebuffer> framebuffers;
        std::vector<VkCommandBuffer> cmdBufs;

        auto cleanupSwapchain = [&](bool destroySwapchain) {
          if (!cmdBufs.empty()) {
            vkFreeCommandBuffers(device, cmdPool, (uint32_t)cmdBufs.size(), cmdBufs.data());
            cmdBufs.clear();
          }
          for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
          framebuffers.clear();
          if (pipeline) vkDestroyPipeline(device, pipeline, nullptr), pipeline = VK_NULL_HANDLE;
          if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr), pipelineLayout = VK_NULL_HANDLE;
          if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr), renderPass = VK_NULL_HANDLE;
          for (auto v : swapViews) vkDestroyImageView(device, v, nullptr);
          swapViews.clear();
          if (destroySwapchain && swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr), swapchain = VK_NULL_HANDLE;
        };

        auto createSwapchain = [&](VkSwapchainKHR oldSwapchain) {
          SwapchainSupport scs = querySwapchainSupport(physical, surface);
          surfaceFormat = chooseSurfaceFormat(scs.formats);
          VkPresentModeKHR presentMode = choosePresentMode(scs.presentModes);
          extent = chooseExtent(scs.caps,
                                g_fbWidth.load(std::memory_order_relaxed),
                                g_fbHeight.load(std::memory_order_relaxed));

          uint32_t imageCount = scs.caps.minImageCount + 1;
          if (scs.caps.maxImageCount > 0 && imageCount > scs.caps.maxImageCount) imageCount = scs.caps.maxImageCount;

          VkSwapchainCreateInfoKHR scci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
          scci.surface = surface;
          scci.minImageCount = imageCount;
          scci.imageFormat = surfaceFormat.format;
          scci.imageColorSpace = surfaceFormat.colorSpace;
          scci.imageExtent = extent;
          scci.imageArrayLayers = 1;
          scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

          uint32_t qIdx[2] = {q.graphics.value(), q.present.value()};
          if (q.graphics.value() != q.present.value()) {
            scci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            scci.queueFamilyIndexCount = 2;
            scci.pQueueFamilyIndices = qIdx;
          } else {
            scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
          }
          scci.preTransform = scs.caps.currentTransform;
          scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
          scci.presentMode = presentMode;
          scci.clipped = VK_TRUE;
          scci.oldSwapchain = oldSwapchain;

          vkCheck(vkCreateSwapchainKHR(device, &scci, nullptr, &swapchain), "vkCreateSwapchainKHR");

          uint32_t scImgCount = 0;
          vkCheck(vkGetSwapchainImagesKHR(device, swapchain, &scImgCount, nullptr),
                  "vkGetSwapchainImagesKHR(count)");
          swapImages.resize(scImgCount);
          vkCheck(vkGetSwapchainImagesKHR(device, swapchain, &scImgCount, swapImages.data()),
                  "vkGetSwapchainImagesKHR");

          swapViews.resize(scImgCount);
          for (uint32_t i = 0; i < scImgCount; ++i) {
            VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            iv.image = swapImages[i];
            iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            iv.format = surfaceFormat.format;
            iv.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            iv.subresourceRange.baseMipLevel = 0;
            iv.subresourceRange.levelCount = 1;
            iv.subresourceRange.baseArrayLayer = 0;
            iv.subresourceRange.layerCount = 1;
            vkCheck(vkCreateImageView(device, &iv, nullptr, &swapViews[i]), "vkCreateImageView");
          }
        };

        auto createRenderPassPipelineAndCmds = [&]() {
          VkAttachmentDescription color{};
          color.format = surfaceFormat.format;
          color.samples = VK_SAMPLE_COUNT_1_BIT;
          color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
          color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
          color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
          color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

          VkAttachmentReference colorRef{};
          colorRef.attachment = 0;
          colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

          VkSubpassDescription subpass{};
          subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
          subpass.colorAttachmentCount = 1;
          subpass.pColorAttachments = &colorRef;

          VkSubpassDependency dep{};
          dep.srcSubpass = VK_SUBPASS_EXTERNAL;
          dep.dstSubpass = 0;
          dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
          dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
          dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

          VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
          rpci.attachmentCount = 1;
          rpci.pAttachments = &color;
          rpci.subpassCount = 1;
          rpci.pSubpasses = &subpass;
          rpci.dependencyCount = 1;
          rpci.pDependencies = &dep;

          vkCheck(vkCreateRenderPass(device, &rpci, nullptr, &renderPass), "vkCreateRenderPass");

          VkShaderModule vert = createShaderModule(device, shaderVertBytes);
          VkShaderModule frag = createShaderModule(device, shaderFragBytes);

          VkPipelineShaderStageCreateInfo stages[2]{};
          stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
          stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
          stages[0].module = vert;
          stages[0].pName = "main";
          stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
          stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
          stages[1].module = frag;
          stages[1].pName = "main";

          VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
          VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
          ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

          VkViewport vp{};
          vp.x = 0.f; vp.y = 0.f;
          vp.width = (float)extent.width;
          vp.height = (float)extent.height;
          vp.minDepth = 0.f; vp.maxDepth = 1.f;
          VkRect2D scissor{{0,0}, extent};
          VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
          vpState.viewportCount = 1;
          vpState.pViewports = &vp;
          vpState.scissorCount = 1;
          vpState.pScissors = &scissor;

          VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
          rs.polygonMode = VK_POLYGON_MODE_FILL;
          rs.cullMode = VK_CULL_MODE_NONE;
          rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
          rs.lineWidth = 1.0f;

          VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
          ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

          VkPipelineColorBlendAttachmentState cbAtt{};
          cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
          VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
          cb.attachmentCount = 1;
          cb.pAttachments = &cbAtt;

          VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
          VkPushConstantRange pcr{};
          pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
          pcr.offset = 0;
          pcr.size = sizeof(float) * 2 + sizeof(int32_t) * 2;
          plci.pushConstantRangeCount = 1;
          plci.pPushConstantRanges = &pcr;
          vkCheck(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout), "vkCreatePipelineLayout");

          VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
          gpci.stageCount = 2;
          gpci.pStages = stages;
          gpci.pVertexInputState = &vi;
          gpci.pInputAssemblyState = &ia;
          gpci.pViewportState = &vpState;
          gpci.pRasterizationState = &rs;
          gpci.pMultisampleState = &ms;
          gpci.pColorBlendState = &cb;
          gpci.layout = pipelineLayout;
          gpci.renderPass = renderPass;
          gpci.subpass = 0;

          vkCheck(vkCreateGraphicsPipelines(device, pipelineCache, 1, &gpci, nullptr, &pipeline),
                  "vkCreateGraphicsPipelines");

          vkDestroyShaderModule(device, frag, nullptr);
          vkDestroyShaderModule(device, vert, nullptr);

          framebuffers.resize(swapViews.size());
          for (size_t i = 0; i < swapViews.size(); ++i) {
            VkImageView att[] = {swapViews[i]};
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments = att;
            fci.width = extent.width;
            fci.height = extent.height;
            fci.layers = 1;
            vkCheck(vkCreateFramebuffer(device, &fci, nullptr, &framebuffers[i]), "vkCreateFramebuffer");
          }

          cmdBufs.resize(swapViews.size());
          VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
          cbai.commandPool = cmdPool;
          cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
          cbai.commandBufferCount = (uint32_t)cmdBufs.size();
          vkCheck(vkAllocateCommandBuffers(device, &cbai, cmdBufs.data()), "vkAllocateCommandBuffers");
        };

        auto recreateSwapchain = [&]() {
          if (g_requestQuit.load(std::memory_order_relaxed)) return;
          vkQueueWaitIdle(graphicsQueue);
          vkQueueWaitIdle(presentQueue);

          while (g_fbWidth.load(std::memory_order_relaxed) == 0 || g_fbHeight.load(std::memory_order_relaxed) == 0) {
            if (g_requestQuit.load(std::memory_order_relaxed)) return;
            Sleep(16);
          }

          VkSwapchainKHR old = swapchain;
          cleanupSwapchain(false);
          createSwapchain(old);
          if (old) vkDestroySwapchainKHR(device, old, nullptr);
          createRenderPassPipelineAndCmds();
          g_framebufferResized.store(false, std::memory_order_relaxed);
        };

        recreateSwapchain();
        traceEvent("worker: swapchain created");

        static constexpr uint32_t kMaxFramesInFlight = 2;
        VkSemaphoreCreateInfo sciSem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fciFence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fciFence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable{};
        std::array<VkSemaphore, kMaxFramesInFlight> renderFinished{};
        std::array<VkFence, kMaxFramesInFlight> inFlight{};
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
          vkCheck(vkCreateSemaphore(device, &sciSem, nullptr, &imageAvailable[i]), "vkCreateSemaphore(imageAvailable)");
          vkCheck(vkCreateSemaphore(device, &sciSem, nullptr, &renderFinished[i]), "vkCreateSemaphore(renderFinished)");
          vkCheck(vkCreateFence(device, &fciFence, nullptr, &inFlight[i]), "vkCreateFence(inFlight)");
        }
        uint32_t currentFrame = 0;

        auto renderOnce = [&]() {
          if (g_requestQuit.load(std::memory_order_relaxed)) return;
          if (g_framebufferResized.load(std::memory_order_relaxed)) recreateSwapchain();
          if (g_requestQuit.load(std::memory_order_relaxed)) return;

          // Don't block forever in worker thread; long waits can trigger Windows busy cursor.
          VkResult wf = vkWaitForFences(device, 1, &inFlight[currentFrame], VK_TRUE, 100000000ull /*100ms*/);
          if (wf == VK_TIMEOUT) return;
          vkCheck(wf, "vkWaitForFences");
          vkCheck(vkResetFences(device, 1, &inFlight[currentFrame]), "vkResetFences");

          uint32_t imageIndex = 0;
          VkResult acq = vkAcquireNextImageKHR(device, swapchain, 100000000ull /*100ms*/,
                                                 imageAvailable[currentFrame], VK_NULL_HANDLE, &imageIndex);
          if (acq == VK_TIMEOUT) return;
          if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) {
            g_framebufferResized.store(true, std::memory_order_relaxed);
            return;
          }
          vkCheck(acq, "vkAcquireNextImageKHR");

          vkCheck(vkResetCommandBuffer(cmdBufs[imageIndex], 0), "vkResetCommandBuffer");
          VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
          vkCheck(vkBeginCommandBuffer(cmdBufs[imageIndex], &bi), "vkBeginCommandBuffer");

          VkClearValue clear{};
          clear.color = {{0.10f, 0.11f, 0.13f, 1.0f}};
          VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
          rbi.renderPass = renderPass;
          rbi.framebuffer = framebuffers[imageIndex];
          rbi.renderArea = {{0,0}, extent};
          rbi.clearValueCount = 1;
          rbi.pClearValues = &clear;
          vkCmdBeginRenderPass(cmdBufs[imageIndex], &rbi, VK_SUBPASS_CONTENTS_INLINE);
          vkCmdBindPipeline(cmdBufs[imageIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          struct Push { float extent_[2]; int hover; int pad; } pc;
          pc.extent_[0] = (float)extent.width;
          pc.extent_[1] = (float)extent.height;
          pc.hover = g_titleHover.load(std::memory_order_relaxed);
          pc.pad = 0;
          vkCmdPushConstants(cmdBufs[imageIndex], pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

          vkCmdDraw(cmdBufs[imageIndex], 3, 1, 0, 0);
          vkCmdDraw(cmdBufs[imageIndex], 6, 1, 100, 0);
          vkCmdDraw(cmdBufs[imageIndex], 6, 1, 110, 0);
          vkCmdDraw(cmdBufs[imageIndex], 6, 1, 120, 0);
          vkCmdDraw(cmdBufs[imageIndex], 6, 1, 130, 0);
          vkCmdDraw(cmdBufs[imageIndex], 6, 1, 140, 0);
          vkCmdDraw(cmdBufs[imageIndex], 24, 1, 150, 0);
          vkCmdDraw(cmdBufs[imageIndex], 12, 1, 180, 0);
          vkCmdEndRenderPass(cmdBufs[imageIndex]);
          vkCheck(vkEndCommandBuffer(cmdBufs[imageIndex]), "vkEndCommandBuffer");

          VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
          VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
          si.waitSemaphoreCount = 1;
          si.pWaitSemaphores = &imageAvailable[currentFrame];
          si.pWaitDstStageMask = &waitStage;
          si.commandBufferCount = 1;
          si.pCommandBuffers = &cmdBufs[imageIndex];
          si.signalSemaphoreCount = 1;
          si.pSignalSemaphores = &renderFinished[currentFrame];
          vkCheck(vkQueueSubmit(graphicsQueue, 1, &si, inFlight[currentFrame]), "vkQueueSubmit");

          VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
          pi.waitSemaphoreCount = 1;
          pi.pWaitSemaphores = &renderFinished[currentFrame];
          pi.swapchainCount = 1;
          pi.pSwapchains = &swapchain;
          pi.pImageIndices = &imageIndex;
          VkResult pres = vkQueuePresentKHR(presentQueue, &pi);
          if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
            g_framebufferResized.store(true, std::memory_order_relaxed);
            return;
          }
          vkCheck(pres, "vkQueuePresentKHR");

          currentFrame = (currentFrame + 1) % kMaxFramesInFlight;

          // First successful present: now it's safe to show the window.
          g_firstFramePresented.store(true, std::memory_order_relaxed);
        };

        // Render loop
        while (!g_requestQuit.load(std::memory_order_relaxed)) {
          renderOnce();
          // Throttle to avoid a tight render spin causing Windows to treat the app as busy.
          Sleep(1);
        }

        // Fast shutdown: non-blocking fence checks.
        // We avoid long waits that can trigger Windows busy/hourglass cursor.
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
          (void)vkWaitForFences(device, 1, &inFlight[i], VK_TRUE, 0);
        }

        // ---- Cleanup ----
        // For startup performance we still use the pipeline cache, but skip persisting it on exit
        // to keep shutdown snappy (avoids disk I/O during closing).
        if (pipelineCache) {
          vkDestroyPipelineCache(device, pipelineCache, nullptr);
          pipelineCache = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
          vkDestroyFence(device, inFlight[i], nullptr);
          vkDestroySemaphore(device, renderFinished[i], nullptr);
          vkDestroySemaphore(device, imageAvailable[i], nullptr);
        }

        vkDestroyCommandPool(device, cmdPool, nullptr);
        cleanupSwapchain(true);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        traceEvent("worker: cleanup done");
      } catch (...) {
        workerError = std::current_exception();
      }
      SetEvent(g_workerDoneEvent);
      traceEvent("worker: signaled done");
    });

    // UI loop: always responsive even during shutdown.
    // Use a short timeout so we can keep the cursor responsive.
    HCURSOR arrowCursor = LoadCursor(nullptr, IDC_ARROW);
    while (true) {
      DWORD r = MsgWaitForMultipleObjects(1, &g_workerDoneEvent, FALSE, 50, QS_ALLINPUT);
      if (r == WAIT_OBJECT_0) break;
      MSG m;
      while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
      }
      SetCursor(arrowCursor);
      // UI thread stays responsive; Vulkan rendering happens on worker thread.
    }
    traceEvent("UI: worker done observed");
    if (worker.joinable()) worker.join();
    traceEvent("UI: worker joined");
    if (g_workerDoneEvent) {
      CloseHandle(g_workerDoneEvent);
      g_workerDoneEvent = nullptr;
    }
    if (workerError) std::rethrow_exception(workerError);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    traceEvent("WinMain: end");
    return 0;
  } catch (const std::exception& e) {
    copyTextToClipboard(nullptr, e.what());
    std::string msg = std::string(e.what()) + "\r\n\r\n(已复制到剪贴板，可直接粘贴)";
    MessageBoxA(nullptr, msg.c_str(), "Fatal error", MB_ICONERROR | MB_OK);
    return 1;
  }
}

