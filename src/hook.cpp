// マウスフック本体：ボタン割り当て・同時押し・ボールスクロール
#include "pointerkit.h"
#include <cmath>
#include <algorithm>

static const ULONG_PTR kMagic = 0x4F4E454DUL;   // 自分が送った入力の目印 ('ONEM')
static HHOOK g_hook = nullptr;
static HWND  g_hookWnd = nullptr;     // フック用スレッドのメッセージ窓（タイマー用）
static Config g_hc;
static HWND volatile g_monitor = nullptr;
static volatile DWORD g_lastTick = 0;
static volatile unsigned g_calls = 0, g_slow = 0;
static volatile double g_maxMs = 0;
static LARGE_INTEGER g_qpf = {};
DWORD HookLastTick() { return g_lastTick; }
void HookStats(unsigned* calls, unsigned* slow, double* maxMs) {
    *calls = g_calls; *slow = g_slow; *maxMs = g_maxMs;
    g_calls = 0; g_slow = 0; g_maxMs = 0;
}  // ボタン確認の通知先
void SetInputMonitor(HWND h) { g_monitor = h; }                    // フック用スレッドが使う設定のコピー
static const UINT WMH_SETCONFIG = WM_APP + 10;
static const UINT WMH_REINSTALL = WM_APP + 11;

// ---------------------------------------------------------------------------
// 入力送信
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 入力の送信は専用スレッドで行う。
// フックの中で SendInput を呼ぶと、送った入力が自分のフックに戻ってくるのを待って
// 処理が詰まり（v1.2 までのカクつき・スクロール停止の原因）、Windows にフックを外される。
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_qCs;
static std::vector<INPUT> g_queue;
static HANDLE g_qEvent = nullptr;
static HANDLE g_senderThread = nullptr;

static DWORD WINAPI SenderThread(LPVOID) {
    std::vector<INPUT> batch;
    for (;;) {
        WaitForSingleObject(g_qEvent, INFINITE);
        EnterCriticalSection(&g_qCs);
        batch.swap(g_queue);
        g_queue.clear();
        LeaveCriticalSection(&g_qCs);
        if (!batch.empty()) SendInput((UINT)batch.size(), batch.data(), sizeof(INPUT));
        batch.clear();
    }
    return 0;
}
static void StartSender() {
    if (g_senderThread) return;
    InitializeCriticalSection(&g_qCs);
    g_qEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_senderThread = CreateThread(nullptr, 0, SenderThread, nullptr, 0, nullptr);
    SetThreadPriority(g_senderThread, THREAD_PRIORITY_ABOVE_NORMAL);
}
static void QueueInput(const INPUT* in, size_t n) {
    EnterCriticalSection(&g_qCs);
    g_queue.insert(g_queue.end(), in, in + n);
    LeaveCriticalSection(&g_qCs);
    SetEvent(g_qEvent);
}

void QueueInputExt(const INPUT* in, size_t n) { QueueInput(in, n); }

static void SendMouseRaw(DWORD flags, DWORD data = 0) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    in.mi.dwExtraInfo = kMagic;
    QueueInput(&in, 1);
}
static void SendButton(Btn b, bool down) {
    switch (b) {
    case B_LEFT:   SendMouseRaw(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP); break;
    case B_RIGHT:  SendMouseRaw(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP); break;
    case B_MIDDLE: SendMouseRaw(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP); break;
    case B_X1:     SendMouseRaw(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON1); break;
    case B_X2:     SendMouseRaw(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON2); break;
    default: break;
    }
}
static void SendMoveAbs(POINT pt) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((double)(pt.x - vx) * 65535.0) / (vw - 1) + 0.5);
    in.mi.dy = (LONG)(((double)(pt.y - vy) * 65535.0) / (vh - 1) + 0.5);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo = kMagic;
    QueueInput(&in, 1);
}
static void SendWheel(int delta, bool horiz) {
    if (delta == 0) return;
    SendMouseRaw(horiz ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, (DWORD)delta);
}

static bool IsExtendedKey(WORD vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_LWIN: case VK_RWIN:
    case VK_SNAPSHOT: case VK_DIVIDE: case VK_NUMLOCK:
        return true;
    default:
        return vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2;
    }
}
static void SendKeys(const std::vector<WORD>& keys) {
    std::vector<INPUT> v;
    auto mk = [&](WORD vk, bool up) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        in.ki.dwExtraInfo = kMagic;
        v.push_back(in);
    };
    for (WORD k : keys) mk(k, false);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) mk(*it, true);
    QueueInput(v.data(), v.size());
}

static HWND TargetWindow(POINT pt) {
    HWND w = WindowFromPoint(pt);
    if (w) w = GetAncestor(w, GA_ROOT);
    if (!w || w == GetDesktopWindow() || w == GetShellWindow()) w = GetForegroundWindow();
    return w;
}

static void SendKeysStr(const std::wstring& k) { SendKeys(ParseKeys(k)); }

static Btn ClickBtnOf(ActType t) {
    switch (t) {
    case A_CLICK_LEFT: return B_LEFT;
    case A_CLICK_RIGHT: return B_RIGHT;
    case A_CLICK_MIDDLE: return B_MIDDLE;
    case A_BACK: return B_X1;
    case A_FORWARD: return B_X2;
    default: return B_NONE;
    }
}

// 「押した瞬間」に実行するアクション（down を握りつぶした場合に使う）
static void RunActionDown(const Action& a, POINT pt) {
    switch (a.type) {
    case A_KEYS: SendKeysStr(a.keys); break;
    case A_CLICK_LEFT: case A_CLICK_RIGHT: case A_CLICK_MIDDLE: case A_BACK: case A_FORWARD:
        SendButton(ClickBtnOf(a.type), true); break;
    case A_MINIMIZE: { HWND w = TargetWindow(pt); if (w) PostMessageW(w, WM_SYSCOMMAND, SC_MINIMIZE, 0); break; }
    case A_MAXIMIZE: {
        HWND w = TargetWindow(pt);
        if (w) PostMessageW(w, WM_SYSCOMMAND, IsZoomed(w) ? SC_RESTORE : SC_MAXIMIZE, 0);
        break;
    }
    case A_CLOSE: { HWND w = TargetWindow(pt); if (w) PostMessageW(w, WM_SYSCOMMAND, SC_CLOSE, 0); break; }
    case A_WHEELUP: SendWheel(WHEEL_DELTA, false); break;
    case A_WHEELDOWN: SendWheel(-WHEEL_DELTA, false); break;
    case A_WHEELLEFT: SendWheel(-WHEEL_DELTA, true); break;
    case A_WHEELRIGHT: SendWheel(WHEEL_DELTA, true); break;
    case A_TOGGLE: PostMessageW(g_hwnd, WM_COMMAND, ID_ENABLE, 0); break;
    default: break;
    }
}
static void RunActionUp(const Action& a) {
    Btn b = ClickBtnOf(a.type);
    if (b != B_NONE) SendButton(b, false);
}
// クリック1回分（押す→離す）を実行
// ---------------------------------------------------------------------------
// マウスジェスチャー
// ---------------------------------------------------------------------------
static POINT g_gStart = {}, g_gAnchor = {};
static POINT g_gVirt = {};   // カーソルを止めているときの「仮想の」位置
static std::wstring g_gPattern;
static std::wstring g_gLastOsd;

static const GestureDef* FindGesture(const std::wstring& p);
static void GestureOsd(const std::wstring& text) {
    if (!g_hc.gestureShow) return;
    if (text == g_gLastOsd) return;
    g_gLastOsd = text;
    PostMessageW(g_hwnd, WM_GESTUREOSD, 0, (LPARAM)new std::wstring(text));
}
static void GestureMove(POINT pt) {
    int dx = pt.x - g_gAnchor.x, dy = pt.y - g_gAnchor.y;
    if (std::abs(dx) < g_hc.gestureDist && std::abs(dy) < g_hc.gestureDist) return;
    wchar_t dir = std::abs(dx) >= std::abs(dy) ? (dx > 0 ? L'R' : L'L') : (dy > 0 ? L'D' : L'U');
    g_gAnchor = pt;
    if (!g_gPattern.empty() && g_gPattern.back() == dir) return;
    if (g_gPattern.size() >= 6) return;
    g_gPattern += dir;
    const GestureDef* g = FindGesture(g_gPattern);
    GestureOsd(GestureArrows(g_gPattern) + L"   " + (g ? ActionDescription(g->action) : std::wstring(L"（割り当てなし）")));
}

static void RunActionClick(Btn b, POINT pt) {
    const Action& a = g_hc.button[b];
    if (a.type == A_PASS) { SendButton(b, true); SendButton(b, false); }
    else if (a.type != A_DISABLE) { RunActionDown(a, pt); RunActionUp(a); }
}

// ---------------------------------------------------------------------------
// フック状態
// ---------------------------------------------------------------------------
enum BtnState {
    S_IDLE,
    S_PASSED,     // down をそのまま通した → up も通す
    S_MAPPED,     // down を握りつぶしてアクション実行済み → up で後処理
    S_PENDING,    // 同時押し待ち / 左右同時押し待ち（down を保留中）
    S_CHORDED,    // 同時押しの「押さえ側」として使われた → up を握りつぶす
    S_SWALLOW,    // up を握りつぶすだけ
    S_TRIGGER,    // スクロールトリガー押下中
    S_LRSCROLL,   // 左右同時押しスクロール中
    S_GESTURE,    // ジェスチャー入力中
    S_DBLWAIT,    // 1回目のクリックを保留して、2回目を待っている
};
static BtnState g_state[B_COUNT] = {};
static DWORD g_pendTime[B_COUNT] = {};
static POINT g_pendPt[B_COUNT] = {};
static bool  g_pendLR[B_COUNT] = {};
static bool  g_pendTap[B_COUNT] = {};  // 長押し・2回押しの判定のために保留中
static DWORD g_dblTime[B_COUNT] = {};
static POINT g_dblPt[B_COUNT] = {};
static const UINT_PTR TIMER_INERTIA = 5, TIMER_CORNER = 6;

static bool  g_scrolling = false;      // スクロールモード（カーソル固定）
static bool  g_scrollStarted = false;  // しきい値を超えて動いたか
static Btn   g_scrollOwner = B_NONE;
static POINT g_anchor = {};
static double g_accV = 0, g_accH = 0;
static int   g_lockAxis = 0;           // 0=未定 1=縦 2=横
static int   g_cursorShape = 0;

static Btn MsgToBtn(WPARAM msg, DWORD mouseData, bool* down) {
    switch (msg) {
    case WM_LBUTTONDOWN: *down = true;  return B_LEFT;
    case WM_LBUTTONUP:   *down = false; return B_LEFT;
    case WM_RBUTTONDOWN: *down = true;  return B_RIGHT;
    case WM_RBUTTONUP:   *down = false; return B_RIGHT;
    case WM_MBUTTONDOWN: *down = true;  return B_MIDDLE;
    case WM_MBUTTONUP:   *down = false; return B_MIDDLE;
    case WM_XBUTTONDOWN: *down = true;  return HIWORD(mouseData) == XBUTTON1 ? B_X1 : B_X2;
    case WM_XBUTTONUP:   *down = false; return HIWORD(mouseData) == XBUTTON1 ? B_X1 : B_X2;
    default: return B_NONE;
    }
}

static void SetScrollCursor(int shape) {
    if (shape != 0 && !g_hc.cursorChange) return;
    if (shape == g_cursorShape) return;
    g_cursorShape = shape;
    PostMessageW(g_hwnd, WM_SETSCROLLCURSOR, (WPARAM)shape, 0);
}

static void ResetInertia();
static void StartScrollMode(Btn owner, POINT pt) {
    ResetInertia();
    g_scrolling = true;
    g_scrollStarted = false;
    g_scrollOwner = owner;
    g_anchor = pt;
    g_accV = g_accH = 0;
    g_lockAxis = 0;
}
static void EndScrollMode() {
    g_scrolling = false;
    g_scrollStarted = false;
    g_scrollOwner = B_NONE;
    SetScrollCursor(0);
}

// W10Wheel と同じ加速テーブル（移動量のしきい値 → 倍率）
static const int kAccelThr[12] = { 1, 2, 3, 5, 7, 10, 14, 20, 30, 43, 63, 91 };
static const double kAccelMul[5][12] = {
    { 1.0, 1.3, 1.7, 2.0, 2.4, 2.7, 3.1, 3.4, 3.8, 4.1, 4.5, 4.8 },  // M5
    { 1.2, 1.6, 2.0, 2.4, 2.8, 3.3, 3.7, 4.1, 4.5, 4.9, 5.4, 5.8 },  // M6
    { 1.4, 1.8, 2.3, 2.8, 3.3, 3.8, 4.3, 4.8, 5.3, 5.8, 6.3, 6.7 },  // M7
    { 1.6, 2.1, 2.7, 3.2, 3.8, 4.4, 4.9, 5.5, 6.0, 6.6, 7.2, 7.7 },  // M8
    { 1.8, 2.4, 3.0, 3.6, 4.3, 4.9, 5.5, 6.2, 6.8, 7.4, 8.1, 8.7 },  // M9
};
static double Curve(int d) {
    if (g_hc.accel <= 0) return d;
    const double* m = kAccelMul[std::min(4, g_hc.accel - 1)];
    int a = std::abs(d);
    int i = 0;
    while (i < 11 && kAccelThr[i] < a) i++;
    return d * m[i];
}

static void SendAcc(double* acc, bool horiz) {
    int send = g_hc.realWheel ? (int)(*acc / WHEEL_DELTA) * WHEEL_DELTA : (int)*acc;
    if (send != 0) { *acc -= send; SendWheel(send, horiz); }
}

// ---------------------------------------------------------------------------
// 慣性スクロール：離す直前の速さで少し滑らせ、だんだん止める
// ---------------------------------------------------------------------------
struct InSample { DWORD t; double v, h; };
static InSample g_inS[32];
static int g_inN = 0, g_inHead = 0;
static double g_inV = 0, g_inH = 0;        // 1ms あたりのホイール量
static double g_inAccV = 0, g_inAccH = 0;
static DWORD g_inLast = 0, g_inStart = 0;
static bool g_inRunning = false;

static void AddInertiaSample(double v, double h) {
    g_inS[g_inHead] = { GetTickCount(), v, h };
    g_inHead = (g_inHead + 1) % 32;
    if (g_inN < 32) g_inN++;
}
static void StopInertia() {
    if (g_inRunning) KillTimer(g_hookWnd, TIMER_INERTIA);
    g_inRunning = false;
    g_inV = g_inH = 0;
}
static void ResetInertia() { StopInertia(); g_inN = 0; }
static void StartInertia() {
    int n = g_inN;
    g_inN = 0;
    if (!g_hc.inertia || n == 0) return;
    DWORD now = GetTickCount();
    int last = (g_inHead + 31) % 32;
    if (now - g_inS[last].t > 60) return;           // 止めてから離した → 滑らせない
    double sv = 0, sh = 0;
    DWORD oldest = now;
    for (int k = 0; k < n; k++) {
        const InSample& x = g_inS[(g_inHead + 31 - k) % 32];
        if (now - x.t > 100) break;
        sv += x.v; sh += x.h; oldest = x.t;
    }
    double span = std::max(16.0, (double)(now - oldest));
    g_inV = sv / span;
    g_inH = sh / span;
    if (std::abs(g_inV) * 16 < 2 && std::abs(g_inH) * 16 < 2) { g_inV = g_inH = 0; return; }
    g_inAccV = g_inAccH = 0;
    g_inLast = g_inStart = now;
    g_inRunning = true;
    SetTimer(g_hookWnd, TIMER_INERTIA, 16, nullptr);
}
static void InertiaTick() {
    if (!g_inRunning) { KillTimer(g_hookWnd, TIMER_INERTIA); return; }
    DWORD now = GetTickCount();
    double dt = std::max(1.0, (double)(now - g_inLast));
    g_inLast = now;
    g_inAccV += g_inV * dt;
    g_inAccH += g_inH * dt;
    SendAcc(&g_inAccV, false);
    SendAcc(&g_inAccH, true);
    double f = std::pow(0.80 + 0.017 * std::min(10, std::max(1, g_hc.inertiaLen)), dt / 16.0);
    g_inV *= f; g_inH *= f;
    if ((std::abs(g_inV) * 16 < 0.5 && std::abs(g_inH) * 16 < 0.5) || now - g_inStart > 5000) StopInertia();
}

static void DoScroll(int dx, int dy) {
    const Config& c = g_hc;
    if (c.swapAxis) std::swap(dx, dy);
    int ax = std::abs(dx), ay = std::abs(dy);
    bool doV = ay > c.vThreshold;
    bool doH = c.horizontal && ax > c.hThreshold;
    if (c.axisLock) {
        if (!g_lockAxis && (doV || doH)) g_lockAxis = (doH && (!doV || ax > ay)) ? 2 : 1;
        if (g_lockAxis == 1) doH = false;
        if (g_lockAxis == 2) doV = false;
    }
    double incV = 0, incH = 0;
    if (doV) {
        // 下へ動かす → 下へスクロール（WHEEL は負）
        incV = -Curve(dy) * (c.vSpeed / 100.0) * (c.reverse ? -1 : 1);
        g_accV += incV;
        SendAcc(&g_accV, false);
    }
    if (doH) {
        // 右へ動かす → 右へスクロール（HWHEEL は正）
        incH = Curve(dx) * (c.hSpeed / 100.0) * (c.hReverse ? -1 : 1);
        g_accH += incH;
        SendAcc(&g_accH, true);
    }
    if (doV || doH) AddInertiaSample(incV, incH);
    if (doV || doH) SetScrollCursor(doH && !doV ? 2 : doV && !doH ? 1 : 3);
}

// 設定からの問い合わせ
static bool HasChord(int held) {
    for (auto& ch : g_hc.chords) if (ch.held == held && ch.action.type != A_PASS) return true;
    return false;
}
static const Action* FindChord(int held, int pressed) {
    for (auto& ch : g_hc.chords)
        if (ch.held == held && ch.pressed == pressed && ch.action.type != A_PASS) return &ch.action;
    return nullptr;
}
static const GestureDef* FindGesture(const std::wstring& p) {
    for (auto& g : g_hc.gestures) if (g.pattern == p && g.action.type != A_PASS) return &g;
    return nullptr;
}
static Btn TriggerBtn() {
    switch (g_hc.trigger) {
    case T_RIGHT: return B_RIGHT;
    case T_MIDDLE: return B_MIDDLE;
    case T_X1: return B_X1;
    case T_X2: return B_X2;
    case T_LEFT: return B_LEFT;
    default: return B_NONE;
    }
}

static bool AnyPending() {
    for (int b = 0; b < B_COUNT; b++) if (g_state[b] == S_PENDING) return true;
    return false;
}
static bool AnyWaiting() {
    for (int b = 0; b < B_COUNT; b++) if (g_state[b] == S_PENDING || g_state[b] == S_DBLWAIT) return true;
    return false;
}
// 長押し・2回押しの判定をするボタンか（スクロール・ジェスチャー・同時押しに使うボタンは対象外）
static bool HasTap(int b) {
    const Config& c = g_hc;
    if (c.longPress[b].type == A_PASS && c.dblClick[b].type == A_PASS) return false;
    if (TriggerBtn() == b || c.gestureBtn == b || HasChord(b)) return false;
    if (c.trigger == T_LR && (b == B_LEFT || b == B_RIGHT)) return false;
    return true;
}
static bool AllIdle() {
    if (g_scrolling) return false;
    for (int b = 0; b < B_COUNT; b++) if (g_state[b] != S_IDLE) return false;
    return true;
}

// 保留していた down を「普通の押下」として確定する。
// 押した位置で down を送り、そのあと finalPt へカーソルを戻す。
// alsoUp=true なら続けて up も送る（=クリック）。
static void FlushPending(Btn b, POINT finalPt, bool alsoUp) {
    if (g_state[b] != S_PENDING) return;
    const Action& a = g_hc.button[b];
    POINT p0 = g_pendPt[b];
    bool moved = (finalPt.x != p0.x || finalPt.y != p0.y);
    g_pendLR[b] = false;
    g_pendTap[b] = false;
    if (moved) SendMoveAbs(p0);
    if (a.type == A_PASS) {
        SendButton(b, true);
        if (alsoUp) SendButton(b, false);
        g_state[b] = alsoUp ? S_IDLE : S_PASSED;
    } else if (a.type == A_DISABLE) {
        g_state[b] = alsoUp ? S_IDLE : S_MAPPED;
    } else {
        RunActionDown(a, p0);
        if (alsoUp) RunActionUp(a);
        g_state[b] = alsoUp ? S_IDLE : S_MAPPED;
    }
    if (moved) SendMoveAbs(finalPt);
    if (!AnyWaiting()) KillTimer(g_hookWnd, TIMER_PENDING);
}

static void StartPending(Btn b, POINT pt, bool lr, bool tap = false) {
    g_state[b] = S_PENDING;
    g_pendTap[b] = tap;
    g_pendTime[b] = GetTickCount();
    g_pendPt[b] = pt;
    g_pendLR[b] = lr;
    SetTimer(g_hookWnd, TIMER_PENDING, 15, nullptr);
}

// 2回目のクリックが来なかった → 保留していた1回目を普通のクリックとして送る
static void ResolveDblWait(Btn b, POINT cur) {
    if (g_state[b] != S_DBLWAIT) return;
    g_state[b] = S_PENDING;
    g_pendPt[b] = g_dblPt[b];
    FlushPending(b, cur, true);
}

// タイマーから呼ばれる：時間切れの保留を確定
static void CheckPendingTimeouts() {
    DWORD now = GetTickCount();
    POINT cur; GetCursorPos(&cur);
    for (int b = 0; b < B_COUNT; b++) {
        if (g_state[b] == S_DBLWAIT) {
            if ((int)(now - g_dblTime[b]) >= g_hc.dblMs) ResolveDblWait((Btn)b, cur);   // 2回目は来なかった
            continue;
        }
        if (g_state[b] != S_PENDING) continue;
        int elapsed = (int)(now - g_pendTime[b]);
        if (g_pendTap[b]) {
            if (elapsed < g_hc.longMs) continue;
            const Action& la = g_hc.longPress[b];
            if (la.type != A_PASS) {       // 長押し
                g_pendTap[b] = false;
                g_state[b] = S_SWALLOW;
                if (la.type != A_DISABLE) { RunActionDown(la, g_pendPt[b]); RunActionUp(la); }
            } else FlushPending((Btn)b, cur, false);   // 長押しの割り当てがなければ普通の押下
            continue;
        }
        bool chordWait = HasChord(b);
        if (g_pendLR[b] && elapsed >= g_hc.lrTimeout) {
            g_pendLR[b] = false;               // 左右同時押しの受付は終了
        }
        if (g_pendLR[b]) continue;
        if (!chordWait || elapsed >= g_hc.chordDelay) FlushPending((Btn)b, cur, false);
    }
    if (!AnyWaiting()) KillTimer(g_hookWnd, TIMER_PENDING);
}

static void ReleaseAllForDisable() {
    // 無効化した瞬間に保留中のものは確定させておく
    POINT cur; GetCursorPos(&cur);
    for (int b = 0; b < B_COUNT; b++) {
        if (g_state[b] == S_DBLWAIT) ResolveDblWait((Btn)b, cur);
        if (g_state[b] == S_PENDING) FlushPending((Btn)b, cur, false);
    }
    StopInertia();
}

static LRESULT HandleButtonDown(Btn b, POINT pt) {
    const Config& c = g_hc;
    StopInertia();

    // --- 2回押し ---
    if (g_state[b] == S_DBLWAIT) {
        const Action& a = c.dblClick[b];
        g_state[b] = S_SWALLOW;
        if (a.type != A_DISABLE && a.type != A_PASS) { RunActionDown(a, pt); RunActionUp(a); }
        if (!AnyWaiting()) KillTimer(g_hookWnd, TIMER_PENDING);
        return 1;
    }

    // --- 保留中/押さえ中のボタンとの組み合わせ ---
    for (int h = 0; h < B_COUNT; h++) {
        if (h == b) continue;
        if (g_state[h] == S_DBLWAIT) ResolveDblWait((Btn)h, pt);
        BtnState hs = g_state[h];
        if (hs != S_PENDING && hs != S_CHORDED) continue;

        // 左右同時押しスクロール
        if (hs == S_PENDING && c.trigger == T_LR && g_pendLR[h] &&
            ((h == B_LEFT && b == B_RIGHT) || (h == B_RIGHT && b == B_LEFT))) {
            g_state[h] = S_LRSCROLL;
            g_state[b] = S_LRSCROLL;
            g_pendLR[h] = false;
            StartScrollMode(b, g_pendPt[h]);
            if (!AnyWaiting()) KillTimer(g_hookWnd, TIMER_PENDING);
            return 1;
        }
        // 同時押しアクション
        if (const Action* pa = FindChord(h, b)) {
            const Action& a = *pa;
            g_state[h] = S_CHORDED;           // 押さえ側の up は握りつぶす（続けて何回でも使える）
            g_pendLR[h] = false;
            if (a.type != A_DISABLE) { RunActionDown(a, pt); RunActionUp(a); }
            g_state[b] = S_SWALLOW;
            if (!AnyWaiting()) KillTimer(g_hookWnd, TIMER_PENDING);
            return 1;
        }
        // 関係ないボタン → 保留を確定させてから通常処理
        if (hs == S_PENDING) FlushPending((Btn)h, pt, false);
    }

    // スクロール中に押された他のボタンはそのまま通す
    if (g_scrolling) { g_state[b] = S_PASSED; return 0; }

    // --- ジェスチャー ---
    if (c.gestureBtn == b && TriggerBtn() != b) {
        g_state[b] = S_GESTURE;
        g_gStart = g_gAnchor = g_gVirt = pt;
        g_gPattern.clear();
        g_gLastOsd.clear();
        return 1;
    }
    // --- スクロールトリガー ---
    if (TriggerBtn() == b) {
        g_state[b] = S_TRIGGER;
        StartScrollMode(b, pt);
        return 1;
    }
    // --- 同時押し / 左右同時押しの保留 ---
    bool lr = c.trigger == T_LR && (b == B_LEFT || b == B_RIGHT);
    if (HasChord(b) || lr) {
        StartPending(b, pt, lr);
        return 1;
    }
    // --- 長押し・2回押しの判定 ---
    if (HasTap(b)) {
        StartPending(b, pt, false, true);
        return 1;
    }
    // --- 通常の割り当て ---
    const Action& a = c.button[b];
    if (a.type == A_PASS) { g_state[b] = S_PASSED; return 0; }
    g_state[b] = S_MAPPED;
    if (a.type != A_DISABLE) RunActionDown(a, pt);
    return 1;
}

static LRESULT HandleButtonUp(Btn b, POINT pt) {
    BtnState st = g_state[b];
    switch (st) {
    case S_PENDING:
        if (g_pendTap[b] && g_hc.dblClick[b].type != A_PASS) {
            // 2回目を待つ（来なければタイマーで普通のクリックにする）
            g_pendTap[b] = false;
            g_state[b] = S_DBLWAIT;
            g_dblTime[b] = GetTickCount();
            g_dblPt[b] = g_pendPt[b];
            SetTimer(g_hookWnd, TIMER_PENDING, 15, nullptr);
            return 1;
        }
        // 同時押しにならずに離した → 普通のクリック
        FlushPending(b, pt, true);
        g_state[b] = S_IDLE;
        return 1;
    case S_TRIGGER: {
        bool started = g_scrollStarted;
        if (started) StartInertia();
        EndScrollMode();
        g_state[b] = S_IDLE;
        if (!started && g_hc.clickIfNoMove) RunActionClick(b, pt);
        return 1;
    }
    case S_GESTURE: {
        g_state[b] = S_IDLE;
        GestureOsd(L"");
        if (g_gPattern.empty()) { RunActionClick(b, pt); return 1; }   // 動かさなければ普通のクリック
        const GestureDef* g = FindGesture(g_gPattern);
        static int s_gl = 0;
        if (s_gl < 30) { s_gl++; Log(L"ジェスチャー %ls → %ls", g_gPattern.c_str(), g ? ActionDescription(g->action).c_str() : L"割り当てなし"); }
        if (g && g->action.type != A_DISABLE) { RunActionDown(g->action, g_gStart); RunActionUp(g->action); }
        g_gPattern.clear();
        return 1;
    }
    case S_LRSCROLL:
        if (g_scrollStarted) StartInertia();
        EndScrollMode();
        g_state[b] = S_IDLE;
        for (int o = 0; o < B_COUNT; o++)
            if (g_state[o] == S_LRSCROLL) g_state[o] = S_SWALLOW;  // もう片方の up も握りつぶす
        return 1;
    case S_MAPPED:
        g_state[b] = S_IDLE;
        RunActionUp(g_hc.button[b]);
        return 1;
    case S_CHORDED:
    case S_SWALLOW:
        g_state[b] = S_IDLE;
        return 1;
    case S_PASSED:
    case S_IDLE:
    default:
        g_state[b] = S_IDLE;
        return 0;
    }
}

// ---------------------------------------------------------------------------
// 画面の四隅：カーソルを角に当てて少し待つと動作を実行
// ---------------------------------------------------------------------------
static int   g_cornerCur = -1;
static DWORD g_cornerTime = 0;
static bool  g_cornerFired = false;
static RECT  g_monRc = {};
static HMONITOR g_monCache = nullptr;

static bool IsOuter(int x, int y) { return MonitorFromPoint(POINT{ x, y }, MONITOR_DEFAULTTONULL) == nullptr; }

// 0=左上 1=右上 2=左下 3=右下、角でなければ -1
static int CornerAt(POINT pt) {
    bool any = false;
    for (auto& a : g_hc.corner) if (a.type != A_PASS) any = true;
    if (!any) return -1;
    if (!g_monCache || pt.x < g_monRc.left || pt.x >= g_monRc.right || pt.y < g_monRc.top || pt.y >= g_monRc.bottom) {
        g_monCache = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(g_monCache, &mi)) { g_monCache = nullptr; return -1; }
        g_monRc = mi.rcMonitor;
    }
    const RECT& r = g_monRc;
    const int t = 1;
    bool L = pt.x <= r.left + t, R = pt.x >= r.right - 1 - t, T = pt.y <= r.top + t, B = pt.y >= r.bottom - 1 - t;
    if (!((L || R) && (T || B))) return -1;
    int i = (B ? 2 : 0) + (R ? 1 : 0);
    // 隣に別のモニターがある角は、角として扱わない
    int ox = R ? r.right + 2 : r.left - 3, oy = B ? r.bottom + 2 : r.top - 3;
    int cx = R ? r.right - 1 : r.left, cy = B ? r.bottom - 1 : r.top;
    if (!IsOuter(ox, cy) || !IsOuter(cx, oy)) return -1;
    return i;
}

static bool ForegroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetShellWindow() || fg == GetDesktopWindow()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman")) return false;
    RECT w; if (!GetWindowRect(fg, &w)) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& m = mi.rcMonitor;
    return w.left <= m.left && w.top <= m.top && w.right >= m.right && w.bottom >= m.bottom;
}

static void CornerCheck(POINT pt) {
    int i = AllIdle() ? CornerAt(pt) : -1;
    if (i == g_cornerCur) return;
    g_cornerCur = i;
    g_cornerFired = false;
    if (i < 0) { KillTimer(g_hookWnd, TIMER_CORNER); return; }
    g_cornerTime = GetTickCount();
    SetTimer(g_hookWnd, TIMER_CORNER, 30, nullptr);
}
static void CornerTick() {
    POINT cur; GetCursorPos(&cur);
    if (g_cornerCur < 0 || g_cornerFired || CornerAt(cur) != g_cornerCur || !AllIdle() || !g_hc.enabled) {
        if (g_cornerCur >= 0 && CornerAt(cur) != g_cornerCur) g_cornerCur = -1;
        KillTimer(g_hookWnd, TIMER_CORNER);
        return;
    }
    if ((int)(GetTickCount() - g_cornerTime) < g_hc.cornerDelay) return;
    g_cornerFired = true;
    KillTimer(g_hookWnd, TIMER_CORNER);
    if (g_hc.cornerSkipFull && ForegroundIsFullscreen()) return;
    const Action& a = g_hc.corner[g_cornerCur];
    static int s_cl = 0;
    if (s_cl < 20) { s_cl++; Log(L"画面の%ls → %ls", kCornerLabels[g_cornerCur], ActionDescription(a).c_str()); }
    if (a.type != A_DISABLE && a.type != A_PASS) { RunActionDown(a, cur); RunActionUp(a); }
}

static LRESULT CALLBACK LowLevelMouseProcBody(int nCode, WPARAM wp, LPARAM lp);

// 処理時間を計測するラッパー（遅いとWindowsにフックを外されるため、ログで確認できるようにする）
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_lastTick = GetTickCount();
    LRESULT r = LowLevelMouseProcBody(nCode, wp, lp);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    g_calls = g_calls + 1;
    if (ms > 10.0) g_slow = g_slow + 1;
    if (ms > g_maxMs) g_maxMs = ms;
    return r;
}

static LRESULT CALLBACK LowLevelMouseProcBody(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode != HC_ACTION) return CallNextHookEx(g_hook, nCode, wp, lp);
    const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lp;
    if (ms->dwExtraInfo == kMagic) return CallNextHookEx(g_hook, nCode, wp, lp);  // 自分の送信

    // 設定画面が開いていれば、押されたボタンを知らせる（ボタン確認用）
    if (HWND mon = g_monitor) {
        bool dn = false;
        Btn mb = MsgToBtn(wp, ms->mouseData, &dn);
        if (mb != B_NONE) PostMessageW(mon, WM_INPUTMON, mb, dn ? 1 : 0);
        else if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) {
            short d = (short)HIWORD(ms->mouseData);
            int code = wp == WM_MOUSEWHEEL ? (d > 0 ? 10 : 11) : (d < 0 ? 12 : 13);
            PostMessageW(mon, WM_INPUTMON, code, 1);
        }
    }
    if (!g_hc.enabled && AllIdle()) return CallNextHookEx(g_hook, nCode, wp, lp);

    POINT pt = ms->pt;

    if (wp == WM_MOUSEMOVE) {
        // タッチパッドの辺を操作中はカーソルを止める（報告が途切れたら止めない）
        if (g_tpSuppress && GetTickCount() - g_tpLastTick < 300) return 1;
        if (g_hc.gestureBtn != B_NONE && g_state[g_hc.gestureBtn] == S_GESTURE) {
            if (g_hc.gestureFreeze) {
                // カーソルは開始位置に止めたまま、動いた量だけを積み上げて判定する
                g_gVirt.x += pt.x - g_gStart.x;
                g_gVirt.y += pt.y - g_gStart.y;
                GestureMove(g_gVirt);
                return 1;
            }
            GestureMove(pt);
            return CallNextHookEx(g_hook, nCode, wp, lp);
        }
        if (g_scrolling) {
            int dx = pt.x - g_anchor.x, dy = pt.y - g_anchor.y;
            if (!g_scrollStarted) {
                if (std::abs(dx) + std::abs(dy) <= g_hc.dragThreshold) return 1;  // まだクリック扱い
                g_scrollStarted = true;
                static int s_sc = 0;
                if (s_sc < 20) { s_sc++; Log(L"ボールスクロール開始（%ls ボタン）", g_scrollOwner >= 0 ? kBtnNames[g_scrollOwner] : L"?"); }
            }
            DoScroll(dx, dy);
            return 1;   // カーソルは動かさない
        }
        CornerCheck(pt);
        if (AnyPending()) {
            bool flushed = false;
            for (int b = 0; b < B_COUNT; b++) {
                if (g_state[b] != S_PENDING) continue;
                int d = std::abs(pt.x - g_pendPt[b].x) + std::abs(pt.y - g_pendPt[b].y);
                int lim = HasChord(b) ? g_hc.chordMove : g_hc.dragThreshold;
                if (d > lim) { FlushPending((Btn)b, pt, false); flushed = true; }
            }
            if (flushed) return 1;   // この移動は FlushPending 内で送り直し済み
        }
        return CallNextHookEx(g_hook, nCode, wp, lp);
    }

    bool down = false;
    Btn b = MsgToBtn(wp, ms->mouseData, &down);
    if (b == B_NONE) {   // ホイール等
        if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) StopInertia();   // ホイールを回したら慣性は止める
        return CallNextHookEx(g_hook, nCode, wp, lp);
    }

    LRESULT r;
    if (!g_hc.enabled) r = down ? 0 : HandleButtonUp(b, pt);       // 無効中は up の後始末だけ
    else r = down ? HandleButtonDown(b, pt) : HandleButtonUp(b, pt);
    static int s_logged = 0;   // 起動直後のボタン操作だけ記録（原因調査用）
    if (s_logged < 60) {
        s_logged++;
        Log(L"ボタン %ls %ls → %ls（状態=%d, スクロール中=%d, 注入=%d）", kBtnNames[b], down ? L"押" : L"離",
            r ? L"PointerKitが処理" : L"そのまま通過", (int)g_state[b], g_scrolling ? 1 : 0,
            (ms->flags & LLMHF_INJECTED) ? 1 : 0);
    }
    return r ? r : CallNextHookEx(g_hook, nCode, wp, lp);
}

static HHOOK g_kbHook = nullptr;
static LRESULT CALLBACK KbHookProc(int n, WPARAM wp, LPARAM lp) { return UsKeyboardProc(g_kbHook, n, wp, lp); }

static bool InstallHookInThread() {
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHookProc, g_hinst, 0);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    QueryPerformanceFrequency(&g_qpf);
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_hinst, 0);
    g_lastTick = GetTickCount();
    Log(L"フック登録: %s (err=%lu)", g_hook ? L"成功" : L"失敗", g_hook ? 0UL : GetLastError());
    return g_hook != nullptr;
}

// 設定変更・無効化のときに、押しっぱなし扱いのものを片付ける
static void ResetHookStateInThread() {
    ReleaseAllForDisable();
    EndScrollMode();
    for (int b = 0; b < B_COUNT; b++)
        if (g_state[b] == S_TRIGGER || g_state[b] == S_LRSCROLL || g_state[b] == S_GESTURE) g_state[b] = S_SWALLOW;
    GestureOsd(L"");
}

static LRESULT CALLBACK HookWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_PENDING) CheckPendingTimeouts();
        else if (wp == TIMER_INERTIA) InertiaTick();
        else if (wp == TIMER_CORNER) CornerTick();
        return 0;
    case WMH_SETCONFIG: {
        Config* c = (Config*)lp;
        ResetHookStateInThread();
        g_hc = *c;
        delete c;
        return 0;
    }
    case WMH_REINSTALL:
        ResetHookStateInThread();
        InstallHookInThread();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HANDLE g_hookReady = nullptr;
static bool g_hookOk = false;

static DWORD WINAPI HookThread(LPVOID) {
    // マウス操作の遅延を防ぐため、このスレッドは優先度を上げる
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = HookWndProc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"PointerKitHookWnd";
    RegisterClassW(&wc);
    g_hookWnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinst, nullptr);
    g_hookOk = InstallHookInThread();
    SetEvent(g_hookReady);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    g_kbHook = nullptr;
    return 0;
}

static HANDLE g_hookThread = nullptr;
static DWORD g_hookThreadId = 0;

// 設定をフック用スレッドに渡す
void SetHookConfig(const Config& c) {
    if (!g_hookWnd) { g_hc = c; return; }
    PostMessageW(g_hookWnd, WMH_SETCONFIG, 0, (LPARAM)new Config(c));
}

bool InstallHook() {
    if (g_hookThread) {               // 復帰時などの再登録
        DWORD_PTR res;
        if (SendMessageTimeoutW(g_hookWnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 500, &res)) {
            PostMessageW(g_hookWnd, WMH_REINSTALL, 0, 0);
            return true;
        }
        // フック用スレッドが固まっている → 見捨てて新しく作り直す
        Log(L"フック用スレッドが応答しないため作り直します");
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
        g_hook = nullptr;
        g_hookWnd = nullptr;
        for (int b = 0; b < B_COUNT; b++) g_state[b] = S_IDLE;
        g_scrolling = false; g_scrollStarted = false;
    }
    g_hc = g_cfg;
    StartSender();
    if (!g_hookReady) g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(g_hookReady);
    g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
    WaitForSingleObject(g_hookReady, 5000);
    return g_hookOk;
}
void RemoveHook() {
    if (!g_hookThread) return;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hookThread, 2000);
    CloseHandle(g_hookThread);
    g_hookThread = nullptr;
}

// ---------------------------------------------------------------------------
// カーソル形状（スクロール中だけ変える）
// ---------------------------------------------------------------------------
void ApplyCursor(int shape) {
    if (shape == 0) { SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); return; }
    LPCWSTR id = shape == 1 ? IDC_SIZENS : shape == 2 ? IDC_SIZEWE : IDC_SIZEALL;
    const DWORD targets[] = { 32512 /*NORMAL*/, 32513 /*IBEAM*/, 32649 /*HAND*/ };
    for (DWORD t : targets) {
        HCURSOR cur = CopyCursor(LoadCursorW(nullptr, id));
        if (cur) SetSystemCursor(cur, t);
    }
}
