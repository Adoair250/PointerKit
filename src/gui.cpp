// 設定画面
#include "pointerkit.h"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <set>
#include <commdlg.h>
#include "resource.h"

// ---------------------------------------------------------------------------
// Windows 11 風の配色
// ---------------------------------------------------------------------------
// ライト／ダークで切り替える色
static COLORREF kWinBg, kCardBg, kCardBd, kText, kDisText, kNavSel, kNavHover,
                kBoxBg, kBoxBd, kBtnBg, kBtnBd, kBtnPress, kEditBg, kTpBg, kTpZone, kOnAccent;
static bool g_dark = false;
static COLORREF g_accent = RGB(0, 95, 184);
static HBRUSH g_winBrush = nullptr, g_cardBrush = nullptr, g_editBrush = nullptr;
static std::set<HWND> g_pushBtns;     // 自前で描く普通のボタン（ページ内）

static void SetPalette(bool dark) {
    g_dark = dark;
    if (dark) {
        kWinBg = RGB(32, 32, 32);   kCardBg = RGB(43, 43, 43);  kCardBd = RGB(29, 29, 29);
        kText = RGB(255, 255, 255); kDisText = RGB(120, 120, 120);
        kNavSel = RGB(56, 56, 56);  kNavHover = RGB(48, 48, 48);
        kBoxBg = RGB(39, 39, 39);   kBoxBd = RGB(160, 160, 160);
        kBtnBg = RGB(55, 55, 55);   kBtnBd = RGB(70, 70, 70);   kBtnPress = RGB(45, 45, 45);
        kEditBg = RGB(30, 30, 30);  kTpBg = RGB(50, 52, 56);    kTpZone = RGB(40, 80, 130);
        kOnAccent = RGB(0, 0, 0);
    } else {
        kWinBg = RGB(243, 243, 243); kCardBg = RGB(251, 251, 251); kCardBd = RGB(229, 229, 229);
        kText = RGB(27, 27, 27);     kDisText = RGB(160, 160, 160);
        kNavSel = RGB(233, 233, 233); kNavHover = RGB(237, 237, 237);
        kBoxBg = RGB(255, 255, 255); kBoxBd = RGB(135, 135, 135);
        kBtnBg = RGB(254, 254, 254); kBtnBd = RGB(208, 208, 208); kBtnPress = RGB(245, 245, 245);
        kEditBg = RGB(255, 255, 255); kTpBg = RGB(236, 238, 242); kTpZone = RGB(180, 210, 245);
        kOnAccent = RGB(255, 255, 255);
    }
    for (HBRUSH* b : { &g_winBrush, &g_cardBrush, &g_editBrush }) if (*b) { DeleteObject(*b); *b = nullptr; }
    g_winBrush = CreateSolidBrush(kWinBg);
    g_cardBrush = CreateSolidBrush(kCardBg);
    g_editBrush = CreateSolidBrush(kEditBg);
}
static bool SystemIsDark() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}
static bool WantDark(int theme) { return theme == 2 || (theme == 0 && SystemIsDark()); }
// コンボボックスやスクロールバーなど Windows 標準の部品もダークにする（非公開 API。無ければ何もしない）
static void SetAppDarkMode(bool dark) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    typedef int (WINAPI *PFN_SetPreferredAppMode)(int);
    typedef void (WINAPI *PFN_Flush)();
    if (auto f = (PFN_SetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135))) f(dark ? 2 : 3);
    if (auto f = (PFN_Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136))) f();
}
static void ThemeControl(HWND c, const wchar_t* cls) {
    if (!g_dark) return;
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
    typedef BOOL (WINAPI *PFN_AllowDark)(HWND, BOOL);
    if (!ux) return;
    if (auto f = (PFN_AllowDark)GetProcAddress(ux, MAKEINTRESOURCEA(133))) f(c, TRUE);
    if (auto f = (PFN_SetWindowTheme)GetProcAddress(ux, "SetWindowTheme")) {
        if (!wcscmp(cls, L"COMBOBOX") || !wcscmp(cls, L"EDIT")) f(c, L"DarkMode_CFD", nullptr);
        else f(c, L"DarkMode_Explorer", nullptr);
    }
}
static HFONT g_fontTitle = nullptr, g_fontIcon = nullptr, g_fontNav = nullptr;
static HWND g_nav = nullptr;
static std::set<HWND> g_toggles;
static int g_navHover = -1;           // -2 = 「≡」ボタンの上
static bool g_navCollapsed = false;    // 左メニューを細く（アイコンだけに）しているか
static const int kNavCompact = 56;
static int NavW() { return g_navCollapsed ? kNavCompact : 220; }
static int Off() { return NavW() - 220; }   // 左メニューを細くした分だけ内容を左へずらす

static HWND g_dlg = nullptr;
static HFONT g_font = nullptr, g_fontBold = nullptr;
static int g_dpi = 96;
enum { P_BUTTONS, P_TAP, P_CHORDS, P_SCROLL, P_GESTURE, P_CORNER, P_MICE, P_TOUCHPAD, P_KEYBOARD, P_GENERAL, kPages };
static bool IsMousePage(int p) { return p <= P_GESTURE; }   // マウスごとに変えられるページ
static std::vector<HWND> g_pages[kPages];
static int g_curPage = 0;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// コントロール ID
enum {
    IDC_TAB = 1000,
    IDC_BTN_ACT = 1100,     // + b
    IDC_BTN_KEY = 1120,     // + b
    IDC_CH_HELD = 1200,     // + i
    IDC_CH_PRESS = 1220,
    IDC_CH_ACT = 1240,
    IDC_CH_KEY = 1260,
    IDC_CH_DELAY = 1280,
    IDC_CH_MOVE,
    IDC_TRIGGER = 1300,
    IDC_CLICKNOMOVE, IDC_VSPEED, IDC_HSPEED, IDC_VSPEED_LBL, IDC_HSPEED_LBL, IDC_ACCEL,
    IDC_HORIZ, IDC_REVERSE, IDC_HREVERSE, IDC_SWAP, IDC_AXISLOCK, IDC_CURSOR, IDC_REALWHEEL,
    IDC_DRAGTH, IDC_HTH, IDC_VTH,
    IDC_TP_EN = 1700, IDC_TP_STATUS, IDC_TP_WIDTH, IDC_TP_STEP, IDC_TP_FREEZE, IDC_TP_PREVIEW, IDC_TP_LIVE,
    IDC_TP_ACT = 1720, IDC_TP_W = 1730,
    IDC_G_BTN = 1600, IDC_G_SHOW, IDC_G_DIST, IDC_G_DEFAULT, IDC_G_FREEZE,
    IDC_G_PAT = 1620, IDC_G_ACT = 1640, IDC_G_KEY = 1660,
    IDC_ENABLED = 1400, IDC_STARTUP, IDC_OPENFOLDER, IDC_RESET,
    IDC_LP_ACT = 1800, IDC_LP_KEY = 1810, IDC_DC_ACT = 1820, IDC_DC_KEY = 1830, IDC_LP_MS = 1840, IDC_DC_MS,
    IDC_INERTIA = 1850, IDC_INERTIA_LEN,
    IDC_CN_ACT = 1860, IDC_CN_KEY = 1870, IDC_CN_DELAY = 1880, IDC_CN_FULL,
    IDC_THEME = 1890, IDC_EXPORT, IDC_IMPORT,
    IDC_US_KB = 1900, IDC_US_MODE = 1910, IDC_US_HOT, IDC_US_CAPS, IDC_US_STATUS,
    IDC_MS_USE = 1950, IDC_MS_OSD = 1960, IDC_MS_LAST, IDC_PROF_SEL, IDC_PROF_LBL,
    IDC_OK = 1500, IDC_CANCEL, IDC_APPLY,
};

static const wchar_t* kBtnShort[B_COUNT] = { L"左", L"右", L"中", L"戻る", L"進む" };

// ボタン確認（実際に押したボタンを光らせる）
static HWND g_btnLbl[B_COUNT] = {};
static bool g_pressed[B_COUNT] = {};
static HWND g_monLbl = nullptr;
static HBRUSH g_hiBrush = nullptr;
static const COLORREF kHiColor = RGB(255, 214, 102);
static const UINT_PTR TIMER_WHEELMSG = 7, TIMER_USSTATUS = 8;

// ---------------------------------------------------------------------------
// キー入力欄（押したキーの組み合わせをそのまま表示する）
// ---------------------------------------------------------------------------
static std::wstring ModPrefix() {
    std::wstring s;
    if (GetKeyState(VK_CONTROL) & 0x8000) s += L"Ctrl+";
    if (GetKeyState(VK_SHIFT) & 0x8000) s += L"Shift+";
    if (GetKeyState(VK_MENU) & 0x8000) s += L"Alt+";
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) s += L"Win+";
    return s;
}
static bool IsModifier(WPARAM vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        return true;
    }
    return false;
}

static LRESULT CALLBACK KeyEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        WORD vk = (WORD)wp;
        if (vk == VK_PROCESSKEY) return 0;
        std::wstring mods = ModPrefix();
        if (IsModifier(vk)) { SetWindowTextW(h, mods.c_str()); return 0; }
        if ((vk == VK_BACK || vk == VK_DELETE) && mods.empty()) { SetWindowTextW(h, L""); return 0; }
        std::wstring t = mods + VkName(vk);
        SetWindowTextW(h, t.c_str());
        SendMessageW(h, EM_SETSEL, t.size(), t.size());
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        // 修飾キーだけで離した場合（"Ctrl+" で終わっている）は消す
        wchar_t buf[128]; GetWindowTextW(h, buf, 128);
        std::wstring s = buf;
        if (!s.empty() && s.back() == L'+' && ModPrefix().empty()) SetWindowTextW(h, L"");
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR: case WM_IME_CHAR:
        return 0;
    case WM_CONTEXTMENU: case WM_PASTE:
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// コントロール作成
// ---------------------------------------------------------------------------
static HWND Make(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h), g_dlg,
                             (HMENU)(INT_PTR)id, g_hinst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_dark && (!wcscmp(cls, L"COMBOBOX") || !wcscmp(cls, L"EDIT") || !wcscmp(cls, TRACKBAR_CLASSW))) ThemeControl(c, cls);
    if (page >= 0) g_pages[page].push_back(c);
    else ShowWindow(c, SW_SHOW);
    return c;
}
static HWND Label(int page, const wchar_t* t, int x, int y, int w, int h = 20, bool bold = false) {
    HWND c = Make(page, L"STATIC", t, SS_LEFT, x, y, w, h, -1);
    if (bold) SendMessageW(c, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    return c;
}
static HWND Combo(int page, int x, int y, int w, int id) {
    return Make(page, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id);
}
// Windows 11 風のチェックボックス（自前で描画。状態は GWLP_USERDATA に保持）
static HWND Check(int page, const wchar_t* t, int x, int y, int w, int id) {
    HWND b = Make(page, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 26, id);
    g_toggles.insert(b);
    return b;
}
static HWND NumEdit(int page, int x, int y, int w, int id) {
    return Make(page, L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_TABSTOP, x, y, w, 23, id, WS_EX_CLIENTEDGE);
}
static HWND KeyEdit(int page, int x, int y, int w, int id) {
    HWND e = Make(page, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x, y, w, 23, id, WS_EX_CLIENTEDGE);
    SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)L"ここでキーを押す");
    SetWindowSubclass(e, KeyEditProc, 1, 0);
    return e;
}
static HWND Item(int id) { return GetDlgItem(g_dlg, id); }
static HWND PushBtn(int page, const wchar_t* t, int x, int y, int w, int id) {
    HWND b = Make(page, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 28, id);
    g_pushBtns.insert(b);
    return b;
}

static void FillActions(HWND cb) {
    for (int i = 0; i < A_COUNT; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kActLabels[i]);
}
static void SetCheck(int id, bool v) { SetWindowLongPtrW(Item(id), GWLP_USERDATA, v ? 1 : 0); InvalidateRect(Item(id), nullptr, TRUE); }
static bool GetCheck(int id) { return GetWindowLongPtrW(Item(id), GWLP_USERDATA) != 0; }

// ---------------------------------------------------------------------------
// 描画ヘルパー
// ---------------------------------------------------------------------------
static void FillRound(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

// Windows 11 風のチェックボックス（角丸の四角、オンでアクセントカラー＋白いチェック）
static void DrawToggle(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, g_cardBrush);
    bool on = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA) != 0;
    bool dis = (di->itemState & ODS_DISABLED) != 0;
    int bs = S(18);
    int by = r.top + (r.bottom - r.top - bs) / 2;
    RECT box = { r.left + S(1), by, r.left + S(1) + bs, by + bs };
    COLORREF acc = dis ? (g_dark ? RGB(90, 90, 90) : RGB(190, 190, 190)) : g_accent;
    if (on) {
        FillRound(dc, box, S(5), acc, acc);
        HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), kOnAccent);
        HGDIOBJ op = SelectObject(dc, pen);
        POINT pts[3] = { { box.left + bs * 24 / 100, box.top + bs * 52 / 100 },
                         { box.left + bs * 42 / 100, box.top + bs * 70 / 100 },
                         { box.left + bs * 76 / 100, box.top + bs * 32 / 100 } };
        Polyline(dc, pts, 3);
        SelectObject(dc, op); DeleteObject(pen);
    } else {
        FillRound(dc, box, S(5), kBoxBg, dis ? kDisText : kBoxBd);
    }
    wchar_t text[256]; GetWindowTextW(di->hwndItem, text, 256);
    RECT tr = { box.right + S(10), r.top, r.right, r.bottom };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dis ? kDisText : kText);
    HGDIOBJ of = SelectObject(dc, g_font);
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    if (di->itemState & ODS_FOCUS) {
        RECT fr = box; InflateRect(&fr, S(3), S(3));
        DrawFocusRect(dc, &fr);
    }
}

// OK・キャンセル・適用ボタン（OK は強調色）
static void DrawPushButton(const DRAWITEMSTRUCT* di, bool primary, bool onCard = false) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, onCard ? g_cardBrush : g_winBrush);
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool dis = (di->itemState & ODS_DISABLED) != 0;
    RECT b = r; InflateRect(&b, -1, -1);
    COLORREF fill = primary ? g_accent : kBtnBg;
    if (pressed) fill = primary ? RGB(GetRValue(g_accent) * 85 / 100, GetGValue(g_accent) * 85 / 100, GetBValue(g_accent) * 85 / 100)
                                : kBtnPress;
    FillRound(dc, b, S(8), fill, primary ? fill : kBtnBd);
    wchar_t text[64]; GetWindowTextW(di->hwndItem, text, 64);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dis ? kDisText : primary ? kOnAccent : kText);
    HGDIOBJ of = SelectObject(dc, g_font);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    if (di->itemState & ODS_FOCUS) {
        RECT fr = r; InflateRect(&fr, -S(3), -S(3));
        DrawFocusRect(dc, &fr);
    }
}
static void SetNum(int id, int v) { SetWindowTextW(Item(id), std::to_wstring(v).c_str()); }
static int GetNum(int id, int def) {
    wchar_t b[32]; GetWindowTextW(Item(id), b, 32);
    return b[0] ? _wtoi(b) : def;
}
static int GetSel(int id) { return (int)SendMessageW(Item(id), CB_GETCURSEL, 0, 0); }
static void SetSel(int id, int i) { SendMessageW(Item(id), CB_SETCURSEL, i, 0); }
static std::wstring GetText(int id) {
    wchar_t b[256]; GetWindowTextW(Item(id), b, 256);
    return b;
}

// ---------------------------------------------------------------------------
// 画面の組み立て
// ---------------------------------------------------------------------------
static const int kNavW = 220;                // 左メニューの幅
static const int PX = kNavW + 36, PY = 84;   // ページ内側の原点（カードの内側）
static const int kWinW = PX + 560 + 36, kWinH = 610;
static const wchar_t* kPageTitles[kPages] = { L"ボタン", L"長押し・2回押し", L"同時押し", L"スクロール", L"ジェスチャー",
                                              L"画面の四隅", L"マウスごとの設定", L"タッチパッド", L"キーボード", L"全般" };
// Segoe Fluent Icons（Windows 11）／Segoe MDL2 Assets（Windows 10）のアイコン
static const wchar_t* kPageIcons[kPages] = { L"\uE962", L"\uE916", L"\uE71D", L"\uE8CB", L"\uE7C9", L"\uE740", L"\uE8B7", L"\uEDA4", L"\uE765", L"\uE713" };

static void BuildButtonsPage() {
    const int p = P_BUTTONS;
    Label(p, L"ボタンを押したときの動作を選びます（実際に押すと、その行が黄色く光ります）。", PX, PY, 540);
    Label(p, L"「キー入力」を選んだら、右の欄をクリックして割り当てたいキーを押してください（例: Ctrl+W）。", PX, PY + 22, 550, 36);
    Label(p, L"ボタン", PX, PY + 70, 180, 20, true);
    Label(p, L"動作", PX + 190, PY + 70, 200, 20, true);
    Label(p, L"キー", PX + 420, PY + 70, 120, 20, true);
    for (int b = 0; b < B_COUNT; b++) {
        int y = PY + 96 + b * 40;
        g_btnLbl[b] = Make(p, L"STATIC", (std::wstring(L" ") + kBtnLabels[b]).c_str(), SS_LEFT | SS_CENTERIMAGE, PX - 6, y, 191, 24, -1);
        FillActions(Combo(p, PX + 190, y, 220, IDC_BTN_ACT + b));
        KeyEdit(p, PX + 420, y, 120, IDC_BTN_KEY + b);
    }
    Label(p, L"※ スクロールの「押しながら動かす」ボタンに選んだボタンは、動かさずに離したときにここの動作をします。\n"
             L"※ 戻る／進むボタンがないマウスでは、その行は使われません。",
          PX, PY + 310, 550, 72);
}

static void BuildChordsPage() {
    const int p = P_CHORDS;
    Label(p, L"ボタンを押したまま、別のボタンを押したときの動作です。", PX, PY, 540);
    Label(p, L"例：「左」を押したまま「進む」を押す → Ctrl+W（ブラウザのタブを閉じる）", PX, PY + 22, 540);
    Label(p, L"押さえる", PX, PY + 56, 80, 20, true);
    Label(p, L"押す", PX + 90, PY + 56, 80, 20, true);
    Label(p, L"動作", PX + 180, PY + 56, 200, 20, true);
    Label(p, L"キー", PX + 420, PY + 56, 120, 20, true);
    for (int i = 0; i < kMaxChords; i++) {
        int y = PY + 80 + i * 36;
        HWND h1 = Combo(p, PX, y, 80, IDC_CH_HELD + i);
        HWND h2 = Combo(p, PX + 90, y, 80, IDC_CH_PRESS + i);
        SendMessageW(h1, CB_ADDSTRING, 0, (LPARAM)L"（なし）");
        SendMessageW(h2, CB_ADDSTRING, 0, (LPARAM)L"（なし）");
        for (int b = 0; b < B_COUNT; b++) {
            SendMessageW(h1, CB_ADDSTRING, 0, (LPARAM)kBtnShort[b]);
            SendMessageW(h2, CB_ADDSTRING, 0, (LPARAM)kBtnShort[b]);
        }
        FillActions(Combo(p, PX + 180, y, 230, IDC_CH_ACT + i));
        KeyEdit(p, PX + 420, y, 120, IDC_CH_KEY + i);
    }
    int y = PY + 80 + kMaxChords * 36 + 10;
    Label(p, L"「押さえる」側のボタンは、同時押しかどうか判断するため、押した瞬間は送らずに少し待ちます。\n"
             L"下の時間押し続けるか、下の距離だけ動かすと、普通の押下（ドラッグなど）として扱います。",
          PX, y, 550, 56);
    Label(p, L"待ち時間（ミリ秒）", PX, y + 69, 160);
    NumEdit(p, PX + 165, y + 66, 70, IDC_CH_DELAY);
    Label(p, L"動かしてよい距離（px）", PX + 270, y + 69, 170);
    NumEdit(p, PX + 445, y + 66, 70, IDC_CH_MOVE);
}

static void BuildScrollPage() {
    const int p = P_SCROLL;
    Label(p, L"ボタンを押したままボール（マウス）を動かすと、縦・横にスクロールします。", PX, PY, 550);
    Label(p, L"スクロールの方法", PX, PY + 32, 130);
    HWND t = Combo(p, PX + 135, PY + 29, 400, IDC_TRIGGER);
    for (int i = 0; i < T_COUNT; i++) SendMessageW(t, CB_ADDSTRING, 0, (LPARAM)kTrigLabels[i]);
    Check(p, L"動かさずに離したときは、普通のクリック（ボタン割り当ての動作）にする", PX, PY + 62, 540, IDC_CLICKNOMOVE);

    Label(p, L"縦の速さ", PX, PY + 100, 90);
    Make(p, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, PX + 95, PY + 96, 360, 28, IDC_VSPEED);
    Label(p, L"", PX + 465, PY + 100, 70);
    SetWindowLongPtrW(g_pages[p].back(), GWLP_ID, IDC_VSPEED_LBL);
    Label(p, L"横の速さ", PX, PY + 134, 90);
    Make(p, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, PX + 95, PY + 130, 360, 28, IDC_HSPEED);
    Label(p, L"", PX + 465, PY + 134, 70);
    SetWindowLongPtrW(g_pages[p].back(), GWLP_ID, IDC_HSPEED_LBL);
    for (int id : { IDC_VSPEED, IDC_HSPEED }) {
        SendMessageW(Item(id), TBM_SETRANGE, TRUE, MAKELPARAM(10, 400));
        SendMessageW(Item(id), TBM_SETPAGESIZE, 0, 10);
    }
    Label(p, L"加速", PX, PY + 170, 90);
    HWND a = Combo(p, PX + 95, PY + 167, 250, IDC_ACCEL);
    for (int i = 0; i < kAccelCount; i++) SendMessageW(a, CB_ADDSTRING, 0, (LPARAM)kAccelLabels[i]);
    Label(p, L"（速く動かすほど大きく進む）", PX + 355, PY + 170, 190);

    Check(p, L"横スクロールも使う", PX, PY + 208, 260, IDC_HORIZ);
    Check(p, L"縦スクロールの向きを逆にする", PX, PY + 234, 260, IDC_REVERSE);
    Check(p, L"横スクロールの向きを逆にする", PX, PY + 260, 260, IDC_HREVERSE);
    Check(p, L"縦と横を入れ替える", PX, PY + 286, 260, IDC_SWAP);
    Check(p, L"最初に動かした方向（縦か横）に固定", PX + 275, PY + 208, 280, IDC_AXISLOCK);
    Check(p, L"スクロール中はカーソルの形を変える", PX + 275, PY + 234, 280, IDC_CURSOR);
    Check(p, L"1目盛りずつ送る（古いアプリ向け）", PX + 275, PY + 260, 280, IDC_REALWHEEL);
    Check(p, L"離したあとも少し滑らせる（慣性スクロール）", PX + 275, PY + 286, 280, IDC_INERTIA);

    Label(p, L"細かい調整（px）", PX, PY + 326, 200, 20, true);
    Label(p, L"スクロール開始までの移動量", PX, PY + 352, 200);
    NumEdit(p, PX + 205, PY + 349, 55, IDC_DRAGTH);
    Label(p, L"横スクロールのあそび", PX + 275, PY + 352, 200);
    NumEdit(p, PX + 480, PY + 349, 55, IDC_HTH);
    Label(p, L"縦スクロールのあそび", PX + 275, PY + 384, 200);
    NumEdit(p, PX + 480, PY + 381, 55, IDC_VTH);
    Label(p, L"慣性で滑る長さ（1〜10）", PX, PY + 384, 200);
    NumEdit(p, PX + 205, PY + 381, 55, IDC_INERTIA_LEN);
}

static void SmallCombo(int page, int x, int y, int w, int id) {
    HWND cb = Combo(page, x, y, w, id);
    FillActions(cb);
    SendMessageW(cb, CB_SETDROPPEDWIDTH, S(270), 0);
}

static void BuildTapPage() {
    const int p = P_TAP;
    Label(p, L"ボタンを長く押したとき・すばやく2回押したときに、別の動作をさせます。", PX, PY, 550);
    Label(p, L"「そのまま」にした欄は、今までどおり（「ボタン」ページの動作）です。", PX, PY + 22, 550);
    Label(p, L"ボタン", PX, PY + 56, 60, 20, true);
    Label(p, L"長押し", PX + 65, PY + 56, 140, 20, true);
    Label(p, L"キー", PX + 210, PY + 56, 90, 20, true);
    Label(p, L"2回押し", PX + 310, PY + 56, 140, 20, true);
    Label(p, L"キー", PX + 455, PY + 56, 90, 20, true);
    for (int b = 0; b < B_COUNT; b++) {
        int y = PY + 80 + b * 38;
        Label(p, kBtnShort[b], PX, y + 3, 60);
        SmallCombo(p, PX + 65, y, 140, IDC_LP_ACT + b);
        KeyEdit(p, PX + 210, y, 90, IDC_LP_KEY + b);
        SmallCombo(p, PX + 310, y, 140, IDC_DC_ACT + b);
        KeyEdit(p, PX + 455, y, 90, IDC_DC_KEY + b);
    }
    int y = PY + 80 + B_COUNT * 38 + 8;
    Label(p, L"長押しと判断する時間（ミリ秒）", PX, y + 3, 225);
    NumEdit(p, PX + 230, y, 70, IDC_LP_MS);
    Label(p, L"2回押しと判断する間隔（ミリ秒）", PX, y + 35, 225);
    NumEdit(p, PX + 230, y + 32, 70, IDC_DC_MS);
    Label(p, L"※ 2回押しを割り当てたボタンは、1回だけ押したときの動作が上の間隔の分だけ遅れます。\n"
             L"※ スクロール・ジェスチャー・同時押しの「押さえる」側に使っているボタンでは使えません（灰色になります）。",
          PX, y + 70, 550, 56);
}

// US配列キーボード
static const int kMaxKb = 6;
static std::vector<std::wstring> g_kbIds;   // 画面に並べたキーボードの ID（チェック欄の順）

static void UpdateUsStatus() {
    if (!Item(IDC_US_STATUS)) return;
    SetWindowTextW(Item(IDC_US_STATUS), UsActive() ? L"いまの状態：変換オン（US配列として入力中）" : L"いまの状態：変換オフ（日本語配列のまま）");
}

static void BuildKeyboardPage() {
    const int p = P_KEYBOARD;
    Label(p, L"US配列のキーボードをつないだときに、キーの刻印どおりの記号が入力されるようにします（例：Shift+2 で @）。\n"
             L"Windows のキーボード設定は日本語配列のまま使えます。", PX, PY, 550, 36);
    Label(p, L"変換のしかた", PX, PY + 49, 100);
    HWND m = Combo(p, PX + 105, PY + 46, 430, IDC_US_MODE);
    for (auto t : { L"使わない", L"下でチェックしたキーボードをつないでいる間だけ変換する（自動）", L"いつも変換する" })
        SendMessageW(m, CB_ADDSTRING, 0, (LPARAM)t);

    Label(p, L"US配列のキーボード（チェックしたものを US配列として扱います）", PX, PY + 84, 550, 20, true);
    g_kbIds.clear();
    std::vector<std::wstring> names;
    for (auto& k : ListKeyboards()) { g_kbIds.push_back(k.id); names.push_back(k.name); }
    for (auto& id : g_cfg.usKeyboards)
        if (std::find(g_kbIds.begin(), g_kbIds.end(), id) == g_kbIds.end()) { g_kbIds.push_back(id); names.push_back(L"いまはつながっていないキーボード（" + id + L"）"); }
    if (g_kbIds.size() > (size_t)kMaxKb) { g_kbIds.resize(kMaxKb); names.resize(kMaxKb); }
    for (size_t i = 0; i < g_kbIds.size(); i++) Check(p, names[i].c_str(), PX, PY + 108 + (int)i * 26, 550, IDC_US_KB + (int)i);
    if (g_kbIds.empty()) Label(p, L"キーボードが見つかりません。", PX, PY + 110, 400);

    Label(p, L"手動で切り替えるショートカット", PX, PY + 275, 210);
    KeyEdit(p, PX + 215, PY + 272, 160, IDC_US_HOT);
    Label(p, L"（トレイのメニューでも切り替え可）", PX + 385, PY + 275, 165);
    Check(p, L"CapsLock キーで大文字固定にする（US配列と同じ動き）", PX, PY + 304, 500, IDC_US_CAPS);
    Label(p, L"", PX, PY + 336, 400, 20, true);
    SetWindowLongPtrW(g_pages[p].back(), GWLP_ID, IDC_US_STATUS);
    Label(p, L"※ USkey2JP など同じ働きのソフトとは同時に使わないでください。\n"
             L"※ Ctrl・Alt・Win と一緒に押したキーは変換しません（Alt+` で日本語入力を切り替えられます）。\n"
             L"※ 自動のときも、手動で切り替えた状態はキーボードをつなぎ直すまで続きます。",
          PX, PY + 362, 550, 60);
}

// ---------------------------------------------------------------------------
// マウスごとの設定
// ---------------------------------------------------------------------------
static const int kMaxMice = 6;
static std::vector<std::wstring> g_msIds, g_msNames;   // 画面に並べたマウス（チェック欄の順）
static Config g_ui;          // 設定画面で編集中の内容（共通 + マウスごとの設定）
static int g_editSlot = -1;  // いま画面に出しているのは -1 = 共通、0〜 = g_ui.profiles の番号

static int ProfileIndex(const std::wstring& id) {
    for (int i = 0; i < (int)g_ui.profiles.size(); i++) if (g_ui.profiles[i].id == id) return i;
    return -1;
}
// 画面に出す設定（マウスごとの設定を選んでいるときは、マウスの項目だけそのマウスのもの）
static Config ViewConfig() {
    if (g_editSlot < 0 || g_editSlot >= (int)g_ui.profiles.size()) return g_ui;
    return MergeMouse(g_ui, g_ui.profiles[g_editSlot].cfg);
}

static void BuildMicePage() {
    const int p = P_MICE;
    Label(p, L"マウスを何台か使うとき、マウスごとに別の設定にできます。チェックしたマウスは専用の設定を持ち、\n"
             L"そのマウスを動かすと自動で切り替わります。チェックしていないマウスは共通の設定を使います。", PX, PY, 550, 36);
    Label(p, L"マウス（専用の設定を使うものにチェック）", PX, PY + 50, 550, 20, true);
    g_msIds.clear(); g_msNames.clear();
    for (auto& m : ListMice()) { g_msIds.push_back(m.id); g_msNames.push_back(m.name); }
    for (auto& pr : g_cfg.profiles)
        if (std::find(g_msIds.begin(), g_msIds.end(), pr.id) == g_msIds.end()) { g_msIds.push_back(pr.id); g_msNames.push_back(pr.name); }
    if (g_msIds.size() > (size_t)kMaxMice) { g_msIds.resize(kMaxMice); g_msNames.resize(kMaxMice); }
    for (size_t i = 0; i < g_msIds.size(); i++) {
        std::wstring t = g_msNames[i] + L"（" + g_msIds[i] + L"）";
        Check(p, t.c_str(), PX, PY + 74 + (int)i * 26, 550, IDC_MS_USE + (int)i);
    }
    if (g_msIds.empty()) Label(p, L"マウスが見つかりません。", PX, PY + 76, 400);
    Label(p, L"", PX, PY + 240, 550, 20, true);
    SetWindowLongPtrW(g_pages[p].back(), GWLP_ID, IDC_MS_LAST);
    Check(p, L"切り替わったときに画面に小さく表示する", PX, PY + 268, 500, IDC_MS_OSD);
    Label(p, L"設定のしかた",PX, PY + 306, 300, 20, true);
    Label(p, L"「ボタン」〜「ジェスチャー」のページの右上にある「設定するマウス」で、どのマウスの設定を変えるか選びます。\n"
             L"チェックを入れたときは、いまの共通の設定がそのマウスにコピーされます。チェックを外すと、そのマウスの設定は消えます。\n"
             L"※ 同じ機種のマウスを2台つないだときは、見分けられないため同じ設定になります。",
          PX, PY + 330, 550, 76);
}

// 「設定するマウス」の選択肢を作り直す
static void FillProfileCombo() {
    HWND cb = Item(IDC_PROF_SEL);
    if (!cb) return;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"共通（ほかのマウスすべて）");
    for (auto& pr : g_ui.profiles) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)pr.name.c_str());
    SendMessageW(cb, CB_SETCURSEL, g_editSlot + 1, 0);
}
static void UpdateProfileVisibility() {
    bool show = IsMousePage(g_curPage) && !g_ui.profiles.empty();
    if (Item(IDC_PROF_SEL)) ShowWindow(Item(IDC_PROF_SEL), show ? SW_SHOW : SW_HIDE);
    if (Item(IDC_PROF_LBL)) ShowWindow(Item(IDC_PROF_LBL), show ? SW_SHOW : SW_HIDE);
}
static void RefreshMiceUi() {
    for (size_t i = 0; i < g_msIds.size(); i++) SetCheck(IDC_MS_USE + (int)i, ProfileIndex(g_msIds[i]) >= 0);
    SetCheck(IDC_MS_OSD, g_ui.profileOsd);
    FillProfileCombo();
    UpdateProfileVisibility();
}
static void UpdateLastMouse() {
    if (!Item(IDC_MS_LAST)) return;
    std::wstring id = LastMouseId();
    std::wstring t = L"いま動かしたマウス：";
    if (id.empty()) t += L"（マウスを少し動かしてください）";
    else {
        std::wstring name = id;
        for (size_t i = 0; i < g_msIds.size(); i++) if (g_msIds[i] == id) name = g_msNames[i] + L"（" + id + L"）";
        t += name;
    }
    wchar_t cur[512]; GetWindowTextW(Item(IDC_MS_LAST), cur, 512);
    if (t != cur) SetWindowTextW(Item(IDC_MS_LAST), t.c_str());
}

static void BuildCornerPage() {
    const int p = P_CORNER;
    Label(p, L"カーソルを画面の角に当てて少し待つと、割り当てた動作をします。", PX, PY, 550);
    Label(p, L"例：左上に Win+Tab（タスクビュー）、右下に Win+D（デスクトップを表示）", PX, PY + 22, 550);
    Label(p, L"角", PX, PY + 56, 60, 20, true);
    Label(p, L"動作", PX + 65, PY + 56, 200, 20, true);
    Label(p, L"キー", PX + 335, PY + 56, 150, 20, true);
    for (int i = 0; i < 4; i++) {
        int y = PY + 80 + i * 38;
        Label(p, kCornerLabels[i], PX, y + 3, 60);
        FillActions(Combo(p, PX + 65, y, 260, IDC_CN_ACT + i));
        KeyEdit(p, PX + 335, y, 150, IDC_CN_KEY + i);
    }
    int y = PY + 80 + 4 * 38 + 10;
    Label(p, L"角に当ててから動作するまでの時間（ミリ秒）", PX, y + 3, 290);
    NumEdit(p, PX + 295, y, 70, IDC_CN_DELAY);
    Check(p, L"全画面のアプリ（動画・ゲームなど）が前面にあるときは使わない", PX, y + 36, 540, IDC_CN_FULL);
    Label(p, L"※ モニターを複数つないでいる場合、隣のモニターとつながっている角は使われません。\n"
             L"※ マウスのボタンを押している間（ドラッグ中など）は動作しません。",
          PX, y + 76, 550, 56);
}

static const wchar_t* kGBtnLabels[] = { L"使わない", L"右ボタン", L"中ボタン（ホイール押し）", L"戻るボタン", L"進むボタン" };
static const Btn kGBtnValues[] = { B_NONE, B_RIGHT, B_MIDDLE, B_X1, B_X2 };

static void BuildGesturePage() {
    const int p = P_GESTURE;
    Label(p, L"ボタンを押したままマウスを動かして離すと、動きに合わせた動作をします（例：← で戻る）。", PX, PY, 550);
    Label(p, L"ジェスチャーに使うボタン", PX, PY + 32, 170);
    HWND gb = Combo(p, PX + 175, PY + 29, 220, IDC_G_BTN);
    for (auto l : kGBtnLabels) SendMessageW(gb, CB_ADDSTRING, 0, (LPARAM)l);
    Check(p, L"動きと動作名を画面に表示する", PX, PY + 62, 280, IDC_G_SHOW);
    Check(p, L"ジェスチャー中はカーソルを動かさない", PX, PY + 86, 280, IDC_G_FREEZE);
    Label(p, L"1つの動きと見なす距離（px）", PX + 290, PY + 64, 190);
    NumEdit(p, PX + 485, PY + 61, 55, IDC_G_DIST);

    Label(p, L"動き", PX, PY + 112, 110, 20, true);
    Label(p, L"動作", PX + 120, PY + 112, 200, 20, true);
    Label(p, L"キー", PX + 400, PY + 112, 120, 20, true);
    for (int i = 0; i < kMaxGestures; i++) {
        int y = PY + 134 + i * 33;
        HWND pc = Combo(p, PX, y, 110, IDC_G_PAT + i);
        SendMessageW(pc, CB_ADDSTRING, 0, (LPARAM)L"（なし）");
        for (int k = 0; k < kGesturePatternCount; k++)
            SendMessageW(pc, CB_ADDSTRING, 0, (LPARAM)GestureArrows(kGesturePatterns[k]).c_str());
        FillActions(Combo(p, PX + 120, y, 270, IDC_G_ACT + i));
        KeyEdit(p, PX + 400, y, 140, IDC_G_KEY + i);
    }
    PushBtn(p, L"おすすめの割り当てに戻す", PX + 340, PY + 392, 200, IDC_G_DEFAULT);
    Label(p, L"動かさずに離すと、そのボタンの普通の動作になります。", PX, PY + 398, 330);
}

// タッチパッドの様子（設定画面の図に表示）
static int g_tpX = 0, g_tpY = 0, g_tpFingers = 0, g_tpEdge = -1;

static void BuildTouchpadPage() {
    const int p = P_TOUCHPAD;
    Label(p, L"ノートPCのタッチパッドの辺に指を置いて、辺に沿って動かすと操作できます。\n"
             L"（左右の辺は上下に、上下の辺は左右に動かします。例：右の辺を上へなぞると音量アップ）", PX, PY, 550, 36);
    Check(p, L"タッチパッドの辺の機能を使う", PX, PY + 44, 300, IDC_TP_EN);
    std::wstring st = TouchpadCount() > 0
        ? L"このPCの高精度タッチパッド：" + std::to_wstring(TouchpadCount()) + L" 台を検出"
        : std::wstring(L"このPCでは高精度タッチパッドが見つかりません（この機能は使えません）");
    Label(p, st.c_str(), PX, PY + 70, 330);
    for (int i = 0; i < 4; i++) {
        int y = PY + 102 + i * 36;
        Label(p, kEdgeLabels[i], PX, y + 3, 70);
        HWND cb = Combo(p, PX + 75, y, 165, IDC_TP_ACT + i);
        Label(p, L"幅", PX + 248, y + 3, 20);
        NumEdit(p, PX + 268, y, 38, IDC_TP_W + i);
        Label(p, L"%", PX + 309, y + 3, 16);
        for (int k = 0; k < E_COUNT; k++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kEdgeActLabels[k]);
    }
    Make(p, L"STATIC", L"", SS_OWNERDRAW, PX + 330, PY + 98, 210, 140, IDC_TP_PREVIEW);
    Label(p, L"タッチパッドに触れると、図に指の位置が出ます", PX + 330, PY + 244, 215, 36);
    SetWindowLongPtrW(g_pages[p].back(), GWLP_ID, IDC_TP_LIVE);

    Label(p, L"幅：辺とみなす範囲（タッチパッドの何%か）", PX, PY + 250, 320);
    Label(p, L"1回分の操作に必要な移動量（%）", PX, PY + 298, 220);
    NumEdit(p, PX + 225, PY + 295, 55, IDC_TP_STEP);
    Check(p, L"辺を操作している間はカーソルを動かさない", PX, PY + 330, 330, IDC_TP_FREEZE);
    Label(p, L"※ 高精度タッチパッドでのみ使えます。明るさは、ノートPCの内蔵画面だけが対象です。", PX, PY + 366, 550, 36);
}

static void DrawTouchpadPreview(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    HBRUSH bg = CreateSolidBrush(kTpBg);
    FillRect(dc, &r, bg); DeleteObject(bg);
    int w = r.right - r.left, h = r.bottom - r.top;
    bool en = GetCheck(IDC_TP_EN);
    for (int i = 0; i < 4; i++) {
        if (!en || GetSel(IDC_TP_ACT + i) <= 0) continue;
        RECT z = r;
        int ew = std::max(2, std::min(40, GetNum(IDC_TP_W + i, 8)));
        int zx = w * ew / 100, zy = h * ew / 100;
        if (i == 0) z.right = r.left + zx;
        if (i == 1) z.left = r.right - zx;
        if (i == 2) z.bottom = r.top + zy;
        if (i == 3) z.top = r.bottom - zy;
        HBRUSH b = CreateSolidBrush(g_tpEdge == i ? RGB(255, 190, 80) : kTpZone);
        FillRect(dc, &z, b); DeleteObject(b);
    }
    HBRUSH frame = CreateSolidBrush(RGB(120, 120, 130));
    FrameRect(dc, &r, frame); DeleteObject(frame);
    if (g_tpFingers > 0) {
        int x = r.left + w * g_tpX / 1000, y = r.top + h * g_tpY / 1000;
        int rad = S(7);
        HBRUSH dot = CreateSolidBrush(RGB(220, 40, 50));
        HGDIOBJ ob = SelectObject(dc, dot);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, x - rad, y - rad, x + rad, y + rad);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(dot);
    }
}

static void BuildGeneralPage() {
    const int p = P_GENERAL;
    Check(p, L"PointerKit を有効にする", PX, PY, 400, IDC_ENABLED);
    Check(p, L"Windows の起動時に自動で起動する", PX, PY + 30, 400, IDC_STARTUP);
    Label(p, L"画面の色", PX, PY + 67, 90);
    HWND th = Combo(p, PX + 95, PY + 64, 220, IDC_THEME);
    for (auto t : { L"Windows の設定に合わせる", L"ライト", L"ダーク" }) SendMessageW(th, CB_ADDSTRING, 0, (LPARAM)t);

    Label(p, L"設定ファイル", PX, PY + 106, 300, 20, true);
    Make(p, L"EDIT", g_iniPath.c_str(), ES_READONLY | ES_AUTOHSCROLL, PX, PY + 130, 430, 23, -1, WS_EX_CLIENTEDGE);
    PushBtn(p, L"フォルダを開く", PX + 440, PY + 128, 105, IDC_OPENFOLDER);
    PushBtn(p, L"設定を書き出す...", PX, PY + 166, 150, IDC_EXPORT);
    PushBtn(p, L"設定を読み込む...", PX + 160, PY + 166, 150, IDC_IMPORT);
    PushBtn(p, L"初期設定に戻す", PX + 405, PY + 166, 140, IDC_RESET);
    Label(p, L"書き出したファイルをほかの人に渡すと、相手は「設定を読み込む」で同じ設定にできます。", PX, PY + 202, 550);

    Label(p, L"ご注意", PX, PY + 238, 300, 20, true);
    Label(p, L"・管理者として実行しているアプリの上では効きません（PointerKit も管理者として起動すると効きます）。\n"
             L"・X-Mouse Button Control や W10Wheel など、同じ働きをするソフトとは同時に使わないでください。",
          PX, PY + 260, 550, 72);
    Label(p, L"使えるマウス", PX, PY + 334, 300, 20, true);
    Label(p, L"機種は問いません（左・右・中・戻る・進むの5ボタンに対応）。専用ソフトで別の動作に割り当てているボタンは、標準の動作に戻してからお使いください。",
          PX, PY + 356, 550, 40);
    Label(p, (std::wstring(L"PointerKit バージョン ") + POINTERKIT_VERSION).c_str(), PX, PY + 404, 300);
}

// ---------------------------------------------------------------------------
// 値の反映
// ---------------------------------------------------------------------------
static void UpdateKeyEnables() {
    for (int b = 0; b < B_COUNT; b++)
        EnableWindow(Item(IDC_BTN_KEY + b), GetSel(IDC_BTN_ACT + b) == A_KEYS);
    for (int i = 0; i < kMaxChords; i++) {
        bool used = GetSel(IDC_CH_HELD + i) > 0 && GetSel(IDC_CH_PRESS + i) > 0;
        EnableWindow(Item(IDC_CH_ACT + i), used);
        EnableWindow(Item(IDC_CH_KEY + i), used && GetSel(IDC_CH_ACT + i) == A_KEYS);
    }
    int trig = GetSel(IDC_TRIGGER);
    EnableWindow(Item(IDC_CLICKNOMOVE), trig != T_NONE && trig != T_LR);
    bool gOn = GetSel(IDC_G_BTN) > 0;
    for (int i = 0; i < kMaxGestures; i++) {
        bool used = gOn && GetSel(IDC_G_PAT + i) > 0;
        EnableWindow(Item(IDC_G_PAT + i), gOn);
        EnableWindow(Item(IDC_G_ACT + i), used);
        EnableWindow(Item(IDC_G_KEY + i), used && GetSel(IDC_G_ACT + i) == A_KEYS);
    }
    EnableWindow(Item(IDC_G_SHOW), gOn);
    EnableWindow(Item(IDC_G_FREEZE), gOn);
    EnableWindow(Item(IDC_G_DIST), gOn);
    EnableWindow(Item(IDC_G_DEFAULT), gOn);

    // 長押し・2回押し：スクロール・ジェスチャー・同時押しに使うボタンは対象外
    static const Btn kTrigBtn[T_COUNT] = { B_NONE, B_RIGHT, B_MIDDLE, B_X1, B_X2, B_LEFT, B_NONE };
    Btn tb = (trig >= 0 && trig < T_COUNT) ? kTrigBtn[trig] : B_NONE;
    Btn gb = kGBtnValues[std::max(0, std::min(4, GetSel(IDC_G_BTN)))];
    for (int b = 0; b < B_COUNT; b++) {
        bool ok = b != tb && b != gb && !(trig == T_LR && (b == B_LEFT || b == B_RIGHT));
        for (int i = 0; i < kMaxChords; i++)
            if (GetSel(IDC_CH_HELD + i) - 1 == b && GetSel(IDC_CH_PRESS + i) > 0) ok = false;
        EnableWindow(Item(IDC_LP_ACT + b), ok);
        EnableWindow(Item(IDC_DC_ACT + b), ok);
        EnableWindow(Item(IDC_LP_KEY + b), ok && GetSel(IDC_LP_ACT + b) == A_KEYS);
        EnableWindow(Item(IDC_DC_KEY + b), ok && GetSel(IDC_DC_ACT + b) == A_KEYS);
    }
    for (int i = 0; i < 4; i++) EnableWindow(Item(IDC_CN_KEY + i), GetSel(IDC_CN_ACT + i) == A_KEYS);
    EnableWindow(Item(IDC_INERTIA_LEN), GetCheck(IDC_INERTIA));
    for (size_t i = 0; i < g_kbIds.size(); i++) EnableWindow(Item(IDC_US_KB + (int)i), GetSel(IDC_US_MODE) == 1);
}

static int PatternIndex(const std::wstring& p) {
    for (int k = 0; k < kGesturePatternCount; k++) if (p == kGesturePatterns[k]) return k + 1;
    return 0;
}
static void LoadGesturesToUi(const std::vector<GestureDef>& gs) {
    for (int i = 0; i < kMaxGestures; i++) {
        if (i < (int)gs.size()) {
            SetSel(IDC_G_PAT + i, PatternIndex(gs[i].pattern));
            SetSel(IDC_G_ACT + i, gs[i].action.type);
            SetWindowTextW(Item(IDC_G_KEY + i), gs[i].action.type == A_KEYS ? gs[i].action.keys.c_str() : L"");
        } else {
            SetSel(IDC_G_PAT + i, 0);
            SetSel(IDC_G_ACT + i, A_KEYS);
            SetWindowTextW(Item(IDC_G_KEY + i), L"");
        }
    }
}
static void UpdateSpeedLabels() {
    for (auto pr : { std::make_pair(IDC_VSPEED, IDC_VSPEED_LBL), std::make_pair(IDC_HSPEED, IDC_HSPEED_LBL) }) {
        int v = (int)SendMessageW(Item(pr.first), TBM_GETPOS, 0, 0);
        wchar_t b[32]; swprintf(b, 32, L"%d%%", v);
        SetWindowTextW(Item(pr.second), b);
    }
}

static void LoadToUi(const Config& c) {
    for (int b = 0; b < B_COUNT; b++) {
        SetSel(IDC_BTN_ACT + b, c.button[b].type);
        SetWindowTextW(Item(IDC_BTN_KEY + b), c.button[b].type == A_KEYS ? c.button[b].keys.c_str() : L"");
    }
    for (int i = 0; i < kMaxChords; i++) {
        if (i < (int)c.chords.size()) {
            const ChordDef& ch = c.chords[i];
            SetSel(IDC_CH_HELD + i, ch.held + 1);
            SetSel(IDC_CH_PRESS + i, ch.pressed + 1);
            SetSel(IDC_CH_ACT + i, ch.action.type);
            SetWindowTextW(Item(IDC_CH_KEY + i), ch.action.type == A_KEYS ? ch.action.keys.c_str() : L"");
        } else {
            SetSel(IDC_CH_HELD + i, 0);
            SetSel(IDC_CH_PRESS + i, 0);
            SetSel(IDC_CH_ACT + i, A_KEYS);
            SetWindowTextW(Item(IDC_CH_KEY + i), L"");
        }
    }
    SetNum(IDC_CH_DELAY, c.chordDelay);
    SetNum(IDC_CH_MOVE, c.chordMove);

    SetSel(IDC_TRIGGER, c.trigger);
    SetCheck(IDC_CLICKNOMOVE, c.clickIfNoMove);
    SendMessageW(Item(IDC_VSPEED), TBM_SETPOS, TRUE, c.vSpeed);
    SendMessageW(Item(IDC_HSPEED), TBM_SETPOS, TRUE, c.hSpeed);
    SetSel(IDC_ACCEL, c.accel);
    SetCheck(IDC_HORIZ, c.horizontal);
    SetCheck(IDC_REVERSE, c.reverse);
    SetCheck(IDC_HREVERSE, c.hReverse);
    SetCheck(IDC_SWAP, c.swapAxis);
    SetCheck(IDC_AXISLOCK, c.axisLock);
    SetCheck(IDC_CURSOR, c.cursorChange);
    SetCheck(IDC_REALWHEEL, c.realWheel);
    SetNum(IDC_DRAGTH, c.dragThreshold);
    SetNum(IDC_HTH, c.hThreshold);
    SetNum(IDC_VTH, c.vThreshold);

    int gsel = 0;
    for (int i = 0; i < 5; i++) if (kGBtnValues[i] == c.gestureBtn) gsel = i;
    SetSel(IDC_G_BTN, gsel);
    SetCheck(IDC_G_SHOW, c.gestureShow);
    SetCheck(IDC_G_FREEZE, c.gestureFreeze);
    SetNum(IDC_G_DIST, c.gestureDist);
    LoadGesturesToUi(c.gestures);

    SetCheck(IDC_TP_EN, c.tpEnabled);
    SetCheck(IDC_TP_FREEZE, c.tpFreeze);
    for (int i = 0; i < 4; i++) SetNum(IDC_TP_W + i, c.tpEdgeWidth[i]);
    SetNum(IDC_TP_STEP, c.tpStep);
    for (int i = 0; i < 4; i++) SetSel(IDC_TP_ACT + i, c.tpAct[i]);

    for (int b = 0; b < B_COUNT; b++) {
        SetSel(IDC_LP_ACT + b, c.longPress[b].type);
        SetWindowTextW(Item(IDC_LP_KEY + b), c.longPress[b].type == A_KEYS ? c.longPress[b].keys.c_str() : L"");
        SetSel(IDC_DC_ACT + b, c.dblClick[b].type);
        SetWindowTextW(Item(IDC_DC_KEY + b), c.dblClick[b].type == A_KEYS ? c.dblClick[b].keys.c_str() : L"");
    }
    SetNum(IDC_LP_MS, c.longMs);
    SetNum(IDC_DC_MS, c.dblMs);
    SetCheck(IDC_INERTIA, c.inertia);
    SetNum(IDC_INERTIA_LEN, c.inertiaLen);
    for (int i = 0; i < 4; i++) {
        SetSel(IDC_CN_ACT + i, c.corner[i].type);
        SetWindowTextW(Item(IDC_CN_KEY + i), c.corner[i].type == A_KEYS ? c.corner[i].keys.c_str() : L"");
    }
    SetNum(IDC_CN_DELAY, c.cornerDelay);
    SetCheck(IDC_CN_FULL, c.cornerSkipFull);
    SetSel(IDC_THEME, c.theme);
    SetSel(IDC_US_MODE, c.usMode);
    for (size_t i = 0; i < g_kbIds.size(); i++)
        SetCheck(IDC_US_KB + (int)i, std::find(c.usKeyboards.begin(), c.usKeyboards.end(), g_kbIds[i]) != c.usKeyboards.end());
    SetWindowTextW(Item(IDC_US_HOT), c.usHotkey.c_str());
    SetCheck(IDC_US_CAPS, c.usCaps);
    UpdateUsStatus();

    SetCheck(IDC_ENABLED, c.enabled);
    SetCheck(IDC_STARTUP, IsStartup());
    UpdateKeyEnables();
    UpdateSpeedLabels();
}

// 画面から Config を作る。問題があれば error に説明を入れて false
static bool ReadPages(Config& c, std::wstring& error) {
    c = ViewConfig();
    for (int b = 0; b < B_COUNT; b++) {
        c.button[b].type = (ActType)std::max(0, GetSel(IDC_BTN_ACT + b));
        c.button[b].keys = GetText(IDC_BTN_KEY + b);
        if (c.button[b].type == A_KEYS && ParseKeys(c.button[b].keys).empty()) {
            error = std::wstring(L"「") + kBtnLabels[b] + L"」のキーが入っていません。キー欄をクリックして、割り当てたいキーを押してください。";
            return false;
        }
    }
    c.chords.clear();
    for (int i = 0; i < kMaxChords; i++) {
        int h = GetSel(IDC_CH_HELD + i) - 1, pr = GetSel(IDC_CH_PRESS + i) - 1;
        if (h < 0 || pr < 0) continue;
        if (h == pr) { error = L"同時押しで、同じボタン同士は組み合わせられません（" + std::to_wstring(i + 1) + L"行目）。"; return false; }
        ChordDef ch;
        ch.held = (Btn)h; ch.pressed = (Btn)pr;
        ch.action.type = (ActType)std::max(0, GetSel(IDC_CH_ACT + i));
        ch.action.keys = GetText(IDC_CH_KEY + i);
        if (ch.action.type == A_KEYS && ParseKeys(ch.action.keys).empty()) {
            error = L"同時押しの " + std::to_wstring(i + 1) + L" 行目のキーが入っていません。";
            return false;
        }
        c.chords.push_back(ch);
    }
    c.chordDelay = std::max(50, std::min(5000, GetNum(IDC_CH_DELAY, 750)));
    c.chordMove = std::max(0, std::min(100, GetNum(IDC_CH_MOVE, 5)));

    c.trigger = (Trigger)std::max(0, GetSel(IDC_TRIGGER));
    c.clickIfNoMove = GetCheck(IDC_CLICKNOMOVE);
    c.vSpeed = (int)SendMessageW(Item(IDC_VSPEED), TBM_GETPOS, 0, 0);
    c.hSpeed = (int)SendMessageW(Item(IDC_HSPEED), TBM_GETPOS, 0, 0);
    c.accel = std::max(0, GetSel(IDC_ACCEL));
    c.horizontal = GetCheck(IDC_HORIZ);
    c.reverse = GetCheck(IDC_REVERSE);
    c.hReverse = GetCheck(IDC_HREVERSE);
    c.swapAxis = GetCheck(IDC_SWAP);
    c.axisLock = GetCheck(IDC_AXISLOCK);
    c.cursorChange = GetCheck(IDC_CURSOR);
    c.realWheel = GetCheck(IDC_REALWHEEL);
    c.dragThreshold = std::max(0, std::min(100, GetNum(IDC_DRAGTH, 5)));
    c.hThreshold = std::max(0, std::min(200, GetNum(IDC_HTH, 10)));
    c.vThreshold = std::max(0, std::min(200, GetNum(IDC_VTH, 0)));
    c.enabled = GetCheck(IDC_ENABLED);

    auto readAct = [&](int actId, int keyId, Action& a, const std::wstring& where) -> bool {
        a.type = (ActType)std::max(0, GetSel(actId));
        a.keys = GetText(keyId);
        if (a.type == A_KEYS && ParseKeys(a.keys).empty()) {
            error = where + L"のキーが入っていません。キー欄をクリックして、割り当てたいキーを押してください。";
            return false;
        }
        return true;
    };
    for (int b = 0; b < B_COUNT; b++) {
        if (!readAct(IDC_LP_ACT + b, IDC_LP_KEY + b, c.longPress[b], std::wstring(L"「") + kBtnShort[b] + L"」ボタンの長押し")) return false;
        if (!readAct(IDC_DC_ACT + b, IDC_DC_KEY + b, c.dblClick[b], std::wstring(L"「") + kBtnShort[b] + L"」ボタンの2回押し")) return false;
    }
    c.longMs = std::max(150, std::min(3000, GetNum(IDC_LP_MS, 500)));
    c.dblMs = std::max(100, std::min(1000, GetNum(IDC_DC_MS, 300)));
    c.inertia = GetCheck(IDC_INERTIA);
    c.inertiaLen = std::max(1, std::min(10, GetNum(IDC_INERTIA_LEN, 5)));
    for (int i = 0; i < 4; i++)
        if (!readAct(IDC_CN_ACT + i, IDC_CN_KEY + i, c.corner[i], std::wstring(L"画面の") + kCornerLabels[i])) return false;
    c.cornerDelay = std::max(0, std::min(3000, GetNum(IDC_CN_DELAY, 300)));
    c.cornerSkipFull = GetCheck(IDC_CN_FULL);
    c.theme = std::max(0, std::min(2, GetSel(IDC_THEME)));
    c.usMode = std::max(0, std::min(2, GetSel(IDC_US_MODE)));
    c.usKeyboards.clear();
    for (size_t i = 0; i < g_kbIds.size(); i++) if (GetCheck(IDC_US_KB + (int)i)) c.usKeyboards.push_back(g_kbIds[i]);
    c.usHotkey = GetText(IDC_US_HOT);
    if (!c.usHotkey.empty()) {
        bool hasKey = false, hasMod = false;
        for (WORD k : ParseKeys(c.usHotkey)) {
            if (k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU || k == VK_LWIN) hasMod = true; else hasKey = true;
        }
        if (!hasKey || !hasMod) {
            error = L"US配列の切り替えショートカットは、Ctrl や Alt と普通のキーの組み合わせにしてください（例：Ctrl+Alt+U）。";
            return false;
        }
    }
    c.usCaps = GetCheck(IDC_US_CAPS);

    c.tpEnabled = GetCheck(IDC_TP_EN);
    c.tpFreeze = GetCheck(IDC_TP_FREEZE);
    for (int i = 0; i < 4; i++) c.tpEdgeWidth[i] = std::max(2, std::min(40, GetNum(IDC_TP_W + i, 8)));
    c.tpStep = std::max(1, std::min(50, GetNum(IDC_TP_STEP, 5)));
    for (int i = 0; i < 4; i++) c.tpAct[i] = (EdgeAct)std::max(0, GetSel(IDC_TP_ACT + i));

    c.gestureBtn = kGBtnValues[std::max(0, std::min(4, GetSel(IDC_G_BTN)))];
    c.gestureShow = GetCheck(IDC_G_SHOW);
    c.gestureFreeze = GetCheck(IDC_G_FREEZE);
    c.gestureDist = std::max(5, std::min(300, GetNum(IDC_G_DIST, 30)));
    c.gestures.clear();
    for (int i = 0; i < kMaxGestures; i++) {
        int pi = GetSel(IDC_G_PAT + i);
        if (pi <= 0) continue;
        GestureDef g;
        g.pattern = kGesturePatterns[pi - 1];
        g.action.type = (ActType)std::max(0, GetSel(IDC_G_ACT + i));
        g.action.keys = GetText(IDC_G_KEY + i);
        for (auto& o : c.gestures)
            if (o.pattern == g.pattern) {
                error = L"ジェスチャーの「" + GestureArrows(g.pattern) + L"」が2回登録されています。";
                return false;
            }
        if (c.gestureBtn != B_NONE && g.action.type == A_KEYS && ParseKeys(g.action.keys).empty()) {
            error = L"ジェスチャーの " + std::to_wstring(i + 1) + L" 行目のキーが入っていません。";
            return false;
        }
        c.gestures.push_back(g);
    }
    if (c.gestureBtn != B_NONE) {
        Btn tb = c.trigger == T_RIGHT ? B_RIGHT : c.trigger == T_MIDDLE ? B_MIDDLE : c.trigger == T_X1 ? B_X1
               : c.trigger == T_X2 ? B_X2 : c.trigger == T_LEFT ? B_LEFT : B_NONE;
        if (tb == c.gestureBtn || (c.trigger == T_LR && c.gestureBtn == B_RIGHT)) {
            error = L"ジェスチャーに使うボタンが、スクロールに使うボタンと同じです。どちらかを別のボタンにしてください。";
            return false;
        }
    }

    // 左ボタンを無効化すると操作できなくなるので確認
    if (c.button[B_LEFT].type == A_DISABLE) {
        error = L"左ボタンを「無効にする」にすると、マウスでの操作ができなくなります。別の動作を選んでください。";
        return false;
    }
    return true;
}

// 画面の内容を g_ui に取り込んだ「全体の設定」を作る
static bool ReadFromUi(Config& c, std::wstring& error) {
    Config t;
    if (!ReadPages(t, error)) {
        if (g_editSlot >= 0 && g_editSlot < (int)g_ui.profiles.size()) error = L"［" + g_ui.profiles[g_editSlot].name + L"］" + error;
        return false;
    }
    t.profileOsd = GetCheck(IDC_MS_OSD);
    if (g_editSlot < 0 || g_editSlot >= (int)g_ui.profiles.size()) {
        c = t;
        c.profiles = g_ui.profiles;
    } else {
        c = MergeMouse(t, g_ui);           // マウス以外の項目は画面のもの、マウスの項目は共通のまま
        c.profiles = g_ui.profiles;
        t.profiles.clear();
        c.profiles[g_editSlot].cfg = t;     // このマウスの設定
    }
    return true;
}
// 全体の設定を画面に出す（slot = どのマウスの設定を出すか）
static void LoadAll(const Config& c, int slot = -1) {
    g_ui = c;
    g_editSlot = slot >= 0 && slot < (int)g_ui.profiles.size() ? slot : -1;
    LoadToUi(ViewConfig());
    RefreshMiceUi();
}
// いまの画面の内容を g_ui に取り込む（失敗したらメッセージを出して false）
static bool CommitUi() {
    Config c; std::wstring err;
    if (!ReadFromUi(c, err)) { MessageBoxW(g_dlg, err.c_str(), L"PointerKit", MB_OK | MB_ICONWARNING); return false; }
    g_ui = c;
    return true;
}
static void OnProfileSelect() {
    int sel = (int)SendMessageW(Item(IDC_PROF_SEL), CB_GETCURSEL, 0, 0) - 1;
    if (sel == g_editSlot) return;
    if (!CommitUi()) { SendMessageW(Item(IDC_PROF_SEL), CB_SETCURSEL, g_editSlot + 1, 0); return; }
    LoadAll(g_ui, sel);
}
static void OnMouseToggle(int i) {
    if (i < 0 || i >= (int)g_msIds.size()) return;
    bool on = GetCheck(IDC_MS_USE + i);
    SetCheck(IDC_MS_USE + i, !on);   // 取り込みが済むまで元に戻しておく
    if (!CommitUi()) return;
    int idx = ProfileIndex(g_msIds[i]);
    if (on && idx < 0) {
        if (g_ui.profiles.size() >= 8) { MessageBoxW(g_dlg, L"専用の設定を使えるマウスは 8 台までです。", L"PointerKit", MB_OK | MB_ICONWARNING); RefreshMiceUi(); return; }
        MouseProfile pr;
        pr.id = g_msIds[i];
        pr.name = g_msNames[i];
        pr.cfg = g_ui;                 // いまの共通の設定をコピー
        pr.cfg.profiles.clear();
        g_ui.profiles.push_back(pr);
    } else if (!on && idx >= 0) {
        g_ui.profiles.erase(g_ui.profiles.begin() + idx);
        if (g_editSlot == idx) g_editSlot = -1;
        else if (g_editSlot > idx) g_editSlot--;
    }
    LoadAll(g_ui, g_editSlot);
}

static const UINT WM_REOPEN = WM_APP + 50;
static bool Apply() {
    Config c;
    std::wstring err;
    if (!ReadFromUi(c, err)) {
        MessageBoxW(g_dlg, err.c_str(), L"PointerKit", MB_OK | MB_ICONWARNING);
        return false;
    }
    g_ui = c;
    ApplyNewConfig(c);
    if (GetCheck(IDC_STARTUP) != IsStartup()) SetStartup(GetCheck(IDC_STARTUP));
    if (WantDark(c.theme) != g_dark) PostMessageW(g_dlg, WM_REOPEN, 0, 0);   // 色を変えたら開き直す
    return true;
}

static void LoadToUi(const Config& c);
void ShowSettings();
static void ShowPage(int page);
// 画面の色を切り替えるため、入力中の内容を保ったまま設定画面を作り直す
static void Reopen() {
    Config tmp;
    std::wstring err;
    bool ok = ReadFromUi(tmp, err);
    int page = g_curPage, slot = g_editSlot;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    GetWindowPlacement(g_dlg, &wp);
    DestroyWindow(g_dlg);
    ShowSettings();
    if (!g_dlg) return;
    if (ok) LoadAll(tmp, slot);
    ShowPage(page);
    SetWindowPlacement(g_dlg, &wp);
}

static bool FileDialog(bool save, wchar_t* file) {
    OPENFILENAMEW of = { sizeof(of) };
    of.hwndOwner = g_dlg;
    of.lpstrFilter = L"PointerKit の設定ファイル (*.ini)\0*.ini\0すべてのファイル (*.*)\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"ini";
    of.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
}

static RECT CardRect() {
    RECT c = { 0, 0, S(kWinW), S(kWinH) };
    if (g_dlg) GetClientRect(g_dlg, &c);
    RECT r = { S(PX - 20 + Off()), S(PY - 16), c.right - S(20), c.bottom - S(72) };
    return r;
}

// ウィンドウの大きさに合わせて、下のボタン類と左メニューを配置し直す
static void LayoutBottom() {
    if (!g_dlg) return;
    RECT c; GetClientRect(g_dlg, &c);
    int bw = S(110), bh = S(34), gap = S(8), y = c.bottom - S(54);
    int xApply = c.right - S(20) - bw;
    int xCancel = xApply - gap - bw;
    int xOk = xCancel - gap - bw;
    SetWindowPos(GetDlgItem(g_dlg, IDC_OK), nullptr, xOk, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(g_dlg, IDC_CANCEL), nullptr, xCancel, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(g_dlg, IDC_APPLY), nullptr, xApply, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_monLbl) {
        int x = S(PX - 20 + Off());
        SetWindowPos(g_monLbl, nullptr, x, y, std::max(S(40), xOk - S(12) - x), bh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_nav) SetWindowPos(g_nav, nullptr, 0, 0, S(NavW()), c.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    if (HWND pc = GetDlgItem(g_dlg, IDC_PROF_SEL)) {
        int w = S(240), x = c.right - S(20) - w;
        SetWindowPos(pc, nullptr, x, S(34), w, S(300), SWP_NOZORDER | SWP_NOACTIVATE);
        if (HWND pl = GetDlgItem(g_dlg, IDC_PROF_LBL)) SetWindowPos(pl, nullptr, x - S(112), S(37), S(108), S(20), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(g_dlg, nullptr, TRUE);
}

static void ShowPage(int page) {
    g_curPage = page;
    if (g_nav) InvalidateRect(g_nav, nullptr, FALSE);
    if (g_dlg) { RECT t; GetClientRect(g_dlg, &t); t.left = S(NavW()); t.bottom = S(PY - 16); InvalidateRect(g_dlg, &t, TRUE); }
    for (int p = 0; p < kPages; p++)
        for (HWND h : g_pages[p]) ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
    UpdateProfileVisibility();
    if (page == P_MICE) UpdateLastMouse();
}

// ---------------------------------------------------------------------------
// 左のナビゲーション（Windows 11 の「設定」アプリ風）
// ---------------------------------------------------------------------------
static const int kNavTop = 76, kNavItemH = 40;

static void SetNavCollapsed(bool collapsed) {
    if (collapsed == g_navCollapsed) return;
    int before = NavW();
    g_navCollapsed = collapsed;
    int dx = S(NavW() - before);
    // ページの項目をまとめて左右にずらす
    for (auto& page : g_pages)
        for (HWND c : page) {
            RECT r; GetWindowRect(c, &r);
            MapWindowPoints(nullptr, g_dlg, (POINT*)&r, 2);
            SetWindowPos(c, nullptr, r.left + dx, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    LayoutBottom();
    if (g_nav) InvalidateRect(g_nav, nullptr, FALSE);
    // 次回も同じ状態で開く
    WritePrivateProfileStringW(L"UI", L"NavCollapsed", collapsed ? L"1" : L"0", g_iniPath.c_str());
    g_iniTime = GetIniTime();
}

static int NavHit(int y) {
    int i = (y - S(kNavTop)) / S(kNavItemH);
    if (y < S(kNavTop) || i < 0 || i >= kPages) return -1;
    return i;
}

static LRESULT CALLBACK NavProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bmp);
        FillRect(dc, &rc, g_winBrush);
        SetBkMode(dc, TRANSPARENT);

        // 「≡」ボタン（メニューを細く／元に戻す）
        RECT hb = { S(8), S(16), S(48), S(56) };
        if (g_navHover == -2) FillRound(dc, hb, S(8), kNavSel, kNavSel);
        SetTextColor(dc, kText);
        HGDIOBJ of = SelectObject(dc, g_fontIcon);
        RECT hbt = { S(22), hb.top, S(48), hb.bottom };
        DrawTextW(dc, L"\uE700", -1, &hbt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        // アプリ名（広げているときだけ）
        if (!g_navCollapsed) {
            HICON ic = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(20), S(20), 0);
            DrawIconEx(dc, S(58), S(26), ic, S(20), S(20), 0, nullptr, DI_NORMAL);
            DestroyIcon(ic);
            RECT tr = { S(86), S(16), rc.right, S(56) };
            SelectObject(dc, g_fontBold);
            DrawTextW(dc, L"PointerKit", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        for (int i = 0; i < kPages; i++) {
            RECT ir = { S(8), S(kNavTop + i * kNavItemH) + S(2), rc.right - S(8), S(kNavTop + (i + 1) * kNavItemH) - S(2) };
            if (i == g_curPage) FillRound(dc, ir, S(8), kNavSel, kNavSel);
            else if (i == g_navHover) FillRound(dc, ir, S(8), kNavHover, kNavHover);
            if (i == g_curPage) {   // 選択中の印（左の縦線）
                RECT bar = { ir.left, ir.top + S(10), ir.left + S(3), ir.bottom - S(10) };
                FillRound(dc, bar, S(3), g_accent, g_accent);
            }
            RECT icr = { ir.left + S(14), ir.top, ir.left + S(40), ir.bottom };
            SelectObject(dc, g_fontIcon);
            SetTextColor(dc, kText);
            DrawTextW(dc, kPageIcons[i], -1, &icr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (!g_navCollapsed) {
                RECT txr = { ir.left + S(46), ir.top, ir.right, ir.bottom };
                SelectObject(dc, g_fontNav);
                DrawTextW(dc, kPageTitles[i], -1, &txr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }
        SelectObject(dc, of);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int i = NavHit(HIWORD(lp));
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x >= S(8) && x < S(48) && y >= S(16) && y < S(56)) i = -2;
        if (i != g_navHover) { g_navHover = i; InvalidateRect(h, nullptr, FALSE); }
        // 細くしているときは、ページ名をツールチップ代わりにタイトルバーへ出す必要はない（見出しに出ている）
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_navHover = -1; InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x >= S(8) && x < S(48) && y >= S(16) && y < S(56)) { SetNavCollapsed(!g_navCollapsed); return 0; }
        int i = NavHit(y);
        if (i >= 0) ShowPage(i);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, g_navHover != -1 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, g_winBrush);
        // ページの見出し
        RECT tr = { S(PX - 20 + Off()), S(22), rc.right, S(PY - 22) };
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        HGDIOBJ of = SelectObject(dc, g_fontTitle);
        DrawTextW(dc, kPageTitles[g_curPage], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        // 内容のカード
        FillRound(dc, CardRect(), S(8), kCardBg, kCardBd);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        LayoutBottom();
        return 0;
    case WM_GETMINMAXINFO: {
        // これより小さくすると項目が隠れるので、最初の大きさを最小にする
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        RECT r = { 0, 0, S(kWinW + Off()), S(kWinH) };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_HSCROLL:
        UpdateSpeedLabels();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if ((code == BN_CLICKED || code == BN_DOUBLECLICKED) && g_toggles.count((HWND)lp)) {
            SetCheck(id, !GetCheck(id));
            code = BN_CLICKED;
            if (id >= IDC_MS_USE && id < IDC_MS_USE + kMaxMice) { OnMouseToggle(id - IDC_MS_USE); return 0; }
            UpdateKeyEnables();
            if (id == IDC_TP_EN) InvalidateRect(Item(IDC_TP_PREVIEW), nullptr, FALSE);
            return 0;
        }
        if (code == CBN_SELCHANGE && id == IDC_PROF_SEL) { OnProfileSelect(); return 0; }
        if (code == CBN_SELCHANGE || (code == BN_CLICKED && id == IDC_TP_EN) || (code == EN_CHANGE && id >= IDC_TP_W && id < IDC_TP_W + 4)) {
            UpdateKeyEnables();
            if (Item(IDC_TP_PREVIEW)) InvalidateRect(Item(IDC_TP_PREVIEW), nullptr, FALSE);
            return 0;
        }
        switch (id) {
        case IDC_OK: if (Apply()) DestroyWindow(h); return 0;
        case IDC_APPLY: Apply(); return 0;
        case IDC_CANCEL: case IDCANCEL: DestroyWindow(h); return 0;
        case IDC_G_DEFAULT: {
            Config def;
            ResetGesturesToDefault(def);
            LoadGesturesToUi(def.gestures);
            UpdateKeyEnables();
            return 0;
        }
        case IDC_OPENFOLDER: {
            std::wstring arg = L"/select,\"" + g_iniPath + L"\"";
            ShellExecuteW(h, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDC_EXPORT: {
            Config c; std::wstring err;
            if (!ReadFromUi(c, err)) { MessageBoxW(h, err.c_str(), L"PointerKit", MB_OK | MB_ICONWARNING); return 0; }
            wchar_t file[MAX_PATH] = L"PointerKit設定.ini";
            if (!FileDialog(true, file)) return 0;
            if (!_wcsicmp(file, g_iniPath.c_str())) {
                MessageBoxW(h, L"いま使っている設定ファイルと同じ場所です。別の名前か場所を選んでください。", L"PointerKit", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (ExportConfig(c, file))
                MessageBoxW(h, (std::wstring(L"設定を書き出しました。\n") + file).c_str(), L"PointerKit", MB_OK | MB_ICONINFORMATION);
            else MessageBoxW(h, L"書き出せませんでした。別の場所を選んでください。", L"PointerKit", MB_OK | MB_ICONWARNING);
            return 0;
        }
        case IDC_IMPORT: {
            wchar_t file[MAX_PATH] = L"";
            if (!FileDialog(false, file)) return 0;
            Config c;
            if (!ImportConfig(file, c)) {
                MessageBoxW(h, L"PointerKit の設定ファイルではないようです。", L"PointerKit", MB_OK | MB_ICONWARNING);
                return 0;
            }
            LoadAll(c);
            MessageBoxW(h, L"設定を読み込みました。\n「OK」または「適用」を押すと反映されます。", L"PointerKit", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        case IDC_RESET:
            if (MessageBoxW(h, L"すべての設定を初期状態に戻します。よろしいですか？\n（「OK」または「適用」を押すまで保存はされません）",
                            L"PointerKit", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                Config def;
                ResetGesturesToDefault(def);
                LoadAll(def);
            }
            return 0;
        }
        return 0;
    }
    case WM_TPMON: {
        g_tpX = LOWORD(wp); g_tpY = HIWORD(wp);
        g_tpFingers = LOWORD(lp); g_tpEdge = (int)HIWORD(lp) - 1;
        InvalidateRect(Item(IDC_TP_PREVIEW), nullptr, FALSE);
        wchar_t b[128];
        if (g_tpFingers > 0)
            swprintf(b, 128, L"指 %d 本　横 %d%%・縦 %d%%%ls%ls", g_tpFingers, g_tpX / 10, g_tpY / 10,
                     g_tpEdge >= 0 ? L"\n操作中：" : L"", g_tpEdge >= 0 ? kEdgeLabels[g_tpEdge] : L"");
        else swprintf(b, 128, L"指は離れています");
        SetWindowTextW(Item(IDC_TP_LIVE), b);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->CtlID == IDC_TP_PREVIEW) { DrawTouchpadPreview(di); return TRUE; }
        if (g_toggles.count(di->hwndItem)) { DrawToggle(di); return TRUE; }
        if (g_pushBtns.count(di->hwndItem)) { DrawPushButton(di, false, true); return TRUE; }
        if (di->CtlID == IDC_OK || di->CtlID == IDC_CANCEL || di->CtlID == IDC_APPLY) {
            DrawPushButton(di, di->CtlID == IDC_OK);
            return TRUE;
        }
        return 0;
    }
    case WM_INPUTMON: {
        int code = (int)wp;
        bool dn = lp != 0;
        std::wstring t;
        if (code >= 0 && code < B_COUNT) {
            g_pressed[code] = dn;
            InvalidateRect(g_btnLbl[code], nullptr, TRUE);
            if (!dn) return 0;
            t = std::wstring(L"押したボタン：") + kBtnLabels[code];
        } else {
            static const wchar_t* w[] = { L"ホイール：上", L"ホイール：下", L"ホイール：左（チルト）", L"ホイール：右（チルト）" };
            t = w[std::min(3, std::max(0, code - 10))];
        }
        SetWindowTextW(g_monLbl, t.c_str());
        InvalidateRect(g_monLbl, nullptr, TRUE);
        KillTimer(h, TIMER_WHEELMSG);
        SetTimer(h, TIMER_WHEELMSG, 3000, nullptr);   // 3秒で案内表示に戻す
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_USSTATUS) { UpdateUsStatus(); if (g_curPage == P_MICE) UpdateLastMouse(); return 0; }
        if (wp == TIMER_WHEELMSG) {
            KillTimer(h, TIMER_WHEELMSG);
            SetWindowTextW(g_monLbl, L"マウスのボタンを押すと、ここに表示されます");
            InvalidateRect(g_monLbl, nullptr, TRUE);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        for (int b = 0; b < B_COUNT; b++)
            if (ctl == g_btnLbl[b] && g_pressed[b]) {
                SetBkColor(dc, kHiColor);
                SetTextColor(dc, RGB(0, 0, 0));
                return (LRESULT)g_hiBrush;
            }
        if (ctl == g_monLbl) {
            SetBkColor(dc, kWinBg);
            SetTextColor(dc, g_accent);
            return (LRESULT)g_winBrush;
        }
        if (ctl == GetDlgItem(g_dlg, IDC_PROF_LBL)) {
            SetBkColor(dc, kWinBg);
            SetTextColor(dc, kText);
            return (LRESULT)g_winBrush;
        }
        SetBkColor(dc, kCardBg);
        SetTextColor(dc, IsWindowEnabled(ctl) ? kText : kDisText);
        return (LRESULT)g_cardBrush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, kEditBg);
        SetTextColor(dc, kText);
        return (LRESULT)g_editBrush;
    }
    case WM_REOPEN:
        Reopen();
        return 0;
    case WM_SETTINGCHANGE:
        // Windows のライト／ダークが切り替わったら追従する
        if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") && GetSel(IDC_THEME) == 0 && SystemIsDark() != g_dark)
            PostMessageW(h, WM_REOPEN, 0, 0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        SetInputMonitor(nullptr);
        SetTouchpadMonitor(nullptr);
        for (int b = 0; b < B_COUNT; b++) { g_btnLbl[b] = nullptr; g_pressed[b] = false; }
        g_monLbl = nullptr;
        g_dlg = nullptr;
        for (auto& v : g_pages) v.clear();
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }
        for (HFONT* f : { &g_fontTitle, &g_fontIcon, &g_fontNav }) if (*f) { DeleteObject(*f); *f = nullptr; }
        g_toggles.clear();
        g_pushBtns.clear();
        g_nav = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool IsSettingsDialogMessage(MSG* m) {
    return g_dlg && IsDialogMessageW(g_dlg, m);
}

typedef UINT (WINAPI *PFN_GetDpiForSystem)();

void ShowSettings() {
    if (g_dlg) {
        ShowWindow(g_dlg, SW_SHOWNORMAL);
        SetForegroundWindow(g_dlg);
        return;
    }
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"PointerKitSettings";
        wc.hIcon = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        RegisterClassExW(&wc);
        registered = true;
    }

    // 表示するモニターの DPI に合わせる
    g_dpi = 96;
    if (auto f = (PFN_GetDpiForSystem)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")) g_dpi = (int)f();
    POINT cp; GetCursorPos(&cp);
    typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll"))
        if (auto f = (PFN_GetDpiForMonitor)GetProcAddress(sh, "GetDpiForMonitor")) {
            UINT dx = 96, dy = 96;
            if (SUCCEEDED(f(MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST), 0, &dx, &dy))) g_dpi = (int)dx;
        }

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(9, g_dpi, 72);
    lf.lfQuality = CLEARTYPE_QUALITY;
    g_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_NORMAL;
    lf.lfHeight = -MulDiv(10, g_dpi, 72);
    g_fontNav = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = -MulDiv(20, g_dpi, 72);
    g_fontTitle = CreateFontIndirectW(&lf);
    // アイコン用フォント（Windows 11 は Segoe Fluent Icons、10 は Segoe MDL2 Assets）
    {
        LOGFONTW il = {};
        il.lfHeight = -MulDiv(12, g_dpi, 72);
        il.lfCharSet = DEFAULT_CHARSET;
        il.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(il.lfFaceName, L"Segoe Fluent Icons");
        HDC sdc = GetDC(nullptr);
        bool found = false;
        EnumFontFamiliesExW(sdc, &il, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *(bool*)p = true; return 0; }, (LPARAM)&found, 0);
        ReleaseDC(nullptr, sdc);
        if (!found) wcscpy_s(il.lfFaceName, L"Segoe MDL2 Assets");
        g_fontIcon = CreateFontIndirectW(&il);
    }

    // Windows のアクセントカラー
    {
        typedef HRESULT (WINAPI *PFN_Colorization)(DWORD*, BOOL*);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        DWORD col = 0; BOOL opaque = FALSE;
        g_accent = RGB(0, 95, 184);
        if (dwm) if (auto f = (PFN_Colorization)GetProcAddress(dwm, "DwmGetColorizationColor"))
            if (SUCCEEDED(f(&col, &opaque))) {
                COLORREF c = RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
                // 明るすぎる色は文字が読めないので既定の青にする
                int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
                if (lum < 170) g_accent = c;
            }
    }
    // 画面の色（ライト／ダーク）
    SetPalette(WantDark(g_cfg.theme));
    SetAppDarkMode(g_dark);
    if (g_dark) {   // ダークでは文字が読めるよう、アクセントカラーを明るめにする
        g_accent = RGB(GetRValue(g_accent) + (255 - GetRValue(g_accent)) * 45 / 100,
                       GetGValue(g_accent) + (255 - GetGValue(g_accent)) * 45 / 100,
                       GetBValue(g_accent) + (255 - GetBValue(g_accent)) * 45 / 100);
    }

    const int W = kWinW, H = kWinH;
    RECT r = { 0, 0, S(W), S(H) };
    DWORD style = WS_OVERLAPPEDWINDOW;   // 大きさを変えられる・最大化できる
    AdjustWindowRectEx(&r, style, FALSE, 0);
    HMONITOR mon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;

    g_dlg = CreateWindowExW(WS_EX_CONTROLPARENT, L"PointerKitSettings", L"PointerKit の設定", style | WS_CLIPCHILDREN,
                            x, y, ww, wh, nullptr, nullptr, g_hinst, nullptr);

    // Windows 11：角丸ウィンドウ・タイトルバーを背景と同じ色に
    {
        typedef HRESULT (WINAPI *PFN_SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
            if (auto f = (PFN_SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
                int corner = 2;               // DWMWCP_ROUND
                f(g_dlg, 33, &corner, sizeof(corner));
                BOOL dk = g_dark;              // DWMWA_USE_IMMERSIVE_DARK_MODE
                f(g_dlg, 20, &dk, sizeof(dk));
                COLORREF cap = kWinBg;        // DWMWA_CAPTION_COLOR
                f(g_dlg, 35, &cap, sizeof(cap));
                COLORREF tc = kText;          // DWMWA_TEXT_COLOR
                f(g_dlg, 36, &tc, sizeof(tc));
            }
    }

    // 左メニュー
    {
        static bool navReg = false;
        if (!navReg) {
            WNDCLASSW nc = {};
            nc.lpfnWndProc = NavProc;
            nc.hInstance = g_hinst;
            nc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            nc.lpszClassName = L"PointerKitNav";
            RegisterClassW(&nc);
            navReg = true;
        }
        g_nav = CreateWindowExW(0, L"PointerKitNav", L"", WS_CHILD | WS_VISIBLE, 0, 0, S(kNavW), S(H),
                                g_dlg, nullptr, g_hinst, nullptr);
    }

    BuildButtonsPage();
    BuildTapPage();
    BuildChordsPage();
    BuildScrollPage();
    BuildGesturePage();
    BuildCornerPage();
    BuildMicePage();
    BuildTouchpadPage();
    BuildKeyboardPage();
    BuildGeneralPage();

    Make(-1, L"STATIC", L"設定するマウス", SS_RIGHT, 0, 0, 108, 20, IDC_PROF_LBL);
    Make(-1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 240, 300, IDC_PROF_SEL);
    Make(-1, L"BUTTON", L"OK", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 110, 34, IDC_OK);
    Make(-1, L"BUTTON", L"キャンセル", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 110, 34, IDC_CANCEL);
    Make(-1, L"BUTTON", L"適用", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 110, 34, IDC_APPLY);

    g_monLbl = Make(-1, L"STATIC", L"マウスのボタンを押すと、ここに表示されます", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                    0, 0, 200, 34, -1);
    LayoutBottom();
    if (!g_hiBrush) g_hiBrush = CreateSolidBrush(kHiColor);
    SetInputMonitor(g_dlg);
    SetTouchpadMonitor(g_dlg);
    SetTimer(g_dlg, TIMER_USSTATUS, 500, nullptr);

    // 前回、左メニューを細くしていたらその状態で開く
    g_navCollapsed = false;
    if (GetPrivateProfileIntW(L"UI", L"NavCollapsed", 0, g_iniPath.c_str())) {
        SetNavCollapsed(true);
        RECT wr; GetWindowRect(g_dlg, &wr);
        SetWindowPos(g_dlg, nullptr, 0, 0, (wr.right - wr.left) - S(220 - kNavCompact), wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    LoadAll(g_cfg);
    ShowPage(0);
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
}
