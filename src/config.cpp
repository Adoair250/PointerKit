// 設定の読み書き（%APPDATA%\PointerKit\PointerKit.ini）
#include "pointerkit.h"
#include <shlobj.h>
#include <map>
#include <cwctype>
#include <algorithm>

const wchar_t* kBtnNames[B_COUNT]  = { L"Left", L"Right", L"Middle", L"X1", L"X2" };
const wchar_t* kBtnLabels[B_COUNT] = { L"左ボタン", L"右ボタン", L"中ボタン（ホイール押し）",
                                       L"戻るボタン（ボタン4）", L"進むボタン（ボタン5）" };

const wchar_t* kActNames[A_COUNT] = {
    L"pass", L"disable", L"keys",
    L"click:Left", L"click:Right", L"click:Middle", L"back", L"forward",
    L"minimize", L"maximize", L"close",
    L"wheelup", L"wheeldown", L"wheelleft", L"wheelright",
    L"toggle",
};
const wchar_t* kActLabels[A_COUNT] = {
    L"そのまま（変更しない）", L"無効にする", L"キー入力（ショートカット）",
    L"左クリック", L"右クリック", L"中クリック", L"戻る", L"進む",
    L"ウィンドウを最小化", L"ウィンドウを最大化／元に戻す", L"ウィンドウを閉じる",
    L"上へスクロール", L"下へスクロール", L"左へスクロール", L"右へスクロール",
    L"PointerKit の有効／無効を切り替え",
};

const wchar_t* kTrigNames[T_COUNT]  = { L"None", L"Right", L"Middle", L"X1", L"X2", L"Left", L"LR" };
const wchar_t* kTrigLabels[T_COUNT] = {
    L"使わない",
    L"右ボタンを押しながらボールを動かす",
    L"中ボタンを押しながらボールを動かす",
    L"戻るボタンを押しながらボールを動かす",
    L"進むボタンを押しながらボールを動かす",
    L"左ボタンを押しながらボールを動かす",
    L"左右ボタンを同時に押してからボールを動かす",
};
const wchar_t* kAccelLabels[kAccelCount] = {
    L"なし（一定の速さ）", L"弱（W10Wheel の M5）", L"やや弱（M6）", L"中（M7）", L"やや強（M8）", L"強（M9）",
};

const wchar_t* kGesturePatterns[] = {
    L"L", L"R", L"U", L"D",
    L"DR", L"DL", L"UR", L"UL", L"RD", L"RU", L"LD", L"LU",
    L"UD", L"DU", L"LR", L"RL",
};
const int kGesturePatternCount = sizeof(kGesturePatterns) / sizeof(kGesturePatterns[0]);

std::wstring GestureArrows(const std::wstring& p) {
    std::wstring r;
    for (wchar_t c : p) {
        switch (c) {
        case L'U': r += L"↑"; break;
        case L'D': r += L"↓"; break;
        case L'L': r += L"←"; break;
        case L'R': r += L"→"; break;
        }
    }
    return r;
}
std::wstring ActionDescription(const Action& a) {
    if (a.type == A_KEYS) return a.keys + L" キー";
    return kActLabels[a.type];
}

// 「ジェスチャー」を初めて使うときのおすすめ設定
static std::vector<GestureDef> DefaultGestures() {
    auto g = [](const wchar_t* p, ActType t, const wchar_t* keys = L"") {
        GestureDef d; d.pattern = p; d.action.type = t; d.action.keys = keys; return d;
    };
    return {
        g(L"L", A_BACK), g(L"R", A_FORWARD),
        g(L"U", A_MAXIMIZE), g(L"D", A_MINIMIZE),
        g(L"DR", A_KEYS, L"Ctrl+W"), g(L"UD", A_KEYS, L"F5"),
        g(L"RL", A_KEYS, L"Ctrl+Shift+T"), g(L"UL", A_KEYS, L"Ctrl+Shift+Tab"),
    };
}

const wchar_t* kCornerNames[4] = { L"TopLeft", L"TopRight", L"BottomLeft", L"BottomRight" };
const wchar_t* kCornerLabels[4] = { L"左上", L"右上", L"左下", L"右下" };

Config g_cfg;
std::wstring g_iniPath;
FILETIME g_iniTime = {};

// ---------------------------------------------------------------------------
static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}
static std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

Btn ParseBtn(const std::wstring& s0) {
    std::wstring s = Upper(Trim(s0));
    for (int i = 0; i < B_COUNT; i++) if (s == Upper(kBtnNames[i])) return (Btn)i;
    if (s == L"BACK") return B_X1;
    if (s == L"FORWARD") return B_X2;
    return B_NONE;
}

// キー名 ⇔ 仮想キーコード
static const std::vector<std::pair<const wchar_t*, WORD>>& KeyTable() {
    static const std::vector<std::pair<const wchar_t*, WORD>> t = {
        {L"Ctrl", VK_CONTROL}, {L"Shift", VK_SHIFT}, {L"Alt", VK_MENU}, {L"Win", VK_LWIN},
        {L"Tab", VK_TAB}, {L"Enter", VK_RETURN}, {L"Esc", VK_ESCAPE}, {L"Space", VK_SPACE},
        {L"Backspace", VK_BACK}, {L"Delete", VK_DELETE}, {L"Insert", VK_INSERT},
        {L"Home", VK_HOME}, {L"End", VK_END}, {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT},
        {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT},
        {L"PrintScreen", VK_SNAPSHOT}, {L"Pause", VK_PAUSE}, {L"Apps", VK_APPS},
        {L"BrowserBack", VK_BROWSER_BACK}, {L"BrowserForward", VK_BROWSER_FORWARD},
        {L"BrowserRefresh", VK_BROWSER_REFRESH}, {L"Mute", VK_VOLUME_MUTE},
        {L"VolumeUp", VK_VOLUME_UP}, {L"VolumeDown", VK_VOLUME_DOWN},
        {L"PlayPause", VK_MEDIA_PLAY_PAUSE}, {L"NextTrack", VK_MEDIA_NEXT_TRACK},
        {L"PrevTrack", VK_MEDIA_PREV_TRACK},
        {L"Minus", VK_OEM_MINUS}, {L"Plus", VK_OEM_PLUS}, {L"Comma", VK_OEM_COMMA},
        {L"Period", VK_OEM_PERIOD}, {L"Num+", VK_ADD}, {L"Num-", VK_SUBTRACT},
        {L"Henkan", VK_CONVERT}, {L"Muhenkan", VK_NONCONVERT},
    };
    return t;
}

std::wstring VkName(WORD vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
    if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
    if (vk == VK_RWIN) vk = VK_LWIN;
    for (auto& p : KeyTable()) if (p.second == vk) return p.first;
    wchar_t buf[16]; swprintf(buf, 16, L"VK%02X", vk);
    return buf;
}

static WORD ParseKey(const std::wstring& k) {
    std::wstring u = Upper(Trim(k));
    if (u.empty()) return 0;
    for (auto& p : KeyTable()) if (u == Upper(p.first)) return p.second;
    if (u == L"CONTROL") return VK_CONTROL;
    if (u == L"ESCAPE") return VK_ESCAPE;
    if (u == L"DEL") return VK_DELETE;
    if (u == L"PGUP") return VK_PRIOR;
    if (u == L"PGDN") return VK_NEXT;
    if (u.size() == 1 && ((u[0] >= L'A' && u[0] <= L'Z') || (u[0] >= L'0' && u[0] <= L'9'))) return (WORD)u[0];
    if (u.size() >= 2 && u[0] == L'F' && iswdigit(u[1])) {
        int n = _wtoi(u.c_str() + 1);
        if (n >= 1 && n <= 24) return (WORD)(VK_F1 + n - 1);
    }
    if (u.size() == 4 && u.rfind(L"NUM", 0) == 0 && iswdigit(u[3])) return (WORD)(VK_NUMPAD0 + (u[3] - L'0'));
    if (u.size() == 4 && u.rfind(L"VK", 0) == 0) return (WORD)wcstol(u.c_str() + 2, nullptr, 16);
    return 0;
}

std::vector<WORD> ParseKeys(const std::wstring& s) {
    std::vector<WORD> r;
    std::wstring cur;
    for (size_t i = 0; i <= s.size(); i++) {
        // "Num+" のような + を含むキー名に対応するため、直前が "Num" の + は区切りにしない
        if (i == s.size() || (s[i] == L'+' && Upper(Trim(cur)) != L"NUM")) {
            WORD vk = ParseKey(cur);
            if (vk) r.push_back(vk);
            cur.clear();
        } else cur += s[i];
    }
    return r;
}

static Action ParseAction(const std::wstring& s0) {
    Action a;
    std::wstring s = Trim(s0);
    std::wstring u = Upper(s);
    if (u.rfind(L"KEYS:", 0) == 0) {
        a.keys = Trim(s.substr(5));
        a.type = ParseKeys(a.keys).empty() ? A_PASS : A_KEYS;
        return a;
    }
    for (int i = 0; i < A_COUNT; i++)
        if (i != A_KEYS && u == Upper(kActNames[i])) { a.type = (ActType)i; return a; }
    if (u == L"CLICK:X1") a.type = A_BACK;
    else if (u == L"CLICK:X2") a.type = A_FORWARD;
    return a;
}
static std::wstring ActionToString(const Action& a) {
    if (a.type == A_KEYS) return L"keys:" + a.keys;
    return kActNames[a.type];
}

// ---------------------------------------------------------------------------
void InitConfigPath() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\PointerKit";
        CoTaskMemFree(appdata);
    }
    // exe と同じ場所に PointerKit.ini があればそちらを優先（ポータブル運用）
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring e = exe;
    std::wstring exeDir = e.substr(0, e.find_last_of(L"\\/") + 1);
    std::wstring local = exeDir + L"PointerKit.ini";
    // 旧名 OneMouse の設定があれば引き継ぐ（同じフォルダの OneMouse.ini → PointerKit.ini）
    std::wstring oldLocal = exeDir + L"OneMouse.ini";
    if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(oldLocal.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(oldLocal.c_str(), local.c_str(), TRUE);
    if (!dir.empty() && GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring oldApp = dir.substr(0, dir.find_last_of(L'\\') + 1) + L"OneMouse\\OneMouse.ini";
        std::wstring newApp = dir + L"\\PointerKit.ini";
        if (GetFileAttributesW(newApp.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(oldApp.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(dir.c_str(), nullptr);
            CopyFileW(oldApp.c_str(), newApp.c_str(), TRUE);
        }
    }
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES || dir.empty()) {
        g_iniPath = local;
        return;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    g_iniPath = dir + L"\\PointerKit.ini";
}

// マウスごとの設定：マウスに関する項目は "P1.Buttons" のような別の見出しに保存する
static std::wstring g_secPre;       // 見出しの前に付ける文字（空 = 共通の設定）
static bool g_mouseOnly = false;    // マウスに関する項目だけを書き込む
static bool IsMouseSec(const wchar_t* sec) {
    static const wchar_t* m[] = { L"Buttons", L"Chords", L"Scroll", L"Gesture", L"LongPress", L"DoubleClick" };
    for (auto x : m) if (!wcscmp(sec, x)) return true;
    return false;
}
static std::wstring Sec(const wchar_t* sec) { return IsMouseSec(sec) ? g_secPre + sec : std::wstring(sec); }
static bool Skip(const wchar_t* sec) { return g_mouseOnly && !IsMouseSec(sec); }
static void IniDel(const wchar_t* sec, const wchar_t* key) {
    if (Skip(sec)) return;
    WritePrivateProfileStringW(Sec(sec).c_str(), key, nullptr, g_iniPath.c_str());
}

static std::wstring IniGet(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(Sec(sec).c_str(), key, def, buf, 512, g_iniPath.c_str());
    std::wstring s = buf;
    size_t p = s.find(L';');            // 「;」以降はコメント
    if (p != std::wstring::npos) s = s.substr(0, p);
    return Trim(s);
}
static int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
    std::wstring v = IniGet(sec, key, L"");
    return v.empty() ? def : _wtoi(v.c_str());
}
static bool IniBool(const wchar_t* sec, const wchar_t* key, bool def) {
    std::wstring v = Upper(IniGet(sec, key, L""));
    if (v.empty()) return def;
    return v == L"1" || v == L"TRUE" || v == L"ON" || v == L"YES";
}
static void IniPut(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    if (Skip(sec)) return;
    WritePrivateProfileStringW(Sec(sec).c_str(), key, v.c_str(), g_iniPath.c_str());
}
static void IniPut(const wchar_t* sec, const wchar_t* key, int v) { IniPut(sec, key, std::to_wstring(v)); }

static const wchar_t* kIniHeader =
    L"; PointerKit の設定ファイルです。\r\n"
    L"; 通常はトレイアイコンの「設定...」から変更してください。\r\n"
    L"; このファイルを直接書き換えた場合も、保存すると数秒で反映されます。\r\n\r\n";

static void EnsureIniFile() {
    if (GetFileAttributesW(g_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    const WORD bom = 0xFEFF;   // UTF-16LE にしておくと日本語を正しく扱える
    WriteFile(h, &bom, 2, &w, nullptr);
    WriteFile(h, kIniHeader, (DWORD)(wcslen(kIniHeader) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
}

FILETIME GetIniTime() {
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    if (GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &d)) return d.ftLastWriteTime;
    return FILETIME{};
}

static void SaveBody(const Config& c) {
    IniPut(L"General", L"Enabled", c.enabled ? 1 : 0);
    for (int b = 0; b < B_COUNT; b++) IniPut(L"Buttons", kBtnNames[b], ActionToString(c.button[b]));

    IniPut(L"Chords", L"ChordDelay", c.chordDelay);
    IniPut(L"Chords", L"ChordMove", c.chordMove);
    for (int i = 0; i < kMaxChords; i++) {
        std::wstring n = L"Chord" + std::to_wstring(i + 1);
        if (i < (int)c.chords.size() && c.chords[i].held != B_NONE && c.chords[i].pressed != B_NONE) {
            const ChordDef& ch = c.chords[i];
            IniPut(L"Chords", (n + L"Held").c_str(), kBtnNames[ch.held]);
            IniPut(L"Chords", (n + L"Button").c_str(), kBtnNames[ch.pressed]);
            IniPut(L"Chords", (n + L"Action").c_str(), ActionToString(ch.action));
        } else {
            IniDel(L"Chords", (n + L"Held").c_str());
            IniDel(L"Chords", (n + L"Button").c_str());
            IniDel(L"Chords", (n + L"Action").c_str());
        }
    }

    IniPut(L"Scroll", L"Trigger", kTrigNames[c.trigger]);
    IniPut(L"Scroll", L"ClickIfNoMove", c.clickIfNoMove);
    IniPut(L"Scroll", L"DragThreshold", c.dragThreshold);
    IniPut(L"Scroll", L"LRTimeout", c.lrTimeout);
    IniPut(L"Scroll", L"VerticalSpeed", c.vSpeed);
    IniPut(L"Scroll", L"HorizontalSpeed", c.hSpeed);
    IniPut(L"Scroll", L"Accel", c.accel);
    IniPut(L"Scroll", L"Reverse", c.reverse);
    IniPut(L"Scroll", L"HorizontalReverse", c.hReverse);
    IniPut(L"Scroll", L"Horizontal", c.horizontal);
    IniPut(L"Scroll", L"SwapAxis", c.swapAxis);
    IniPut(L"Scroll", L"AxisLock", c.axisLock);
    IniPut(L"Scroll", L"VerticalThreshold", c.vThreshold);
    IniPut(L"Scroll", L"HorizontalThreshold", c.hThreshold);
    IniPut(L"Scroll", L"RealWheel", c.realWheel);
    IniPut(L"Scroll", L"CursorChange", c.cursorChange);

    IniPut(L"General", L"Theme", c.theme);
    for (int b = 0; b < B_COUNT; b++) {
        IniPut(L"LongPress", kBtnNames[b], ActionToString(c.longPress[b]));
        IniPut(L"DoubleClick", kBtnNames[b], ActionToString(c.dblClick[b]));
    }
    IniPut(L"LongPress", L"Time", c.longMs);
    IniPut(L"DoubleClick", L"Interval", c.dblMs);
    IniPut(L"Scroll", L"Inertia", c.inertia);
    IniPut(L"Scroll", L"InertiaLength", c.inertiaLen);
    for (int i = 0; i < 4; i++) IniPut(L"Corners", kCornerNames[i], ActionToString(c.corner[i]));
    IniPut(L"Corners", L"Delay", c.cornerDelay);
    IniPut(L"Corners", L"SkipFullscreen", c.cornerSkipFull);

    IniPut(L"USKeyboard", L"Mode", c.usMode);
    {
        std::wstring ks;
        for (auto& k : c.usKeyboards) { if (!ks.empty()) ks += L","; ks += k; }
        IniPut(L"USKeyboard", L"Keyboards", ks);
    }
    IniPut(L"USKeyboard", L"Hotkey", c.usHotkey);
    IniPut(L"USKeyboard", L"CapsLock", c.usCaps);

    IniPut(L"Touchpad", L"Enabled", c.tpEnabled);
    for (int i = 0; i < 4; i++)
        IniPut(L"Touchpad", (std::wstring(L"EdgeWidth") + kEdgeNames[i]).c_str(), c.tpEdgeWidth[i]);
    WritePrivateProfileStringW(L"Touchpad", L"EdgeWidth", nullptr, g_iniPath.c_str());   // 旧形式は削除
    IniPut(L"Touchpad", L"Step", c.tpStep);
    IniPut(L"Touchpad", L"FreezeCursor", c.tpFreeze);
    for (int i = 0; i < 4; i++) IniPut(L"Touchpad", kEdgeNames[i], kEdgeActNames[c.tpAct[i]]);

    IniPut(L"Gesture", L"Button", c.gestureBtn == B_NONE ? std::wstring(L"None") : std::wstring(kBtnNames[c.gestureBtn]));
    IniPut(L"Gesture", L"Distance", c.gestureDist);
    IniPut(L"Gesture", L"Show", c.gestureShow);
    IniPut(L"Gesture", L"FreezeCursor", c.gestureFreeze);
    IniPut(L"Gesture", L"Defined", 1);
    for (int i = 0; i < kMaxGestures; i++) {
        std::wstring n = L"Gesture" + std::to_wstring(i + 1);
        if (i < (int)c.gestures.size()) {
            IniPut(L"Gesture", n.c_str(), c.gestures[i].pattern);
            IniPut(L"Gesture", (n + L"Action").c_str(), ActionToString(c.gestures[i].action));
        } else {
            IniDel(L"Gesture", n.c_str());
            IniDel(L"Gesture", (n + L"Action").c_str());
        }
    }
}

static const int kMaxProfiles = 8;
static const wchar_t* kMouseSecs[] = { L"Buttons", L"Chords", L"Scroll", L"Gesture", L"LongPress", L"DoubleClick" };

void SaveConfig(const Config& c) {
    EnsureIniFile();
    g_secPre.clear(); g_mouseOnly = false;
    SaveBody(c);
    IniPut(L"Profiles", L"ShowSwitch", c.profileOsd);
    IniPut(L"Profiles", L"Count", (int)c.profiles.size());
    for (int i = 0; i < kMaxProfiles; i++) {
        std::wstring pre = L"P" + std::to_wstring(i + 1) + L".";
        std::wstring k = L"P" + std::to_wstring(i + 1);
        if (i < (int)c.profiles.size()) {
            IniPut(L"Profiles", (k + L"Id").c_str(), c.profiles[i].id);
            IniPut(L"Profiles", (k + L"Name").c_str(), c.profiles[i].name);
            g_secPre = pre; g_mouseOnly = true;
            SaveBody(c.profiles[i].cfg);
            g_secPre.clear(); g_mouseOnly = false;
        } else {
            IniDel(L"Profiles", (k + L"Id").c_str());
            IniDel(L"Profiles", (k + L"Name").c_str());
            for (auto sec : kMouseSecs) WritePrivateProfileStringW((pre + sec).c_str(), nullptr, nullptr, g_iniPath.c_str());
        }
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_iniPath.c_str());  // flush
    g_iniTime = GetIniTime();
}

Config MergeMouse(const Config& base, const Config& m) {
    Config r = base;
    for (int b = 0; b < B_COUNT; b++) { r.button[b] = m.button[b]; r.longPress[b] = m.longPress[b]; r.dblClick[b] = m.dblClick[b]; }
    r.chords = m.chords; r.chordDelay = m.chordDelay; r.chordMove = m.chordMove;
    r.trigger = m.trigger; r.clickIfNoMove = m.clickIfNoMove; r.dragThreshold = m.dragThreshold; r.lrTimeout = m.lrTimeout;
    r.vSpeed = m.vSpeed; r.hSpeed = m.hSpeed; r.accel = m.accel; r.reverse = m.reverse; r.hReverse = m.hReverse;
    r.horizontal = m.horizontal; r.swapAxis = m.swapAxis; r.axisLock = m.axisLock; r.vThreshold = m.vThreshold;
    r.hThreshold = m.hThreshold; r.realWheel = m.realWheel; r.cursorChange = m.cursorChange;
    r.inertia = m.inertia; r.inertiaLen = m.inertiaLen;
    r.gestureBtn = m.gestureBtn; r.gestureDist = m.gestureDist; r.gestureShow = m.gestureShow; r.gestureFreeze = m.gestureFreeze;
    r.gestures = m.gestures;
    r.longMs = m.longMs; r.dblMs = m.dblMs;
    return r;
}

void ResetGesturesToDefault(Config& c) { c.gestures = DefaultGestures(); }

static Config LoadBody();
void LoadConfig() {
    bool fresh = GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    Config c;
    c.gestures = DefaultGestures();
    if (fresh) {
        SaveConfig(c);   // 初回は既定値で作成
        g_cfg = c;
        return;
    }
    g_secPre.clear();
    c = LoadBody();
    c.profileOsd = IniBool(L"Profiles", L"ShowSwitch", true);
    int n = std::min(kMaxProfiles, std::max(0, IniInt(L"Profiles", L"Count", 0)));
    for (int i = 0; i < n; i++) {
        std::wstring k = L"P" + std::to_wstring(i + 1);
        MouseProfile mp;
        mp.id = IniGet(L"Profiles", (k + L"Id").c_str(), L"");
        mp.name = IniGet(L"Profiles", (k + L"Name").c_str(), L"");
        if (mp.id.empty()) continue;
        g_secPre = k + L".";
        mp.cfg = LoadBody();
        g_secPre.clear();
        if (mp.name.empty()) mp.name = mp.id;
        c.profiles.push_back(mp);
    }
    g_cfg = c;
    g_iniTime = GetIniTime();
}

static Config LoadBody() {
    Config c;
    c.gestures = DefaultGestures();
    c.enabled = IniBool(L"General", L"Enabled", true);
    for (int b = 0; b < B_COUNT; b++) c.button[b] = ParseAction(IniGet(L"Buttons", kBtnNames[b], L"pass"));

    c.chordDelay = std::max(50, IniInt(L"Chords", L"ChordDelay", 750));
    c.chordMove  = std::max(0, IniInt(L"Chords", L"ChordMove", 5));
    for (int i = 0; i < kMaxChords; i++) {
        std::wstring n = L"Chord" + std::to_wstring(i + 1);
        ChordDef ch;
        ch.held = ParseBtn(IniGet(L"Chords", (n + L"Held").c_str(), L""));
        ch.pressed = ParseBtn(IniGet(L"Chords", (n + L"Button").c_str(), L""));
        ch.action = ParseAction(IniGet(L"Chords", (n + L"Action").c_str(), L""));
        if (ch.held != B_NONE && ch.pressed != B_NONE && ch.held != ch.pressed) c.chords.push_back(ch);
    }

    std::wstring t = Upper(IniGet(L"Scroll", L"Trigger", L"Right"));
    c.trigger = T_NONE;
    for (int i = 0; i < T_COUNT; i++) if (t == Upper(kTrigNames[i])) c.trigger = (Trigger)i;
    c.clickIfNoMove = IniBool(L"Scroll", L"ClickIfNoMove", true);
    c.dragThreshold = std::max(0, IniInt(L"Scroll", L"DragThreshold", 5));
    c.lrTimeout     = std::max(20, IniInt(L"Scroll", L"LRTimeout", 200));
    c.vSpeed = std::min(1000, std::max(5, IniInt(L"Scroll", L"VerticalSpeed", 100)));
    c.hSpeed = std::min(1000, std::max(5, IniInt(L"Scroll", L"HorizontalSpeed", 100)));
    c.accel  = std::min(kAccelCount - 1, std::max(0, IniInt(L"Scroll", L"Accel", 1)));
    c.reverse    = IniBool(L"Scroll", L"Reverse", false);
    c.hReverse   = IniBool(L"Scroll", L"HorizontalReverse", false);
    c.horizontal = IniBool(L"Scroll", L"Horizontal", true);
    c.swapAxis   = IniBool(L"Scroll", L"SwapAxis", false);
    c.axisLock   = IniBool(L"Scroll", L"AxisLock", false);
    c.vThreshold = std::max(0, IniInt(L"Scroll", L"VerticalThreshold", 0));
    c.hThreshold = std::max(0, IniInt(L"Scroll", L"HorizontalThreshold", 10));
    c.realWheel  = IniBool(L"Scroll", L"RealWheel", false);
    c.cursorChange = IniBool(L"Scroll", L"CursorChange", false);

    c.theme = std::min(2, std::max(0, IniInt(L"General", L"Theme", 0)));
    for (int b = 0; b < B_COUNT; b++) {
        c.longPress[b] = ParseAction(IniGet(L"LongPress", kBtnNames[b], L"pass"));
        c.dblClick[b] = ParseAction(IniGet(L"DoubleClick", kBtnNames[b], L"pass"));
    }
    c.longMs = std::min(3000, std::max(150, IniInt(L"LongPress", L"Time", 500)));
    c.dblMs = std::min(1000, std::max(100, IniInt(L"DoubleClick", L"Interval", 300)));
    c.inertia = IniBool(L"Scroll", L"Inertia", false);
    c.inertiaLen = std::min(10, std::max(1, IniInt(L"Scroll", L"InertiaLength", 5)));
    for (int i = 0; i < 4; i++) c.corner[i] = ParseAction(IniGet(L"Corners", kCornerNames[i], L"pass"));
    c.cornerDelay = std::min(3000, std::max(0, IniInt(L"Corners", L"Delay", 300)));
    c.cornerSkipFull = IniBool(L"Corners", L"SkipFullscreen", true);

    c.usMode = std::min(2, std::max(0, IniInt(L"USKeyboard", L"Mode", 0)));
    {
        std::wstring ks = IniGet(L"USKeyboard", L"Keyboards", L""), cur;
        for (size_t i = 0; i <= ks.size(); i++) {
            if (i == ks.size() || ks[i] == L',') { cur = Trim(cur); if (!cur.empty()) c.usKeyboards.push_back(cur); cur.clear(); }
            else cur += ks[i];
        }
    }
    c.usHotkey = IniGet(L"USKeyboard", L"Hotkey", L"");
    c.usCaps = IniBool(L"USKeyboard", L"CapsLock", true);

    c.tpEnabled = IniBool(L"Touchpad", L"Enabled", true);
    int oldW = IniInt(L"Touchpad", L"EdgeWidth", 8);   // v1.5 までの共通設定
    for (int i = 0; i < 4; i++)
        c.tpEdgeWidth[i] = std::min(40, std::max(2, IniInt(L"Touchpad", (std::wstring(L"EdgeWidth") + kEdgeNames[i]).c_str(), oldW)));
    c.tpStep = std::min(50, std::max(1, IniInt(L"Touchpad", L"Step", 5)));
    c.tpFreeze = IniBool(L"Touchpad", L"FreezeCursor", true);
    for (int i = 0; i < 4; i++) {
        std::wstring v = Upper(IniGet(L"Touchpad", kEdgeNames[i], kEdgeActNames[c.tpAct[i]]));
        for (int k = 0; k < E_COUNT; k++) if (v == Upper(kEdgeActNames[k])) c.tpAct[i] = (EdgeAct)k;
    }

    c.gestureBtn  = ParseBtn(IniGet(L"Gesture", L"Button", L"None"));
    if (c.gestureBtn == B_LEFT) c.gestureBtn = B_NONE;   // 左ボタンは不可
    c.gestureDist = std::min(300, std::max(5, IniInt(L"Gesture", L"Distance", 30)));
    c.gestureShow = IniBool(L"Gesture", L"Show", true);
    c.gestureFreeze = IniBool(L"Gesture", L"FreezeCursor", true);
    if (IniInt(L"Gesture", L"Defined", 0)) {
        c.gestures.clear();
        for (int i = 0; i < kMaxGestures; i++) {
            std::wstring n = L"Gesture" + std::to_wstring(i + 1);
            GestureDef g;
            g.pattern = IniGet(L"Gesture", n.c_str(), L"");
            g.action = ParseAction(IniGet(L"Gesture", (n + L"Action").c_str(), L""));
            if (!g.pattern.empty()) c.gestures.push_back(g);
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// 設定の書き出し・読み込み（ほかの PC や、配布する相手に渡す用）
// ---------------------------------------------------------------------------
bool ExportConfig(const Config& c, const std::wstring& path) {
    std::wstring keep = g_iniPath;
    FILETIME keepTime = g_iniTime;
    DeleteFileW(path.c_str());
    g_iniPath = path;
    SaveConfig(c);
    bool ok = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    g_iniPath = keep;
    g_iniTime = keepTime;
    return ok;
}

bool ImportConfig(const std::wstring& path, Config& out) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    // PointerKit（OneMouse）の設定ファイルかどうか
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Buttons", L"Left", L"\x01", buf, 64, path.c_str());
    if (buf[0] == 1) return false;
    std::wstring keep = g_iniPath;
    FILETIME keepTime = g_iniTime;
    Config keepCfg = g_cfg;
    g_iniPath = path;
    LoadConfig();
    out = g_cfg;
    g_cfg = keepCfg;
    g_iniPath = keep;
    g_iniTime = keepTime;
    return true;
}
