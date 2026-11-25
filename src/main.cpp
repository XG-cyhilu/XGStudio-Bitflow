// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>

#include <memory>
#include <string>

#include <xglog/xglog.h>

#include "bridge.h"
#include "settings.h"
#include "text.h"
#include "WebAssets.generated.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* kHostOrigin = L"https://bitflow.local/";
const wchar_t* kWindowClass = L"BitflowHostWindow";

HWND g_hwnd = nullptr;
HINSTANCE g_hinst = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_ctrl;
ComPtr<ICoreWebView2> g_web;
std::unique_ptr<Bridge> g_bridge;
bool g_dark = false;

const wchar_t* CurrentProcessPath() {
    static wchar_t buf[MAX_PATH];
    static bool init = false;
    if (!init) {
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        init = true;
    }
    return buf;
}

bool SystemUsesDarkTheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return value == 0;
}

COLORREF SurfaceColor() {
    return g_dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
}

void ApplyWindowTheme(HWND hwnd) {
    BOOL dark = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF caption = SurfaceColor();
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    COLORREF text = g_dark ? RGB(255, 255, 255) : RGB(32, 32, 32);
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
    COLORREF border = g_dark ? RGB(58, 58, 58) : RGB(224, 224, 224);
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));
}

COREWEBVIEW2_COLOR WebViewBackground() {
    COREWEBVIEW2_COLOR c;
    c.A = 255;
    c.R = GetRValue(SurfaceColor());
    c.G = GetGValue(SurfaceColor());
    c.B = GetBValue(SurfaceColor());
    return c;
}

std::wstring DataDirectory() {
    std::wstring base;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        base = local;
        CoTaskMemFree(local);
    } else {
        base = L".";
    }
    std::wstring dir = base + L"\\XGStudio\\BitFlow\\WebView2";
    std::wstring acc;
    for (size_t i = 0; i < dir.size(); ++i) {
        acc.push_back(dir[i]);
        if (dir[i] == L'\\' && acc.size() > 3) CreateDirectoryW(acc.c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

const wchar_t* MimeFor(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"application/octet-stream";
    std::wstring ext = path.substr(dot + 1);
    for (auto& ch : ext) ch = towlower(ch);
    if (ext == L"html" || ext == L"htm") return L"text/html; charset=utf-8";
    if (ext == L"css") return L"text/css; charset=utf-8";
    if (ext == L"js") return L"text/javascript; charset=utf-8";
    if (ext == L"json") return L"application/json; charset=utf-8";
    if (ext == L"svg") return L"image/svg+xml";
    if (ext == L"png") return L"image/png";
    if (ext == L"jpg" || ext == L"jpeg") return L"image/jpeg";
    if (ext == L"ico") return L"image/x-icon";
    if (ext == L"woff2") return L"font/woff2";
    if (ext == L"ttf") return L"font/ttf";
    return L"application/octet-stream";
}

const BitflowAsset* FindAsset(const std::wstring& relative) {
    for (int i = 0; i < kBitflowAssetCount; ++i) {
        if (relative == kBitflowAssets[i].path) return &kBitflowAssets[i];
    }
    return nullptr;
}

HRESULT Respond(ICoreWebView2WebResourceRequestedEventArgs* args, int status, const wchar_t* reason,
                const wchar_t* mime, const void* data, size_t size) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (!mem) return E_OUTOFMEMORY;
    void* dst = GlobalLock(mem);
    if (size) memcpy(dst, data, size);
    GlobalUnlock(mem);

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(mem, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(mem);
        return hr;
    }

    std::wstring headers = std::wstring(L"Content-Type: ") + mime +
                           L"\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n";
    ComPtr<ICoreWebView2WebResourceResponse> response;
    hr = g_env->CreateWebResourceResponse(stream, status, reason, headers.c_str(), &response);
    stream->Release();
    if (FAILED(hr)) return hr;

    return args->put_Response(response.Get());
}

HRESULT ServeAsset(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    if (!request) return E_FAIL;

    LPWSTR rawUri = nullptr;
    request->get_Uri(&rawUri);
    std::wstring uri = rawUri ? rawUri : L"";
    CoTaskMemFree(rawUri);

    std::wstring relative = L"index.html";
    if (uri.rfind(kHostOrigin, 0) == 0) relative = uri.substr(wcslen(kHostOrigin));
    size_t cut = relative.find_first_of(L"?#");
    if (cut != std::wstring::npos) relative = relative.substr(0, cut);
    if (relative.empty() || relative == L"/") relative = L"index.html";
    if (!relative.empty() && relative[0] == L'/') relative.erase(0, 1);

    const BitflowAsset* asset = FindAsset(relative);
    if (!asset) {
        const char* body = "404";
        return Respond(args, 404, L"Not Found", L"text/plain; charset=utf-8", body, 3);
    }

    HRSRC res = FindResourceW(g_hinst, MAKEINTRESOURCE(asset->id), RT_RCDATA);
    if (!res) return Respond(args, 500, L"Internal Error", L"text/plain", "missing", 7);

    HGLOBAL loaded = LoadResource(g_hinst, res);
    DWORD size = SizeofResource(g_hinst, res);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes) return Respond(args, 500, L"Internal Error", L"text/plain", "locked", 6);

    return Respond(args, 200, L"OK", MimeFor(asset->path), bytes, size);
}

void ResizeWebView() {
    if (!g_ctrl) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_ctrl->put_Bounds(RECT{0, 0, rc.right - rc.left, rc.bottom - rc.top});
}

void ApplyWebViewTheme() {
    ComPtr<ICoreWebView2Controller2> ctrl2;
    if (SUCCEEDED(g_ctrl.As(&ctrl2))) {
        COREWEBVIEW2_COLOR bg = WebViewBackground();
        ctrl2->put_DefaultBackgroundColor(bg);
    }
    if (g_web) {
        std::string theme = g_dark ? "dark" : "light";
        std::string msg = "{\"type\":\"theme\",\"theme\":\"" + theme + "\"}";
        g_web->PostWebMessageAsJson(U8ToW(msg).c_str());
    }
}

void ConfigureWebView() {
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_web->get_Settings(&settings)) && settings) {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsBuiltInErrorPageEnabled(FALSE);
    }

    g_ctrl->put_IsVisible(TRUE);
    ApplyWebViewTheme();

    g_web->AddWebResourceRequestedFilter(L"https://bitflow.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken token{};
    g_web->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                return ServeAsset(args);
            }).Get(),
        &token);

    g_web->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                args->get_Uri(&raw);
                std::wstring uri = raw ? raw : L"";
                CoTaskMemFree(raw);
                if (uri.rfind(kHostOrigin, 0) != 0) {
                    args->put_Cancel(TRUE);
                    if (!uri.empty()) {
                        ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                return S_OK;
            }).Get(),
        &token);

    g_web->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                args->get_WebMessageAsJson(&raw);
                std::string payload = raw ? WToU8(raw) : std::string();
                CoTaskMemFree(raw);
                if (!payload.empty() && g_bridge) {
                    std::string reply = g_bridge->Handle(payload);
                    if (!reply.empty()) g_web->PostWebMessageAsJson(U8ToW(reply).c_str());
                }
                return S_OK;
            }).Get(),
        &token);

    g_web->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                g_bridge->Tick();
                return S_OK;
            }).Get(),
        &token);

    g_web->Navigate(L"https://bitflow.local/index.html");
}

void InitWebView() {
    std::wstring dataDir = DataDirectory();
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection");

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    MessageBoxW(g_hwnd,
                                L"WebView2 初始化失败，请确认已安装 Microsoft Edge WebView2 Runtime。",
                                L"BitFlow", MB_ICONERROR);
                    return S_OK;
                }
                g_env = env;
                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result2) || !controller) return S_OK;
                            g_ctrl = controller;
                            controller->get_CoreWebView2(&g_web);
                            ConfigureWebView();
                            ResizeWebView();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        MessageBoxW(g_hwnd,
                    L"WebView2 环境创建失败，请确认已安装 Microsoft Edge WebView2 Runtime。",
                    L"BitFlow", MB_ICONERROR);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_TIMER:
        if (wp == 1 && g_bridge) g_bridge->Tick();
        return 0;
    case WM_SETTINGCHANGE: {
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            bool dark = SystemUsesDarkTheme();
            if (dark != g_dark) {
                g_dark = dark;
                ApplyWindowTheme(hwnd);
                ApplyWebViewTheme();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_bridge) g_bridge->Shutdown();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_web) g_web.Reset();
        if (g_ctrl) {
            g_ctrl->Close();
            g_ctrl.Reset();
        }
        g_env.Reset();
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_hinst = hinst;
    g_dark = SystemUsesDarkTheme();

    SettingsStore settings;
    settings.Load();

    xglog::Config logCfg;
    logCfg.dir = LogDir();
    logCfg.baseName = L"bitflow";
    logCfg.level = ParseLogLevelName(settings.Get().logLevel);
    logCfg.fileLevel = xglog::Level::Debug;
    logCfg.console = false;
    logCfg.debugger = true;
    logCfg.file = true;
    xglog::Init(logCfg);
    XGLOG_INFO("BitFlow starting, settings=%s", WToU8(SettingsPath()).c_str());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = CreateSolidBrush(SurfaceColor());
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    DWORD dpi = GetDpiForSystem();
    RECT frame{0, 0, MulDiv(1120, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = frame.right - frame.left;
    int h = frame.bottom - frame.top;

    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

    g_hwnd = CreateWindowExW(0, kWindowClass, L"BitFlow", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr,
                             nullptr, hinst, nullptr);
    if (!g_hwnd) return 1;

    ApplyWindowTheme(g_hwnd);

    g_bridge = std::make_unique<Bridge>(
        [](const std::string& json) {
            if (g_web) g_web->PostWebMessageAsJson(U8ToW(json).c_str());
        },
        []() { return g_dark; },
        &settings);
    g_bridge->Start();

    SetTimer(g_hwnd, 1, 200, nullptr);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);

    InitWebView();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_bridge) {
        g_bridge->Shutdown();
        g_bridge.reset();
    }
    xglog::Shutdown();
    CoUninitialize();
    return 0;
}
