// US配列キーボード → 日本語配列（Windows の設定）での入力変換
#include "pointerkit.h"
#include <cwctype>
#include <algorithm>
extern "C" {
#include <hidsdi.h>
}

volatile bool g_usConvert = false;   // フック用スレッドが参照する「いま変換するか」
static int g_usManual = -1;          // 手動で切り替えた状態（-1 = 自動のまま）
static bool g_usLastAuto = false;
static bool g_usFirst = true;

// ---------------------------------------------------------------------------
// 変換表：US配列で押したキー（スキャンコード）→ 日本語配列で同じ文字になるキー
// ---------------------------------------------------------------------------
struct KeyOut { WORD vk; bool shift; };
struct KeyMap { DWORD sc; KeyOut normal, shifted; };
static const KeyMap kUsMap[] = {
    { 0x03, { '2', false },          { VK_OEM_3, false } },      // 2  @
    { 0x07, { '6', false },          { VK_OEM_7, false } },      // 6  ^
    { 0x08, { '7', false },          { '6', true } },            // 7  &
    { 0x09, { '8', false },          { VK_OEM_1, true } },       // 8  *
    { 0x0A, { '9', false },          { '8', true } },            // 9  (
    { 0x0B, { '0', false },          { '9', true } },            // 0  )
    { 0x0C, { VK_OEM_MINUS, false }, { VK_OEM_102, true } },     // -  _
    { 0x0D, { VK_OEM_MINUS, true },  { VK_OEM_PLUS, true } },    // =  +
    { 0x1A, { VK_OEM_4, false },     { VK_OEM_4, true } },       // [  {
    { 0x1B, { VK_OEM_6, false },     { VK_OEM_6, true } },       // ]  }
    { 0x2B, { VK_OEM_5, false },     { VK_OEM_5, true } },       // \  |
    { 0x27, { VK_OEM_PLUS, false },  { VK_OEM_1, false } },      // ;  :
    { 0x28, { '7', true },           { '2', true } },            // '  "
    { 0x29, { VK_OEM_3, true },      { VK_OEM_7, true } },       // `  ~
};
static const KeyMap* FindMap(DWORD sc) {
    for (auto& m : kUsMap) if (m.sc == sc) return &m;
    return nullptr;
}

static void AddKey(std::vector<INPUT>& v, WORD vk, bool up) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    in.ki.dwExtraInfo = kInputMagic;
    v.push_back(in);
}
static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool g_usCaps = true;   // CapsLock を US 配列と同じ働き（押すだけで大文字固定）にする

LRESULT UsKeyboardProc(HHOOK hook, int n, WPARAM wp, LPARAM lp) {
    if (n != HC_ACTION || !g_usConvert) return CallNextHookEx(hook, n, wp, lp);
    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
    if (k->dwExtraInfo == kInputMagic || (k->flags & LLKHF_INJECTED) || (k->flags & LLKHF_EXTENDED))
        return CallNextHookEx(hook, n, wp, lp);
    bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    bool lsh = Down(VK_LSHIFT), rsh = Down(VK_RSHIFT);

    // CapsLock（日本語配列では「英数」になる）→ Shift+CapsLock で大文字固定を切り替え
    if (k->scanCode == 0x3A && g_usCaps) {
        if (!Down(VK_CONTROL) && !Down(VK_MENU)) {
            if (down) {
                std::vector<INPUT> v;
                if (!lsh && !rsh) AddKey(v, VK_LSHIFT, false);
                AddKey(v, VK_CAPITAL, false);
                AddKey(v, VK_CAPITAL, true);
                if (!lsh && !rsh) AddKey(v, VK_LSHIFT, true);
                QueueInputExt(v.data(), v.size());
            }
            return 1;
        }
        return CallNextHookEx(hook, n, wp, lp);
    }
    // Ctrl・Alt・Win と一緒に押したキーはそのまま（ショートカットや Alt+` の日本語入力切り替えを壊さない）
    if (Down(VK_CONTROL) || Down(VK_MENU) || Down(VK_LWIN) || Down(VK_RWIN)) return CallNextHookEx(hook, n, wp, lp);
    const KeyMap* m = FindMap(k->scanCode);
    if (!m) return CallNextHookEx(hook, n, wp, lp);
    if (!down) return 1;                       // 押したときに「押す→離す」を送り済み
    bool sh = lsh || rsh;
    const KeyOut& o = sh ? m->shifted : m->normal;
    std::vector<INPUT> v;
    if (o.shift && !sh) AddKey(v, VK_LSHIFT, false);
    if (!o.shift && sh) { if (lsh) AddKey(v, VK_LSHIFT, true); if (rsh) AddKey(v, VK_RSHIFT, true); }
    AddKey(v, o.vk, false);
    AddKey(v, o.vk, true);
    if (o.shift && !sh) AddKey(v, VK_LSHIFT, true);
    if (!o.shift && sh) { if (lsh) AddKey(v, VK_LSHIFT, false); if (rsh) AddKey(v, VK_RSHIFT, false); }
    QueueInputExt(v.data(), v.size());
    return 1;
}

// ---------------------------------------------------------------------------
// つながっているキーボードの一覧
// ---------------------------------------------------------------------------
static std::wstring KbId(const std::wstring& name0) {
    std::wstring name = name0;
    for (auto& c : name) c = (wchar_t)towupper(c);
    if (name.find(L"RDP_KBD") != std::wstring::npos || name.find(L"RDP_MOU") != std::wstring::npos) return L"";      // リモートデスクトップの仮想キーボード
    size_t v = name.find(L"VID");
    if (v != std::wstring::npos) {
        size_t p = name.find(L"PID", v);
        if (p != std::wstring::npos && p - v < 16) {
            size_t e = p + 4;
            while (e < name.size() && iswxdigit(name[e])) e++;
            return name.substr(v, e - v);
        }
    }
    // USB 以外（ノートPC本体の PS/2・I2C など）：ハードウェアIDの部分
    size_t a = name.find(L'#');
    size_t b = a == std::wstring::npos ? a : name.find(L'#', a + 1);
    if (a != std::wstring::npos && b != std::wstring::npos) return name.substr(a + 1, b - a - 1);
    return name;
}

static std::vector<KbInfo> ListDevices(DWORD type, bool names);
std::vector<KbInfo> ListKeyboards(bool names) { return ListDevices(RIM_TYPEKEYBOARD, names); }
std::vector<KbInfo> ListMice(bool names) { return ListDevices(RIM_TYPEMOUSE, names); }

// 生の入力のデバイスハンドル → ID（つなぎ直すとハンドルが変わるので、見つからなければ調べ直す）
std::wstring DeviceIdFromHandle(HANDLE h) {
    static std::vector<std::pair<HANDLE, std::wstring>> cache;
    for (auto& c : cache) if (c.first == h) return c.second;
    wchar_t nm[512]; UINT sz = 512;
    std::wstring id;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nm, &sz) != (UINT)-1) id = KbId(nm);
    if (cache.size() > 64) cache.clear();
    cache.push_back({ h, id });
    return id;
}

static std::vector<KbInfo> ListDevices(DWORD type, bool names) {
    std::vector<KbInfo> r;
    UINT n = 0;
    if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) return r;
    std::vector<RAWINPUTDEVICELIST> list(n);
    n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    if (n == (UINT)-1) return r;
    for (UINT i = 0; i < n; i++) {
        if (list[i].dwType != type) continue;
        wchar_t nm[512]; UINT sz = 512;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nm, &sz) == (UINT)-1) continue;
        std::wstring name = nm;
        std::wstring id = KbId(name);
        if (id.empty()) continue;
        bool dup = false;
        for (auto& x : r) if (x.id == id) dup = true;
        if (dup) continue;
        KbInfo k;
        k.id = id;
        std::wstring up = name;
        for (auto& c : up) c = (wchar_t)towupper(c);
        if (!names) {
        } else if (up.find(L"VID") == std::wstring::npos) {
            k.name = type == RIM_TYPEKEYBOARD ? L"ノートPC本体のキーボード" : L"ノートPC本体のタッチパッド・ポインター";
        } else {
            HANDLE h = CreateFileW(name.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            wchar_t prod[128] = {};
            if (h != INVALID_HANDLE_VALUE) {
                if (!HidD_GetProductString(h, prod, sizeof(prod))) prod[0] = 0;
                CloseHandle(h);
            }
            k.name = prod[0] ? std::wstring(prod) : std::wstring(type == RIM_TYPEKEYBOARD ? L"外付けキーボード" : L"マウス");
            k.name += L"（" + id + L"）";
        }
        r.push_back(k);
    }
    return r;
}

// ---------------------------------------------------------------------------
// 自動・手動の切り替え（メインスレッド）
// ---------------------------------------------------------------------------
bool UsActive() { return g_usConvert; }

static bool AutoWanted() {
    if (g_cfg.usMode == 2) return true;
    if (g_cfg.usMode != 1) return false;
    for (auto& k : ListKeyboards(false))
        for (auto& id : g_cfg.usKeyboards) if (k.id == id) return true;
    return false;
}

static void ShowUsOsd(const wchar_t* why) {
    std::wstring t = std::wstring(L"US配列の変換：") + (g_usConvert ? L"オン" : L"オフ") + why;
    PostMessageW(g_hwnd, WM_TPOSD, 0, (LPARAM)new std::wstring(t));
}

void UpdateUsState() {
    g_usCaps = g_cfg.usCaps;
    bool want = AutoWanted();
    bool changed = want != g_usLastAuto;
    g_usLastAuto = want;
    if (changed) g_usManual = -1;          // つなぐ・外すをしたら手動の切り替えは解除
    bool before = g_usConvert;
    g_usConvert = g_usManual >= 0 ? g_usManual != 0 : want;
    if (g_usConvert != before) {
        Log(L"US配列の変換を %ls（%ls）", g_usConvert ? L"オン" : L"オフ", changed ? L"キーボードの接続" : L"設定");
        if (!g_usFirst) ShowUsOsd(changed && g_cfg.usMode == 1 ? (g_usConvert ? L"（US配列キーボードを検出）" : L"（US配列キーボードが外れました）") : L"");
    }
    g_usFirst = false;
}

void ToggleUs() {
    g_usManual = g_usConvert ? 0 : 1;
    g_usConvert = g_usManual != 0;
    Log(L"US配列の変換を手動で %ls", g_usConvert ? L"オン" : L"オフ");
    ShowUsOsd(L"");
}

// 切り替えのショートカットキー
void RegisterUsHotkey() {
    UnregisterHotKey(g_hwnd, 1);
    if (g_cfg.usHotkey.empty()) return;
    UINT mods = MOD_NOREPEAT, vk = 0;
    for (WORD k : ParseKeys(g_cfg.usHotkey)) {
        if (k == VK_CONTROL) mods |= MOD_CONTROL;
        else if (k == VK_SHIFT) mods |= MOD_SHIFT;
        else if (k == VK_MENU) mods |= MOD_ALT;
        else if (k == VK_LWIN) mods |= MOD_WIN;
        else vk = k;
    }
    if (!vk) return;
    if (!RegisterHotKey(g_hwnd, 1, mods, vk)) Log(L"ショートカット %ls はほかのソフトが使っているため登録できませんでした", g_cfg.usHotkey.c_str());
}
