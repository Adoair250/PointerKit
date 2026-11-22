// PointerKit 共通ヘッダ
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

#define POINTERKIT_VERSION L"2.3.0"

// ---------------------------------------------------------------------------
// ボタン・アクション
// ---------------------------------------------------------------------------
enum Btn { B_NONE = -1, B_LEFT = 0, B_RIGHT, B_MIDDLE, B_X1, B_X2, B_COUNT };
extern const wchar_t* kBtnNames[B_COUNT];   // INI 用 (Left, Right, ...)
extern const wchar_t* kBtnLabels[B_COUNT];  // 表示用 (左ボタン, ...)

enum ActType {
    A_PASS, A_DISABLE, A_KEYS,
    A_CLICK_LEFT, A_CLICK_RIGHT, A_CLICK_MIDDLE, A_BACK, A_FORWARD,
    A_MINIMIZE, A_MAXIMIZE, A_CLOSE,
    A_WHEELUP, A_WHEELDOWN, A_WHEELLEFT, A_WHEELRIGHT,
    A_TOGGLE,
    A_COUNT
};
extern const wchar_t* kActNames[A_COUNT];   // INI 用
extern const wchar_t* kActLabels[A_COUNT];  // 表示用

struct Action {
    ActType type = A_PASS;
    std::wstring keys;            // A_KEYS のとき "Ctrl+W" 形式
};

struct ChordDef {
    Btn held = B_NONE;            // 押さえているボタン
    Btn pressed = B_NONE;         // 押したボタン
    Action action;
};

// スクロールのトリガー
enum Trigger { T_NONE, T_RIGHT, T_MIDDLE, T_X1, T_X2, T_LEFT, T_LR, T_COUNT };
extern const wchar_t* kTrigNames[T_COUNT];
extern const wchar_t* kTrigLabels[T_COUNT];

static const int kMaxChords = 6;
static const int kAccelCount = 6;   // 0=なし, 1..5 = M5..M9
extern const wchar_t* kAccelLabels[kAccelCount];

// ジェスチャー：動きは U/D/L/R の並び（例 "DR" = ↓→）
struct GestureDef {
    std::wstring pattern;
    Action action;
};
static const int kMaxGestures = 8;
extern const wchar_t* kGesturePatterns[];   // 選べる動きの一覧
extern const int kGesturePatternCount;
std::wstring GestureArrows(const std::wstring& pattern);   // "DR" → "↓→"
std::wstring ActionDescription(const Action& a);           // 表示用の説明

// タッチパッドの辺に割り当てる機能
enum EdgeAct { E_NONE, E_VOLUME, E_BRIGHTNESS, E_VSCROLL, E_HSCROLL, E_ZOOM, E_TAB, E_SEEK, E_TRACK, E_COUNT };
extern const wchar_t* kEdgeNames[4];       // Left, Right, Top, Bottom
extern const wchar_t* kEdgeLabels[4];
extern const wchar_t* kEdgeActNames[E_COUNT];
extern const wchar_t* kEdgeActLabels[E_COUNT];

struct Config {
    bool enabled = true;
    Action button[B_COUNT];
    std::vector<ChordDef> chords;
    int chordDelay = 750;      // ms
    int chordMove = 5;         // px

    Trigger trigger = T_RIGHT;
    bool clickIfNoMove = true; // 動かさず離したら通常クリック
    int  dragThreshold = 5;    // スクロール開始までの移動量 px
    int  lrTimeout = 200;      // 左右同時押しの受付時間 ms
    int  vSpeed = 100;         // 縦速度 %（100 = W10Wheel と同等）
    int  hSpeed = 100;         // 横速度 %
    int  accel = 1;            // 0=なし 1..5 = M5..M9
    bool reverse = false;
    bool hReverse = false;
    bool horizontal = true;
    bool swapAxis = false;
    bool axisLock = false;     // スクロール中は最初に動かした方向に固定
    int  vThreshold = 0;       // この px を超えた縦移動だけ縦スクロール
    int  hThreshold = 10;      // この px を超えた横移動だけ横スクロール
    bool realWheel = false;    // 120 単位でのみ送信
    bool cursorChange = false; // スクロール中にカーソル形状を変える

    // マウスジェスチャー
    Btn  gestureBtn = B_NONE;  // ジェスチャーに使うボタン（B_NONE=使わない）
    int  gestureDist = 30;     // 1つの動きと認識する距離 px
    bool gestureShow = true;   // 動きと動作名を画面に表示
    bool gestureFreeze = true; // ジェスチャー中はカーソルを動かさない
    std::vector<GestureDef> gestures;

    // タッチパッドの辺（左・右・上・下）
    bool tpEnabled = true;
    int  tpEdgeWidth[4] = { 8, 8, 8, 8 };  // 辺とみなす幅（タッチパッドの何%か）左・右・上・下
    int  tpStep = 5;           // 1回分の操作に必要な移動量（%）
    bool tpFreeze = true;      // 辺を操作中はカーソルを動かさない
    EdgeAct tpAct[4] = { E_NONE, E_VOLUME, E_NONE, E_NONE };

    // 長押し・2回押し
    Action longPress[B_COUNT];
    Action dblClick[B_COUNT];
    int  longMs = 500;         // 長押しと判断する時間 ms
    int  dblMs = 300;          // 2回押しと判断する間隔 ms

    // 慣性スクロール
    bool inertia = false;
    int  inertiaLen = 5;       // 余韻の長さ 1..10

    // 画面の四隅（左上・右上・左下・右下）
    Action corner[4];
    int  cornerDelay = 300;    // 角に当ててから実行するまで ms
    bool cornerSkipFull = true; // 全画面のアプリが前面にあるときは使わない

    // 設定画面の色 0=Windows に合わせる 1=ライト 2=ダーク
    int  theme = 0;

    // US配列キーボードの変換 0=使わない 1=登録したキーボードをつないでいる間 2=常に
    int  usMode = 0;
    std::vector<std::wstring> usKeyboards;   // US配列として登録したキーボード（VID_xxxx&PID_xxxx など）
    std::wstring usHotkey;                   // 手動で切り替えるショートカット
    bool usCaps = true;                      // CapsLock を US 配列と同じ働きにする
};
extern const wchar_t* kCornerNames[4];
extern const wchar_t* kCornerLabels[4];

// ---------------------------------------------------------------------------
// config.cpp
// ---------------------------------------------------------------------------
extern Config g_cfg;
extern std::wstring g_iniPath;
void InitConfigPath();
void LoadConfig();
void ResetGesturesToDefault(Config& c);
void SaveConfig(const Config& c);
FILETIME GetIniTime();
extern FILETIME g_iniTime;
std::vector<WORD> ParseKeys(const std::wstring& s);
std::wstring VkName(WORD vk);
Btn ParseBtn(const std::wstring& s);
bool ExportConfig(const Config& c, const std::wstring& path);
bool ImportConfig(const std::wstring& path, Config& out);

// ---------------------------------------------------------------------------
// hook.cpp
// ---------------------------------------------------------------------------
bool InstallHook();              // 初回はフック用スレッドを起動、2回目以降は再登録
void RemoveHook();
void SetHookConfig(const Config& c);
void SetInputMonitor(HWND h);     // 押されたボタンを通知する先（設定画面）。nullptr で停止
static const UINT WM_TPOSD = WM_APP + 22;     // lParam: std::wstring*（少し表示して自動で消す）
static const UINT WM_TPMON = WM_APP + 23;     // 設定画面へ：wParam=x,y(千分率) lParam=指の数,辺+1
static const UINT WM_TPCONFIG = WM_APP + 24;  // タッチパッド用スレッドへ設定を渡す
static const UINT_PTR TIMER_OSDHIDE = 4;
static const UINT WM_GESTUREOSD = WM_APP + 21;  // lParam: std::wstring*（空文字で非表示）
static const UINT WM_INPUTMON = WM_APP + 20;  // wParam: 0-4=ボタン 10-13=ホイール上下左右, lParam: 1=押した 0=離した
void ApplyCursor(int shape);
// 診断用
DWORD HookLastTick();              // 最後にフックが呼ばれた時刻
void  HookStats(unsigned* calls, unsigned* slow, double* maxMs);  // 取得するとリセット
void  Log(const wchar_t* fmt, ...);
std::wstring LogPath();

// ---------------------------------------------------------------------------
// main.cpp
// ---------------------------------------------------------------------------
extern HWND g_hwnd;
extern HINSTANCE g_hinst;
static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_SETSCROLLCURSOR = WM_APP + 2;
static const UINT WM_CONFIGCHANGED = WM_APP + 3;
static const UINT_PTR TIMER_PENDING = 1;
static const UINT_PTR TIMER_CONFIGWATCH = 2;
enum MenuId { ID_ENABLE = 100, ID_SETTINGS, ID_EDIT, ID_STARTUP, ID_EXIT, ID_LOG, ID_USTOGGLE };
static const UINT_PTR TIMER_USCHECK = 8, TIMER_USDEVCHANGE = 9;
static const UINT_PTR TIMER_WATCHDOG = 3;
bool IsStartup();
void SetStartup(bool on);
void ToggleEnabled();
void ApplyNewConfig(const Config& c);   // GUI から

// ---------------------------------------------------------------------------
// gui.cpp
// ---------------------------------------------------------------------------
void ShowSettings();

// ---------------------------------------------------------------------------
// keyboard.cpp（US配列キーボードの変換）
// ---------------------------------------------------------------------------
static const ULONG_PTR kInputMagic = 0x4F4E454DUL;   // 自分が送った入力の目印
void QueueInputExt(const INPUT* in, size_t n);       // hook.cpp：送信用スレッド経由で SendInput
struct KbInfo { std::wstring id, name; };
std::vector<KbInfo> ListKeyboards(bool names = true);
LRESULT UsKeyboardProc(HHOOK hook, int n, WPARAM wp, LPARAM lp);
bool UsActive();
void UpdateUsState();
void ToggleUs();
void RegisterUsHotkey();

// ---------------------------------------------------------------------------
// touchpad.cpp
// ---------------------------------------------------------------------------
extern volatile bool g_tpSuppress;
extern volatile DWORD g_tpLastTick;
void StartTouchpad();
void SetTouchpadConfig(const Config& c);
void SetTouchpadMonitor(HWND h);
int  TouchpadCount();
