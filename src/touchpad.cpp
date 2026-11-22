// タッチパッドの4辺に機能を割り当てる（ノートPC用）
//
// Windows の「高精度タッチパッド（Precision Touchpad）」が送る指の座標を Raw Input で読み、
// 辺に置いた指を辺に沿って動かしたときに、音量・明るさ・スクロールなどを操作する。
#include "pointerkit.h"
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <wbemidl.h>
#include <map>
#include <cmath>
#include <algorithm>

const wchar_t* kEdgeNames[4]  = { L"Left", L"Right", L"Top", L"Bottom" };
const wchar_t* kEdgeLabels[4] = { L"左の辺", L"右の辺", L"上の辺", L"下の辺" };
const wchar_t* kEdgeActNames[E_COUNT] = {
    L"none", L"volume", L"brightness", L"vscroll", L"hscroll", L"zoom", L"tab", L"seek", L"track",
};
const wchar_t* kEdgeActLabels[E_COUNT] = {
    L"使わない", L"音量", L"画面の明るさ", L"縦スクロール", L"横スクロール",
    L"拡大・縮小（Ctrl+ホイール）", L"タブの切り替え", L"早送り・巻き戻し（←→キー）", L"曲送り・曲戻し",
};

volatile bool g_tpSuppress = false;   // 辺を操作中 → マウスフックがカーソル移動を止める
volatile DWORD g_tpLastTick = 0;       // 最後にタッチパッドの報告が来た時刻（止めっぱなし防止）

static HWND g_tpWnd = nullptr;
static HWND volatile g_tpMonitor = nullptr;
static Config g_tc;                    // タッチパッド用スレッドの設定コピー
static int g_tpDevices = 0;

static const ULONG_PTR kMagicTp = 0x4F4E454DUL;   // hook.cpp と同じ目印

// ---------------------------------------------------------------------------
// デバイスごとの解析情報
// ---------------------------------------------------------------------------
struct Finger { USHORT link; LONG xmin, xmax, ymin, ymax; bool hasTip; };
struct TpDevice {
    std::vector<BYTE> pp;     // PHIDP_PREPARSED_DATA
    std::vector<Finger> fingers;
    bool hasCount = false;
    USHORT countLink = 0;
    bool ok = false;
};
static std::map<HANDLE, TpDevice> g_devs;

static TpDevice& GetDevice(HANDLE h) {
    auto it = g_devs.find(h);
    if (it != g_devs.end()) return it->second;
    TpDevice& d = g_devs[h];
    UINT sz = 0;
    GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &sz);
    if (!sz) return d;
    d.pp.resize(sz);
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d.pp.data(), &sz) == (UINT)-1) return d;
    PHIDP_PREPARSED_DATA pp = (PHIDP_PREPARSED_DATA)d.pp.data();
    HIDP_CAPS caps;
    if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) return d;

    std::vector<HIDP_VALUE_CAPS> vc(caps.NumberInputValueCaps);
    USHORT n = caps.NumberInputValueCaps;
    if (n) HidP_GetValueCaps(HidP_Input, vc.data(), &n, pp);
    std::map<USHORT, Finger> f;
    for (USHORT i = 0; i < n; i++) {
        const HIDP_VALUE_CAPS& v = vc[i];
        USHORT usage = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
        if (v.UsagePage == 0x01 && (usage == 0x30 || usage == 0x31)) {
            Finger& fi = f[v.LinkCollection];
            fi.link = v.LinkCollection;
            if (usage == 0x30) { fi.xmin = v.LogicalMin; fi.xmax = v.LogicalMax; }
            else { fi.ymin = v.LogicalMin; fi.ymax = v.LogicalMax; }
        }
        if (v.UsagePage == 0x0D && usage == 0x54) { d.hasCount = true; d.countLink = v.LinkCollection; }
    }
    std::vector<HIDP_BUTTON_CAPS> bc(caps.NumberInputButtonCaps);
    USHORT nb = caps.NumberInputButtonCaps;
    if (nb) HidP_GetButtonCaps(HidP_Input, bc.data(), &nb, pp);
    for (USHORT i = 0; i < nb; i++) {
        const HIDP_BUTTON_CAPS& b = bc[i];
        USHORT lo = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
        USHORT hi = b.IsRange ? b.Range.UsageMax : b.NotRange.Usage;
        if (b.UsagePage == 0x0D && lo <= 0x42 && 0x42 <= hi && f.count(b.LinkCollection)) f[b.LinkCollection].hasTip = true;
    }
    for (auto& kv : f)
        if (kv.second.hasTip && kv.second.xmax > kv.second.xmin && kv.second.ymax > kv.second.ymin)
            d.fingers.push_back(kv.second);
    d.ok = !d.fingers.empty();
    Log(L"タッチパッドを検出: 指の情報 %d 個（%ls）", (int)d.fingers.size(), d.ok ? L"使用可" : L"未対応の形式");
    return d;
}

// ---------------------------------------------------------------------------
// 入力の送信（このスレッドはフックの外なので、そのまま SendInput してよい）
// ---------------------------------------------------------------------------
static void Keys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> v;
    auto mk = [&](WORD vk, bool up) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        bool ext = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
                   (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2);
        in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
        in.ki.dwExtraInfo = kMagicTp;
        v.push_back(in);
    };
    for (WORD k : keys) mk(k, false);
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) mk(*it, true);
    SendInput((UINT)v.size(), v.data(), sizeof(INPUT));
}
static void Wheel(int delta, bool horiz, bool ctrl) {
    std::vector<INPUT> v;
    if (ctrl) { INPUT k = {}; k.type = INPUT_KEYBOARD; k.ki.wVk = VK_CONTROL; k.ki.dwExtraInfo = kMagicTp; v.push_back(k); }
    INPUT m = {};
    m.type = INPUT_MOUSE;
    m.mi.dwFlags = horiz ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    m.mi.mouseData = (DWORD)delta;
    m.mi.dwExtraInfo = kMagicTp;
    v.push_back(m);
    if (ctrl) { INPUT k = {}; k.type = INPUT_KEYBOARD; k.ki.wVk = VK_CONTROL; k.ki.dwFlags = KEYEVENTF_KEYUP; k.ki.dwExtraInfo = kMagicTp; v.push_back(k); }
    SendInput((UINT)v.size(), v.data(), sizeof(INPUT));
}

static void Osd(const std::wstring& t) {
    PostMessageW(g_hwnd, WM_TPOSD, 0, (LPARAM)new std::wstring(t));
}

// ---------------------------------------------------------------------------
// 画面の明るさ（ノートPCの内蔵ディスプレイ。WMI を使う）
// ---------------------------------------------------------------------------
static IWbemServices* g_wmi = nullptr;
static int g_bright = -1;

static bool WmiInit() {
    if (g_wmi) return true;
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc))) return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_wmi);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr)) { g_wmi = nullptr; return false; }
    CoSetProxyBlanket(g_wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

static IWbemClassObject* WmiFirst(const wchar_t* query) {
    IEnumWbemClassObject* en = nullptr;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    HRESULT hr = g_wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang); SysFreeString(q);
    if (FAILED(hr) || !en) return nullptr;
    IWbemClassObject* obj = nullptr; ULONG got = 0;
    en->Next(3000, 1, &obj, &got);
    en->Release();
    return got ? obj : nullptr;
}

static int GetBrightness() {
    if (!WmiInit()) return -1;
    IWbemClassObject* o = WmiFirst(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    if (!o) return -1;
    VARIANT v; VariantInit(&v);
    int r = -1;
    if (SUCCEEDED(o->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) {
        if (v.vt == VT_UI1) r = v.bVal; else if (v.vt == VT_I4) r = v.lVal;
    }
    VariantClear(&v);
    o->Release();
    return r;
}

static bool SetBrightness(int level) {
    if (!WmiInit()) return false;
    IWbemClassObject* inst = WmiFirst(L"SELECT * FROM WmiMonitorBrightnessMethods");
    if (!inst) return false;
    VARIANT path; VariantInit(&path);
    inst->Get(L"__PATH", 0, &path, nullptr, nullptr);
    inst->Release();
    IWbemClassObject* cls = nullptr; IWbemClassObject* inDef = nullptr; IWbemClassObject* in = nullptr;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
    BSTR method = SysAllocString(L"WmiSetBrightness");
    bool ok = false;
    if (SUCCEEDED(g_wmi->GetObject(clsName, 0, nullptr, &cls, nullptr)) &&
        SUCCEEDED(cls->GetMethod(method, 0, &inDef, nullptr)) && inDef &&
        SUCCEEDED(inDef->SpawnInstance(0, &in))) {
        VARIANT vb; VariantInit(&vb); vb.vt = VT_UI1; vb.bVal = (BYTE)level;
        VARIANT vt; VariantInit(&vt); vt.vt = VT_I4; vt.lVal = 0;
        in->Put(L"Brightness", 0, &vb, 0);
        in->Put(L"Timeout", 0, &vt, 0);
        ok = SUCCEEDED(g_wmi->ExecMethod(path.bstrVal, method, 0, nullptr, in, nullptr, nullptr));
    }
    if (in) in->Release();
    if (inDef) inDef->Release();
    if (cls) cls->Release();
    SysFreeString(clsName); SysFreeString(method);
    VariantClear(&path);
    return ok;
}

// ---------------------------------------------------------------------------
// 辺の操作
// ---------------------------------------------------------------------------
static int  g_edge = -1;          // 操作中の辺（-1=なし）
static double g_edgeAnchor = 0;   // 辺に沿った位置（0〜1）の基準
static int  g_prevFingers = 0;
static int  g_actLogged = 0;

static void Fire(EdgeAct a, int dir /* +1 = 上 or 右 */) {
    switch (a) {
    case E_VOLUME: Keys({ (WORD)(dir > 0 ? VK_VOLUME_UP : VK_VOLUME_DOWN) }); break;
    case E_BRIGHTNESS: {
        if (g_bright < 0) g_bright = GetBrightness();
        if (g_bright < 0) { Osd(L"このPCでは明るさを変更できません"); break; }
        g_bright = std::max(0, std::min(100, g_bright + dir * 5));
        SetBrightness(g_bright);
        Osd(L"明るさ  " + std::to_wstring(g_bright) + L"%");
        break;
    }
    case E_VSCROLL: Wheel(dir * WHEEL_DELTA, false, false); break;
    case E_HSCROLL: Wheel(dir * WHEEL_DELTA, true, false); break;
    case E_ZOOM: Wheel(dir * WHEEL_DELTA, false, true); break;
    case E_TAB: if (dir > 0) Keys({ VK_CONTROL, VK_TAB }); else Keys({ VK_CONTROL, VK_SHIFT, VK_TAB }); break;
    case E_SEEK: Keys({ (WORD)(dir > 0 ? VK_RIGHT : VK_LEFT) }); break;
    case E_TRACK: Keys({ (WORD)(dir > 0 ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PREV_TRACK) }); break;
    default: break;
    }
}

static void OnTouch(int fingers, double x, double y) {
    const Config& c = g_tc;
    g_tpLastTick = GetTickCount();
    if (HWND mon = g_tpMonitor)
        PostMessageW(mon, WM_TPMON, MAKEWPARAM((WORD)(x * 1000), (WORD)(y * 1000)), MAKELPARAM(fingers, (WORD)(g_edge + 1)));

    bool newTouch = g_prevFingers == 0 && fingers > 0;
    g_prevFingers = fingers;

    if (g_edge >= 0) {
        if (fingers != 1 || !c.enabled || !c.tpEnabled) {
            g_edge = -1;
            g_tpSuppress = false;
            return;
        }
        bool vertical = (g_edge == 0 || g_edge == 1);
        double pos = vertical ? -y : x;      // 上方向・右方向をプラスにする
        double step = std::max(0.01, c.tpStep / 100.0);
        EdgeAct a = c.tpAct[g_edge];
        if (a == E_VSCROLL || a == E_HSCROLL) step *= 0.5;   // スクロールは細かめに
        while (pos - g_edgeAnchor >= step) { Fire(a, +1); g_edgeAnchor += step; }
        while (g_edgeAnchor - pos >= step) { Fire(a, -1); g_edgeAnchor -= step; }
        return;
    }
    if (!newTouch || fingers != 1 || !c.enabled || !c.tpEnabled) return;

    // 指を置いた位置が辺の近くか判定（角は、より近い方の辺）
    double d[4] = { x, 1 - x, y, 1 - y };
    int best = -1; double bd = 1e9;
    for (int i = 0; i < 4; i++) {
        double e = std::max(0.02, c.tpEdgeWidth[i] / 100.0);
        double rel = d[i] / e;   // 幅に対してどれだけ内側か（小さいほど辺に近い）
        if (d[i] < e && c.tpAct[i] != E_NONE && rel < bd) { best = i; bd = rel; }
    }
    if (best < 0) return;
    g_edge = best;
    g_edgeAnchor = (best == 0 || best == 1) ? -y : x;
    g_bright = -1;
    g_tpSuppress = c.tpFreeze;
    if (g_actLogged < 20) { g_actLogged++; Log(L"タッチパッド %ls の操作を開始（%ls）", kEdgeLabels[best], kEdgeActLabels[c.tpAct[best]]); }
}

static void OnRawInput(HRAWINPUT hri) {
    UINT sz = 0;
    GetRawInputData(hri, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
    if (!sz) return;
    static std::vector<BYTE> buf;
    if (buf.size() < sz) buf.resize(sz);
    if (GetRawInputData(hri, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT* ri = (RAWINPUT*)buf.data();
    if (ri->header.dwType != RIM_TYPEHID) return;
    TpDevice& d = GetDevice(ri->header.hDevice);
    if (!d.ok) return;
    PHIDP_PREPARSED_DATA pp = (PHIDP_PREPARSED_DATA)d.pp.data();
    for (DWORD r = 0; r < ri->data.hid.dwCount; r++) {
        PCHAR rep = (PCHAR)ri->data.hid.bRawData + r * ri->data.hid.dwSizeHid;
        ULONG len = ri->data.hid.dwSizeHid;
        int count = 0;
        double fx = 0, fy = 0;
        bool have = false;
        for (const Finger& f : d.fingers) {
            USAGE usages[16]; ULONG nu = 16;
            bool tip = false;
            if (HidP_GetUsages(HidP_Input, 0x0D, f.link, usages, &nu, pp, rep, len) == HIDP_STATUS_SUCCESS)
                for (ULONG i = 0; i < nu; i++) if (usages[i] == 0x42) tip = true;
            if (!tip) continue;
            count++;
            if (have) continue;
            ULONG vx = 0, vy = 0;
            if (HidP_GetUsageValue(HidP_Input, 0x01, f.link, 0x30, &vx, pp, rep, len) != HIDP_STATUS_SUCCESS) continue;
            if (HidP_GetUsageValue(HidP_Input, 0x01, f.link, 0x31, &vy, pp, rep, len) != HIDP_STATUS_SUCCESS) continue;
            fx = (double)((LONG)vx - f.xmin) / (double)(f.xmax - f.xmin);
            fy = (double)((LONG)vy - f.ymin) / (double)(f.ymax - f.ymin);
            have = true;
        }
        // 指を離した瞬間のレポートでは「接触数」は 1 のまま、触れているフラグ（Tip）だけが消える。
        // そのため指の数は Tip が立っている指で数え、接触数は「続きのレポートか」の判定だけに使う。
        if (d.hasCount) {
            ULONG cnt = 0;
            if (HidP_GetUsageValue(HidP_Input, 0x0D, d.countLink, 0x54, &cnt, pp, rep, len) == HIDP_STATUS_SUCCESS && cnt == 0)
                continue;   // 指が多いときの2枚目以降のレポート（ハイブリッド方式）は無視
        }
        OnTouch(count, std::max(0.0, std::min(1.0, fx)), std::max(0.0, std::min(1.0, fy)));
    }
}

static LRESULT CALLBACK TpWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT:
        OnRawInput((HRAWINPUT)lp);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_INPUT_DEVICE_CHANGE:
        if (wp == GIDC_REMOVAL) g_devs.erase((HANDLE)lp);
        return 0;
    case WM_TPCONFIG: {
        Config* c = (Config*)lp;
        g_tc = *c;
        delete c;
        if (!g_tc.tpEnabled) { g_edge = -1; g_tpSuppress = false; }
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI TpThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TpWndProc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"PointerKitTouchpad";
    RegisterClassW(&wc);
    g_tpWnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinst, nullptr);

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x0D;   // Digitizer
    rid.usUsage = 0x05;       // Touch Pad
    rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid.hwndTarget = g_tpWnd;
    BOOL ok = RegisterRawInputDevices(&rid, 1, sizeof(rid));

    // タッチパッドがあるか数える
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (n) GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    for (UINT i = 0; i < n; i++) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info = { sizeof(info) };
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) != (UINT)-1 &&
            info.hid.usUsagePage == 0x0D && info.hid.usUsage == 0x05) g_tpDevices++;
    }
    Log(L"タッチパッドの監視: %ls、高精度タッチパッド %d 台", ok ? L"開始" : L"失敗", g_tpDevices);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    if (g_wmi) g_wmi->Release();
    CoUninitialize();
    return 0;
}

void StartTouchpad() {
    g_tc = g_cfg;
    CreateThread(nullptr, 0, TpThread, nullptr, 0, nullptr);
}
void SetTouchpadConfig(const Config& c) {
    if (g_tpWnd) PostMessageW(g_tpWnd, WM_TPCONFIG, 0, (LPARAM)new Config(c));
    else g_tc = c;
}
void SetTouchpadMonitor(HWND h) { g_tpMonitor = h; }
int TouchpadCount() { return g_tpDevices; }
