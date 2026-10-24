#include "app.h"
#include "gpu.h"
#include "pipeline.h"
#include "widgets.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dbghelp.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Ask hybrid-graphics laptops to run us on the discrete GPU (NVIDIA Optimus / AMD PowerXpress).
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

std::vector<std::wstring> g_gpuNames;
int g_gpuUsed = -1;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static std::vector<std::wstring> g_dropFiles;
int RunSelfTest(const std::wstring& in, const std::wstring& dir);
void UiTestSetup(int scenario);

// Saves the current back buffer (used by --uitest to check the layout).
static void SaveBackbuffer(const std::wstring& path) {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    g_dev->CreateTexture2D(&d, nullptr, &st);
    g_ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    std::vector<uint16_t> px((size_t)d.Width * d.Height * 4);
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; y++) {
            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (UINT x = 0; x < d.Width * 4; x++) px[(size_t)y * d.Width * 4 + x] = row[x] * 257;
        }
        g_ctx->Unmap(st, 0);
    }
    EncodeOptions eo; eo.fmt = EXP_PNG;
    SaveImage(path, eo, px, d.Width, d.Height);
    st->Release(); bb->Release();
}

static void CreateRT() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}
static void CleanupRT() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

// Hardware adapters, most powerful first (Windows' "high performance" order when available).
static std::vector<IDXGIAdapter1*> ListAdapters() {
    std::vector<IDXGIAdapter1*> list;
    IDXGIFactory1* f1 = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return list;
    IDXGIFactory6* f6 = nullptr;
    if (SUCCEEDED(f1->QueryInterface(IID_PPV_ARGS(&f6)))) {
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)) != DXGI_ERROR_NOT_FOUND; i++) list.push_back(ad);
        f6->Release();
    } else {
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; f1->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) list.push_back(ad);
        std::stable_sort(list.begin(), list.end(), [](IDXGIAdapter1* x, IDXGIAdapter1* y) {
            DXGI_ADAPTER_DESC1 dx, dy; x->GetDesc1(&dx); y->GetDesc1(&dy);
            return dx.DedicatedVideoMemory > dy.DedicatedVideoMemory;
        });
    }
    f1->Release();
    std::vector<IDXGIAdapter1*> hw;
    for (auto* ad : list) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ad->Release();
        else hw.push_back(ad);
    }
    return hw;
}

static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    // MUON_WARP=1 forces the software renderer (to check how Muon behaves on a PC without a usable GPU)
    wchar_t warp[8] = L"";
    bool forceWarp = GetEnvironmentVariableW(L"MUON_WARP", warp, 8) && warp[0] == L'1';
    auto adapters = forceWarp ? std::vector<IDXGIAdapter1*>() : ListAdapters();
    std::wstring pref = ReadGpuPreference();
    std::vector<int> order;
    for (int i = 0; i < (int)adapters.size(); i++) {
        DXGI_ADAPTER_DESC1 d; adapters[i]->GetDesc1(&d);
        g_gpuNames.push_back(d.Description);
        if (!pref.empty() && pref == d.Description) order.insert(order.begin(), i);
        else order.push_back(i);
    }
    HRESULT hr = E_FAIL;
    for (int i : order) {
        hr = D3D11CreateDeviceAndSwapChain(adapters[i], D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
        if (SUCCEEDED(hr)) { g_gpuUsed = i; break; }
    }
    for (auto* ad : adapters) ad->Release();
    if (FAILED(hr) && !forceWarp) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) // no suitable GPU: fall back to the software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRT();
    return true;
}

// Writes a minidump next to the settings and tells the user; edits are autosaved anyway.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    wchar_t base[MAX_PATH] = L"";
    std::wstring path;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        std::wstring d = std::wstring(base) + L"\\Muon";
        CreateDirectoryW(d.c_str(), nullptr);
        d += L"\\crash";
        CreateDirectoryW(d.c_str(), nullptr);
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"\\muon_%04d%02d%02d_%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        path = d + name;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(f);
        }
    }
    std::wstring msg = TW(L"Muon неожиданно закрылся. Правки сохраняются автоматически, после перезапуска можно продолжить.\n\nОтчёт об ошибке:\n",
                          L"Muon closed unexpectedly. Edits are saved automatically, so you can continue after restarting.\n\nCrash report:\n") + path;
    MessageBoxW(nullptr, msg.c_str(), L"Muon", MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void RestartSelf() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    std::wstring cur = AppCurrentPath();
    if (!cur.empty()) cmd += L" \"" + cur + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Windows delivers button messages before the (lazily generated) WM_MOUSEMOVE, so a quick
    // move-and-click would hit whatever was under the old cursor position. Take the position from the click.
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
        if (ImGui::GetCurrentContext()) ImGui::GetIO().AddMousePosEvent((float)(short)LOWORD(lParam), (float)(short)HIWORD(lParam));
        break;
    }
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = 900; mm->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = (HDROP)wParam;
        wchar_t buf[MAX_PATH * 2];
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT k = 0; k < n; k++) if (DragQueryFileW(d, k, buf, MAX_PATH * 2)) g_dropFiles.push_back(buf);
        DragFinish(d);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void LoadFonts(float dpi) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static ImVector<ImWchar> ranges;
    ranges.clear();
    ImFontGlyphRangesBuilder b;
    b.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    static const ImWchar extra[] = { 0x00B0, 0x00D7, 0x2010, 0x2027, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2700, 0x27BF, 0x27F0, 0x27FF, 0x29C9, 0x29C9, 0 };
    b.AddRanges(extra);
    b.BuildRanges(&ranges);
    static const ImWchar symRanges[] = { 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2700, 0x27BF, 0x27F0, 0x27FF, 0x29C9, 0x29C9, 0 };
    float sz = 15.5f * dpi;
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    std::string fonts = WideToUtf8(win) + "\\Fonts\\";
    auto exists = [](const std::string& p) { return GetFileAttributesW(Utf8ToWide(p).c_str()) != INVALID_FILE_ATTRIBUTES; };
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
    ImFontConfig merge; merge.MergeMode = true; merge.OversampleH = 2; merge.OversampleV = 2;
    auto addFamily = [&](const char* file, float size) -> ImFont* {
        ImFont* f = nullptr;
        if (exists(fonts + file)) f = io.Fonts->AddFontFromFileTTF((fonts + file).c_str(), size, &cfg, ranges.Data);
        if (!f) { ImFontConfig dc; dc.SizePixels = size; f = io.Fonts->AddFontDefault(&dc); return f; }
        if (exists(fonts + "seguisym.ttf")) io.Fonts->AddFontFromFileTTF((fonts + "seguisym.ttf").c_str(), size, &merge, symRanges);
        return f;
    };
    g_app.fontMain = addFamily("segoeui.ttf", sz);
    g_app.fontBold = addFamily(exists(fonts + "seguisb.ttf") ? "seguisb.ttf" : "segoeui.ttf", sz);
    g_app.fontBig = addFamily(exists(fonts + "segoeuil.ttf") ? "segoeuil.ttf" : "segoeui.ttf", 44 * dpi);
    g_app.fontSmall = addFamily("segoeui.ttf", 13.5f * dpi);
    W::SetFonts(g_app.fontBold, g_app.fontSmall);
    io.FontDefault = g_app.fontMain;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? 0 : 1;   // refined by saved settings later
    SetUnhandledExceptionFilter(CrashFilter);
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, LoadIconW(hInst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"MuonWnd", nullptr };
    RegisterClassExW(&wc);
    HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    int sw = mi.rcWork.right - mi.rcWork.left, sh = mi.rcWork.bottom - mi.rcWork.top;
    int ww = (int)(sw * 0.86), wh = (int)(sh * 0.88);
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, TW(L"Muon. Фоторедактор", L"Muon. Photo editor"), WS_OVERLAPPEDWINDOW,
                                mi.rcWork.left + (sw - ww) / 2, mi.rcWork.top + (sh - wh) / 2, ww, wh, nullptr, nullptr, hInst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF cap = RGB(19, 20, 22);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool selftest = argv && argc >= 4 && wcscmp(argv[1], L"--selftest") == 0;
    bool uitest = argv && argc >= 5 && wcscmp(argv[1], L"--uitest") == 0;
    std::wstring uiOut = uitest ? argv[3] : L"";
    if (uitest) g_app.noPersist = true;
    int uiScenario = uitest ? _wtoi(argv[4]) : 0;
    if (selftest) {
        g_quietErrors = true;
        g_logFile = WideToUtf8(argv[3]) + "\\log.txt";
        DeleteFileW((std::wstring(argv[3]) + L"\\log.txt").c_str());
    }

    if (!CreateDevice(hwnd)) {
        MessageBoxW(nullptr, TW(L"Не удалось инициализировать Direct3D 11.", L"Could not initialise Direct3D 11."), L"Muon", MB_ICONERROR);
        return 1;
    }
    if (selftest) {
        int rc = 3;
        LogLine("gpu: " + (g_gpuUsed >= 0 ? WideToUtf8(g_gpuNames[g_gpuUsed]) : std::string("software (WARP)")));
        if (g_gpu.init(g_dev, g_ctx) && g_pipe.init()) rc = RunSelfTest(argv[2], argv[3]);
        else LogLine("init failed");
        g_pipe.shutdown(); g_gpu.shutdown();
        return rc;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;   // keys are shortcuts here, not navigation
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    g_app.dpi = dpi;
    g_app.hwnd = hwnd;
    LoadFonts(dpi);
    W::SetupStyle(dpi);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    if (!g_gpu.init(g_dev, g_ctx) || !g_pipe.init()) {
        MessageBoxW(nullptr, TW(L"Не удалось скомпилировать шейдеры.", L"Could not compile the shaders."), L"Muon", MB_ICONERROR);
        return 1;
    }
    AppInit();

    if (uitest) AppOpenFile(argv[2]);
    else if (argv && argc > 1) {
        std::vector<std::wstring> files;
        for (int k = 1; k < argc; k++) files.push_back(argv[k]);
        AppDropFiles(files);
    }
    if (argv) LocalFree(argv);
    int uiFrames = 0;
    bool uiSetup = false;

    bool done = false;
    int idleFrames = 0;
    while (!done) {
        // Sleep while nothing happens, but keep animating during loading and for a few frames after input.
        bool busy = g_app.loading || g_app.batchActive || idleFrames < 6;
        if (!busy) MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        bool hadMsg = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
            hadMsg = true;
        }
        if (done) break;
        idleFrames = hadMsg ? 0 : idleFrames + 1;
        if (g_resizeW != 0 && g_resizeH != 0) {
            CleanupRT();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRT();
            idleFrames = 0;
        }
        if (!g_dropFiles.empty()) { AppDropFiles(g_dropFiles); g_dropFiles.clear(); }
        float ndpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
        if (std::abs(ndpi - g_app.dpi) > 0.01f) {
            g_app.dpi = ndpi;
            LoadFonts(ndpi);
            ImGui_ImplDX11_InvalidateDeviceObjects();
            W::SetupStyle(ndpi);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        uint32_t revBefore = g_app.rev;
        AppFrame();
        if (g_app.rev != revBefore) idleFrames = 0;
        ImGui::Render();
        const float clear[4] = { 0.075f, 0.078f, 0.085f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        D3D11_VIEWPORT vp = { 0, 0, io.DisplaySize.x, io.DisplaySize.y, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (uitest) {
            if (!g_app.loading && g_pipe.hasSource()) {
                if (!uiSetup) { UiTestSetup(uiScenario); uiSetup = true; }
                if (++uiFrames == 12) { SaveBackbuffer(uiOut); done = true; }
            }
            idleFrames = 0;
        }
        HRESULT pr = g_swap->Present(1, 0);
        if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) {
            // the video driver reset the GPU: save, restart cleanly instead of rendering garbage or crashing
            AppEmergencySave();
            MessageBoxW(hwnd, TW(L"Видеодрайвер перезапустил видеокарту. Правки сохранены, Muon сейчас откроется снова.",
                                 L"The video driver reset the GPU. Your edits are saved and Muon will reopen now."), L"Muon", MB_ICONWARNING | MB_OK);
            RestartSelf();
            ExitProcess(0);
        }
    }

    AppShutdown();
    g_pipe.shutdown();
    g_gpu.shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupRT();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
