// PointerKit - ボタン割り当て + トラックボールでのスクロールを1本にまとめた常駐ソフト
#include "pointerkit.h"
#include <shellapi.h>
#include "resource.h"
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>

// ---------------------------------------------------------------------------
// 動作ログ（設定ファイルと同じフォルダの PointerKit.log）
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_logCs;
static DWORD g_mainThreadId = 0;
static const UINT WM_LOGLINE = WM_APP + 30;
static void WriteLogLine(const wchar_t* line);
static bool g_logInit = false;
std::wstring LogPath() {
    return g_iniPath.substr(0, g_iniPath.find_last_of(L"\\/") + 1) + L"PointerKit.log";
}
void Log(const wchar_t* fmt, ...) {
    if (!g_logInit || g_iniPath.empty()) return;
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    msg[1023] = 0;
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1200];
    swprintf(line, 1200, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %ls\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    // メインスレッド以外（マウスのフックなど）からは、書き込みをメインスレッドに任せて待たせない
    if (g_mainThreadId && GetCurrentThreadId() != g_mainThreadId && g_hwnd) {
        PostMessageW(g_hwnd, WM_LOGLINE, 0, (LPARAM)new std::wstring(line));
        return;
    }
    WriteLogLine(line);
}
static void WriteLogLine(const wchar_t* line) {
    EnterCriticalSection(&g_logCs);
    std::wstring path = LogPath();
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) && d.nFileSizeLow > 512 * 1024)
        DeleteFileW(path.c_str());   // 大きくなりすぎたら作り直す
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, &u[0], n, nullptr, nullptr);
        DWORD w; WriteFile(h, u.data(), (DWORD)(n - 1), &w, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------------------
// ジェスチャーの表示（カーソルの近くに動きと動作名を出す）
// ---------------------------------------------------------------------------
static HWND g_osd = nullptr;
static std::wstring g_osdText;
static HFONT g_osdFont = nullptr;

static LRESULT CALLBACK OsdProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(40, 44, 52));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(dc, g_osdFont);
        DrawTextW(dc, g_osdText.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowOsd(const std::wstring& text) {
    if (text.empty()) { if (g_osd) ShowWindow(g_osd, SW_HIDE); return; }
    if (!g_osd) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = OsdProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = L"PointerKitOsd";
        RegisterClassW(&wc);
        g_osd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_hinst, nullptr);
        SetLayeredWindowAttributes(g_osd, 0, 225, LWA_ALPHA);
    }
    POINT p; GetCursorPos(&p);
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    UINT dpi = 96;
    typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor f = (PFN_GetDpiForMonitor)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (f) { UINT dy; f(mon, 0, &dpi, &dy); }
    static UINT fontDpi = 0;
    if (!g_osdFont || fontDpi != dpi) {
        if (g_osdFont) DeleteObject(g_osdFont);
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -MulDiv(14, (int)dpi, 72);
        lf.lfWeight = FW_BOLD;
        g_osdFont = CreateFontIndirectW(&lf);
        fontDpi = dpi;
    }
    g_osdText = text;
    HDC dc = GetDC(g_osd);
    HGDIOBJ old = SelectObject(dc, g_osdFont);
    SIZE sz; GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(g_osd, dc);
    int pad = MulDiv(14, (int)dpi, 96);
    int w = sz.cx + pad * 2, h = sz.cy + pad;
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    int x = p.x + MulDiv(24, (int)dpi, 96), y = p.y + MulDiv(24, (int)dpi, 96);
    if (x + w > mi.rcWork.right) x = p.x - w - 8;
    if (y + h > mi.rcWork.bottom) y = p.y - h - 8;
    SetWindowPos(g_osd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_osd, nullptr, TRUE);
}

// 同じ働きをするソフトが動いていないか調べる
static std::wstring FindConflicts() {
    static const wchar_t* names[][2] = {
        { L"xmousebuttoncontrol.exe", L"X-Mouse Button Control" },
        { L"w10wheel.exe", L"W10Wheel" },
        { L"w10wheel.net.exe", L"W10Wheel" },
        { L"logioptionsplus_agent.exe", L"" },  // ログ用（競合ではない）
    };
    std::wstring found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring n = pe.szExeFile;
        for (auto& c : n) c = (wchar_t)towlower(c);
        for (auto& e : names) {
            if (!e[1][0] || n != e[0]) continue;
            if (found.find(e[1]) == std::wstring::npos) found += std::wstring(found.empty() ? L"" : L"、") + e[1];
        }
        if (n == L"logioptionsplus_agent.exe" || n == L"logioptions.exe" || n == L"setpoint.exe" ||
            n == L"kensingtonworks.exe" || n == L"trackballworks.exe")
            Log(L"メーカー製マウスソフトを検出: %ls", pe.szExeFile);
    }
    CloseHandle(snap);
    if (FindWindowW(nullptr, L"W10Wheel") && found.find(L"W10Wheel") == std::wstring::npos)
        found += std::wstring(found.empty() ? L"" : L"、") + L"W10Wheel";
    return found;
}

HWND g_hwnd = nullptr;
HINSTANCE g_hinst = nullptr;
static NOTIFYICONDATAW g_nid = {};
static UINT WM_TASKBARCREATED = 0;
static const wchar_t* kAppName = L"PointerKit";

// ---------------------------------------------------------------------------
// スタートアップ登録（HKCU\...\Run）
// ---------------------------------------------------------------------------
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static std::wstring ExePath() {
    wchar_t p[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    return p;
}
bool IsStartup() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2]; DWORD sz = sizeof(buf), type = 0;
    bool ok = RegQueryValueExW(k, kAppName, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}
void SetStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// トレイアイコン
// ---------------------------------------------------------------------------
static void UpdateTray(bool add) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(g_cfg.enabled ? IDI_APP : IDI_OFF),
                                    IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(g_nid.szTip, g_cfg.enabled ? L"PointerKit（有効）" : L"PointerKit（無効）");
    if (!Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid) && !add) Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void Balloon(const wchar_t* text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, L"PointerKit");
    wcscpy_s(n.szInfo, text);
    n.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"設定...");
    SetMenuDefaultItem(m, ID_SETTINGS, FALSE);
    AppendMenuW(m, MF_STRING | (g_cfg.enabled ? MF_CHECKED : 0), ID_ENABLE, L"有効");
    AppendMenuW(m, MF_STRING | (UsActive() ? MF_CHECKED : 0), ID_USTOGGLE, L"US配列キーボードの変換");
    AppendMenuW(m, MF_STRING | (IsStartup() ? MF_CHECKED : 0), ID_STARTUP, L"Windows 起動時に自動で起動");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_LOG, L"動作ログを開く");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"終了");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------------------
// 設定の反映
// ---------------------------------------------------------------------------
void ApplyNewConfig(const Config& c) {
    g_cfg = c;
    SetHookConfig(c);
    SetTouchpadConfig(c);
    SaveConfig(c);
    UpdateTray(false);
    RegisterUsHotkey();
    UpdateUsState();
}

void ToggleEnabled() {
    Config c = g_cfg;
    c.enabled = !c.enabled;
    ApplyNewConfig(c);
    Balloon(c.enabled ? L"有効にしました" : L"無効にしました（ボタン・スクロールは通常の動作になります）");
}

// フックが Windows に外されていないか見張り、外れていたら付け直す
static POINT g_wdPos = {};
static void Watchdog() {
    unsigned calls, slow; double maxMs;
    HookStats(&calls, &slow, &maxMs);
    if (slow) Log(L"処理が遅かった回数: %u / %u 回（最大 %.1f ms）", slow, calls, maxMs);
    POINT p; GetCursorPos(&p);
    bool moved = p.x != g_wdPos.x || p.y != g_wdPos.y;
    g_wdPos = p;
    DWORD idle = GetTickCount() - HookLastTick();
    static DWORD s_lastFix = 0;
    if (moved && idle > 3000 && GetTickCount() - s_lastFix > 10000) {
        s_lastFix = GetTickCount();
        Log(L"マウスは動いているのにフックが %lu ms 呼ばれていない → 付け直します", idle);
        InstallHook();
    }
}

static void ReloadIfChanged() {
    FILETIME t = GetIniTime();
    if (CompareFileTime(&t, &g_iniTime) == 0) return;
    LoadConfig();
    SetHookConfig(g_cfg);
    SetTouchpadConfig(g_cfg);
    UpdateTray(false);
    RegisterUsHotkey();
    UpdateUsState();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED && WM_TASKBARCREATED) { UpdateTray(true); return 0; }
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowMenu();
        else if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) ShowSettings();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ENABLE: ToggleEnabled(); break;
        case ID_SETTINGS: ShowSettings(); break;
        case ID_STARTUP: SetStartup(!IsStartup()); break;
        case ID_USTOGGLE: ToggleUs(); break;
        case ID_EXIT: Log(L"終了"); DestroyWindow(h); break;
        case ID_LOG: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + LogPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_CONFIGWATCH) ReloadIfChanged();
        else if (wp == TIMER_WATCHDOG) Watchdog();
        else if (wp == TIMER_USCHECK) UpdateUsState();
        else if (wp == TIMER_USDEVCHANGE) { KillTimer(h, TIMER_USDEVCHANGE); UpdateUsState(); }
        else if (wp == TIMER_OSDHIDE) { KillTimer(h, TIMER_OSDHIDE); ShowOsd(L""); }
        return 0;
    case WM_TPOSD: {
        std::wstring* t = (std::wstring*)lp;
        ShowOsd(*t);
        delete t;
        SetTimer(h, TIMER_OSDHIDE, 1200, nullptr);
        return 0;
    }
    case WM_GESTUREOSD: {
        std::wstring* t = (std::wstring*)lp;
        ShowOsd(*t);
        delete t;
        return 0;
    }
    case WM_LOGLINE: {
        std::wstring* l = (std::wstring*)lp;
        WriteLogLine(l->c_str());
        delete l;
        return 0;
    }
    case WM_HOTKEY:
        if (wp == 1) ToggleUs();
        return 0;
    case WM_DEVICECHANGE:
        SetTimer(h, TIMER_USDEVCHANGE, 700, nullptr);   // キーボードのつなぎ外し（少し待ってから調べる）
        return TRUE;
    case WM_SETSCROLLCURSOR:
        ApplyCursor((int)wp);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) InstallHook();
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == 0x8 /*WTS_SESSION_UNLOCK*/) InstallHook();
        return 0;
    case WM_ENDSESSION:
        if (wp) ApplyCursor(0);
        return 0;
    case WM_DESTROY:
        ApplyCursor(0);
        RemoveHook();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

typedef BOOL (WINAPI *PFN_WTSReg)(HWND, DWORD);
typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
extern bool IsSettingsDialogMessage(MSG* m);

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmd, int) {
    g_hinst = hInst;
    InitializeCriticalSection(&g_logCs);
    g_mainThreadId = GetCurrentThreadId();
    HANDLE mtx = CreateMutexW(nullptr, FALSE, L"Local\\PointerKit_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // すでに起動している PointerKit（古い版や別フォルダのものを含む）を終了させて入れ替わる
        HWND other = FindWindowW(L"PointerKitWnd", nullptr);
        if (other) PostMessageW(other, WM_COMMAND, ID_EXIT, 0);
        DWORD r = WaitForSingleObject(mtx, 5000);
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
            MessageBoxW(nullptr, L"起動中の PointerKit を終了できませんでした。タスクマネージャーで PointerKit を終了してから、もう一度起動してください。",
                        kAppName, MB_OK | MB_ICONWARNING);
            return 0;
        }
    } else {
        WaitForSingleObject(mtx, 0);
    }

    // 旧名 OneMouse が動いていたら終了させる（同時に動くと二重に処理されるため）
    if (HWND old = FindWindowW(L"OneMouseWnd", nullptr)) {
        PostMessageW(old, WM_COMMAND, ID_EXIT, 0);
        for (int i = 0; i < 50 && IsWindow(old); i++) Sleep(100);
    }
    // 旧名 OneMouse の自動起動登録を PointerKit に置き換える
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ | KEY_WRITE, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExW(k, L"OneMouse", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                RegDeleteValueW(k, L"OneMouse");
                RegCloseKey(k);
                SetStartup(true);
            } else RegCloseKey(k);
        }
    }

    // マルチモニター・高DPIでも座標がずれないように
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (auto f = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext")) f((HANDLE)-4);
    else SetProcessDPIAware();

    InitConfigPath();
    bool firstRun = GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    LoadConfig();
    g_logInit = true;
    Log(L"---- PointerKit %ls 起動 ----", POINTERKIT_VERSION);
    Log(L"exe: %ls", ExePath().c_str());
    Log(L"設定: %ls（スクロール方法=%ls、有効=%d）", g_iniPath.c_str(), kTrigNames[g_cfg.trigger], g_cfg.enabled ? 1 : 0);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PointerKitWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    if (HMODULE wts = LoadLibraryW(L"wtsapi32.dll"))
        if (auto f = (PFN_WTSReg)GetProcAddress(wts, "WTSRegisterSessionNotification")) f(g_hwnd, 0);

    UpdateTray(true);
    if (!InstallHook()) {
        MessageBoxW(nullptr, L"マウスの監視を開始できませんでした。", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
    SetTimer(g_hwnd, TIMER_CONFIGWATCH, 2000, nullptr);
    GetCursorPos(&g_wdPos);
    SetTimer(g_hwnd, TIMER_WATCHDOG, 1000, nullptr);
    SetTimer(g_hwnd, TIMER_USCHECK, 3000, nullptr);
    RegisterUsHotkey();
    UpdateUsState();
    StartTouchpad();

    std::wstring conflicts = FindConflicts();
    if (!conflicts.empty()) {
        Log(L"同じ働きのソフトが起動中: %ls", conflicts.c_str());
        std::wstring msg = conflicts + L" が起動しています。\n\nPointerKit と同時に使うと、スクロールやボタンが正しく動かなかったり、"
                           L"マウスの動きがカクついたりします。\nタスクトレイから終了してください"
                           L"（Windows 起動時の自動起動もオフにしておくと安心です）。";
        MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }

    if (firstRun) {
        Balloon(L"起動しました。トレイのアイコンをクリックすると設定を開けます。");
        ShowSettings();
    } else if (cmd && wcsstr(cmd, L"/settings")) ShowSettings();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsSettingsDialogMessage(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ApplyCursor(0);
    if (mtx) CloseHandle(mtx);
    return 0;
}
